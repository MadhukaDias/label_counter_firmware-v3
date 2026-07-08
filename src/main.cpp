#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>

#include "config.h"
#include "display_mgr.h"
#include "imu_detector.h"
#include "cfg_manager.h"
#include "mqtt_mgr.h"
#include "web_server_mgr.h"

// ── Globals ───────────────────────────────────────────────────────────────────
static AppConfig appCfg;

static uint32_t lastImuMs     = 0;
static uint32_t lastMqttMs    = 0;
static uint32_t lastDisplayMs = 0;

// Button state
static bool     btnIncLast    = HIGH;
static bool     btnDecLast    = HIGH;
static uint32_t btnIncDownMs  = 0;
static uint32_t btnDecDownMs  = 0;
static bool     btnIncLong    = false;
static bool     btnDecLong    = false;
static uint32_t btnBothDownMs = 0;
static bool     btnBothLong   = false;

static bool     vibActive     = false;
static uint8_t  sewState      = 0;
static bool     wifiOk        = false;

#include <algorithm> // for std::sort
// ── Calibration ───────────────────────────────────────────────────────────────
enum CalibState { CALIB_IDLE, CALIB_SAMPLING, CALIB_NOISE_DONE, CALIB_LOCK_WAITING, CALIB_SEW_WAITING, CALIB_FINE_TUNE, CALIB_SEW_DONE, CALIB_SKIP_LOCK };
static CalibState calibState = CALIB_IDLE;
static uint8_t calibLockCount = 0;
static uint32_t calibLockPeaks[3] = {0, 0, 0};
static uint32_t calibCurrentLockPeak = 0;
static uint32_t calibLastLockEventMs = 0;
static bool calibInLockEvent = false;
#define CALIB_SAMPLES 150
static uint16_t calibIdx = 0;

// Phase 3 dynamic buffer
#define CALIB_SEW_SAMPLES 2000
static uint32_t* calibSewBuffer = nullptr;
static uint16_t calibSewIdx = 0;

// Phase 4 tracking
static uint32_t* attemptLowestPeaks = nullptr;
static uint32_t* attemptMedians = nullptr;
static uint32_t* attemptDurations = nullptr;
static uint8_t fineTuneCount = 0; // 0 for Phase 3, 1-4 for Phase 4
static uint32_t prevAttemptEndMs = 0;

// Local Buffer for calibration
uint32_t* calibBuffer = nullptr;
uint32_t calibWarningMs = 0;

void startCalibration() {
    if (!calibBuffer) calibBuffer = new uint32_t[CALIB_SAMPLES];
    calibState = CALIB_SAMPLING;
    calibIdx = 0;
    calibLockCount = 0;
    calibLockPeaks[0] = 0; calibLockPeaks[1] = 0; calibLockPeaks[2] = 0;
    calibCurrentLockPeak = 0;
    calibLastLockEventMs = 0;
    calibInLockEvent = false;
    Serial.println("[CALIB] Phase 1: Started 3-second noise sampling...");
    
    displayShowMessage("CALIBRATION", "MODE");
    delay(1000);

    if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) {
        mqttPublishEvent(1, appCfg.deviceId); // calib_start
    }
}

void getCalibStatus(int& state, int& lockCount, int& imuState, int& ftCount) {
    state = (int)calibState;
    lockCount = calibLockCount;
    imuState = imuGetState();
    ftCount = fineTuneCount;
}

void skipCalibPhase2() {
    if (calibState == CALIB_LOCK_WAITING || calibState == CALIB_SKIP_LOCK) {
        calibState = CALIB_SEW_WAITING;
        appCfg.lastLockPeak = 22000; // Default fallback for spike detection
        cfgSave(appCfg);
        Serial.println("[CALIB] Phase 2 Skipped. Lock peak set to 22000 fallback.");
        if (calibSewBuffer) delete[] calibSewBuffer;
        calibSewBuffer = new uint32_t[CALIB_SEW_SAMPLES];
        calibSewIdx = 0;
        
        if (attemptLowestPeaks) delete[] attemptLowestPeaks;
        if (attemptMedians) delete[] attemptMedians;
        if (attemptDurations) delete[] attemptDurations;
        attemptLowestPeaks = new uint32_t[5];
        attemptMedians = new uint32_t[5];
        attemptDurations = new uint32_t[5];
        fineTuneCount = 0;
        
        Serial.println("[CALIB] Phase 3: Waiting for 1st sewing attempt...");
    }
}

void abortCalibration() {
    if (calibState != CALIB_IDLE) {
        calibState = CALIB_IDLE;
        appCfg.lastCalibStatus = 2; // CANCELED
        cfgSave(appCfg);
        
        displayShowMessage("CALIBRATION", "ABORTED");
        delay(1000);
        
        // Cleanup arrays
        if (attemptLowestPeaks) { delete[] attemptLowestPeaks; attemptLowestPeaks = nullptr; }
        if (attemptMedians) { delete[] attemptMedians; attemptMedians = nullptr; }
        if (attemptDurations) { delete[] attemptDurations; attemptDurations = nullptr; }
        if (calibSewBuffer) { delete[] calibSewBuffer; calibSewBuffer = nullptr; }
        if (calibBuffer) { delete[] calibBuffer; calibBuffer = nullptr; }
        
        Serial.println("[CALIB] Calibration aborted by user.");
        displayShowMessage("Aborting", "Calibration...");
        lastDisplayMs = millis() + 1000; // Small pause to remain readable

        if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) {
            mqttPublishEvent(3, appCfg.deviceId); // calib_canceled
        }
    }
}

// ── WiFiManager ───────────────────────────────────────────────────────────────
static void startWiFi() {
    WiFiManager wm;
    wm.setConfigPortalTimeout(WIFI_TIMEOUT_S);

    wm.setAPCallback([](WiFiManager*) {
        String ip = WiFi.softAPIP().toString();
        displayShowPortal(WIFI_AP_NAME, ip.c_str());
        Serial.printf("[WiFi] AP: %s  IP: %s\n", WIFI_AP_NAME, ip.c_str());
    });

    displayShowConnecting("Saved network...");
    wifiOk = wm.autoConnect(WIFI_AP_NAME, WIFI_AP_PASS);

    if (wifiOk) {
        Serial.printf("[WiFi] Connected: %s  IP: %s\n",
                      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[WiFi] Portal timeout — running offline");
        displayShowError("WiFi timeout", "Running offline");
        delay(2000);
    }
}

// ── Buttons ───────────────────────────────────────────────────────────────────
static void handleButtons() {
    uint32_t now = millis();
    bool incNow  = digitalRead(PIN_BTN_INC);
    bool decNow  = digitalRead(PIN_BTN_DEC);

    // ── Dual button ──
    if (incNow == LOW && decNow == LOW) {
        if (btnBothDownMs == 0) btnBothDownMs = now;
        if (calibState == CALIB_IDLE) {
            if (!btnBothLong && (now - btnBothDownMs) >= LONG_PRESS_MS) {
                btnBothLong = true;
                btnIncLong = true; // Prevent individual triggers on release
                btnDecLong = true;
                Serial.println("[BTN] Dual Long Press → Start Calibration");
                startCalibration();
            }
        } else {
            if (!btnBothLong && (now - btnBothDownMs) >= 3000) {
                btnBothLong = true;
                btnIncLong = true;
                btnDecLong = true;
                Serial.println("[BTN] Dual Long Press (3s) → Abort Calibration");
                abortCalibration();
            }
        }
    } else {
        btnBothDownMs = 0;
        btnBothLong = false;
    }

    // ── INC button ──
    if (btnIncLast == HIGH && incNow == LOW) {
        btnIncDownMs = now;
        btnIncLong   = false;
    }
    if (incNow == LOW && !btnIncLong && (now - btnIncDownMs) >= LONG_PRESS_MS && btnBothDownMs == 0) {
        btnIncLong    = true;
        appCfg.count  = 0;
        cfgSaveCount(0);
        if (wifiOk && appCfg.mqttEnabled && mqttIsConnected() && calibState == CALIB_IDLE) mqttPublish(appCfg.count, appCfg.deviceId);
        Serial.println("[BTN] Long INC → reset");
    }
    if (btnIncLast == LOW && incNow == HIGH && !btnIncLong && (now - btnIncDownMs) >= DEBOUNCE_MS) {
        if (calibState == CALIB_SAMPLING) {
            // Ignore during Phase 1
        } else if (calibState == CALIB_LOCK_WAITING) {
            skipCalibPhase2();
        } else if (calibState == CALIB_SEW_WAITING || calibState == CALIB_FINE_TUNE) {
            calibState = CALIB_SEW_DONE; // Manual override/force
            Serial.println("[CALIB] Phase 3/4 manual force finish (INC).");
        } else {
            appCfg.count++;
            cfgSaveCount(appCfg.count);
            if (wifiOk && appCfg.mqttEnabled && mqttIsConnected() && calibState == CALIB_IDLE) mqttPublish(appCfg.count, appCfg.deviceId);
            Serial.printf("[BTN] +1 → %lu\n", (unsigned long)appCfg.count);
        }
    }

    // ── DEC button ──
    if (btnDecLast == HIGH && decNow == LOW) {
        btnDecDownMs = now;
        btnDecLong   = false;
    }
    if (decNow == LOW && !btnDecLong && (now - btnDecDownMs) >= LONG_PRESS_MS && btnBothDownMs == 0) {
        btnDecLong    = true;
        appCfg.count  = 0;
        cfgSaveCount(0);
        if (wifiOk && appCfg.mqttEnabled && mqttIsConnected() && calibState == CALIB_IDLE) mqttPublish(appCfg.count, appCfg.deviceId);
        Serial.println("[BTN] Long DEC → reset");
    }
    if (btnDecLast == LOW && decNow == HIGH && !btnDecLong && (now - btnDecDownMs) >= DEBOUNCE_MS) {
        if (calibState == CALIB_SAMPLING) {
            // Ignore during Phase 1
        } else if (calibState == CALIB_LOCK_WAITING) {
            skipCalibPhase2();
        } else if (calibState == CALIB_FINE_TUNE && fineTuneCount > 0) {
            fineTuneCount--;
            if ((now - prevAttemptEndMs) <= (appCfg.vib.silenceMs * 2.5)) {
                uint32_t start2 = imuGetLastVibStart();
                uint32_t newSil = (start2 > prevAttemptEndMs) ? (start2 - prevAttemptEndMs) + 250 : 250;
                if (newSil > 2000) newSil = 2000;
                appCfg.vib.silenceMs = newSil;
                cfgSave(appCfg);
                Serial.printf("[CALIB] Merge Attempts! New silence gap: %lu ms\n", newSil);
            } else {
                Serial.println("[CALIB] Too much time passed, stepped back without merging.");
            }
        } else if (calibState == CALIB_SEW_WAITING || calibState == CALIB_FINE_TUNE) {
            calibState = CALIB_SEW_DONE; // Manual override/force
            Serial.println("[CALIB] Phase 3/4 manual force finish (DEC).");
        } else {
            if (appCfg.count > 0) appCfg.count--;
            cfgSaveCount(appCfg.count);
            if (wifiOk && appCfg.mqttEnabled && mqttIsConnected() && calibState == CALIB_IDLE) mqttPublish(appCfg.count, appCfg.deviceId);
            Serial.printf("[BTN] -1 → %lu\n", (unsigned long)appCfg.count);
        }
    }

    btnIncLast = incNow;
    btnDecLast = decNow;
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[BOOT] Label Counter starting...");

    Wire.begin(PIN_SDA, PIN_SCL);

    pinMode(PIN_BTN_INC, INPUT_PULLUP);
    pinMode(PIN_BTN_DEC, INPUT_PULLUP);

    displayInit();
    cfgLoad(appCfg);

    // FIX: if NVS had a stale count from a previous session, show it on serial
    // so the user knows. Buttons now work reliably to correct it.
    Serial.printf("[BOOT] Restored count from NVS: %lu\n", (unsigned long)appCfg.count);

    imuInit();
    startWiFi();

    webServerInit(&appCfg);

    if (wifiOk) {
        mqttInit(appCfg.deviceId);
        String ip = WiFi.localIP().toString();
        displayShowCfgIP(ip.c_str());
        Serial.printf("[WEB] Portal: http://%s\n", ip.c_str());
        
        // Start OTA listener
        ArduinoOTA.setHostname(appCfg.deviceId);
        ArduinoOTA.begin();
    }

    Serial.println("[BOOT] Ready.");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    if (wifiOk) ArduinoOTA.handle();

    // ── IMU at fixed rate ──
    if (now - lastImuMs >= IMU_SAMPLE_MS) {
        lastImuMs = now;

        bool newVib  = false;
        bool counted = imuUpdate(appCfg.vib, appCfg.lastLockPeak, &newVib);
        vibActive    = newVib;
        uint32_t currentMag = imuGetMagnitude();

        if (calibState == CALIB_SAMPLING) {
            calibBuffer[calibIdx++] = currentMag;
            if (calibIdx >= CALIB_SAMPLES) {
                calibState = CALIB_NOISE_DONE;
            }
        } else if (calibState == CALIB_NOISE_DONE) {
            uint32_t sortedBuf[CALIB_SAMPLES];
            memcpy(sortedBuf, calibBuffer, CALIB_SAMPLES * sizeof(uint32_t));
            std::sort(sortedBuf, sortedBuf + CALIB_SAMPLES);
            uint32_t median = sortedBuf[CALIB_SAMPLES / 2];

            uint32_t spikeThreshold = median * 5;
            bool valid[CALIB_SAMPLES];
            for (int i = 0; i < CALIB_SAMPLES; i++) valid[i] = true;

            int i = 0;
            while (i < CALIB_SAMPLES) {
                if (calibBuffer[i] > spikeThreshold) {
                    int eventStart = i;
                    while (i < CALIB_SAMPLES && calibBuffer[i] > spikeThreshold) {
                        i++;
                    }
                    int eventEnd = i - 1;
                    int duration = (eventEnd - eventStart) + 1;
                    
                    // If spike lasts <= 100ms (7 samples), neglect it
                    if (duration <= 7) {
                        for (int k = eventStart; k <= eventEnd; k++) {
                            valid[k] = false;
                        }
                    }
                } else {
                    i++;
                }
            }

            uint32_t cleanMax = 0;
            uint32_t cleanMin = 0xFFFFFFFF;
            for (int i = 0; i < CALIB_SAMPLES; i++) {
                if (valid[i]) {
                    if (calibBuffer[i] > cleanMax) cleanMax = calibBuffer[i];
                    if (calibBuffer[i] < cleanMin) cleanMin = calibBuffer[i];
                }
            }

            if (cleanMax == 0) cleanMax = median; // Fallback
            if (cleanMin == 0xFFFFFFFF) cleanMin = median;

            appCfg.vib.threshold = cleanMax * 3;
            if (appCfg.vib.threshold < 100) appCfg.vib.threshold = 100;
            if (appCfg.vib.threshold > 8000) appCfg.vib.threshold = 8000;

            appCfg.lastCalibMax = cleanMax;
            appCfg.lastCalibMin = cleanMin;
            appCfg.lastSpikeThr = spikeThreshold;
            
            appCfg.vib.stopThreshold = (appCfg.vib.threshold + cleanMax) / 2;
            
            cfgSave(appCfg);

            Serial.printf("[CALIB] Phase 1 Done. Median: %lu, SpikeThr: %lu, CleanMax: %lu, NewThr: %lu\n", 
                          median, spikeThreshold, cleanMax, appCfg.vib.threshold);

            // Transition to Phase 2
            if (appCfg.hasLockSolenoid) {
                calibState = CALIB_LOCK_WAITING;
                Serial.println("[CALIB] Phase 2: Waiting for 3 solenoid actuations...");
            } else {
                calibState = CALIB_SKIP_LOCK;
                calibLastLockEventMs = now;
                Serial.println("[CALIB] No Lock Solenoid configured. Skipping Phase 2...");
            }
            
            // Clean up RAM immediately
            delete[] calibBuffer;
            calibBuffer = nullptr;
            Serial.println("[CALIB] Phase 2: Waiting for 3 solenoid actuations...");
        } else if (calibState == CALIB_LOCK_WAITING) {
            uint32_t lockTrigger = appCfg.vib.threshold * 7;
            
            // Only start a new event if 2.5s cooldown has passed OR we are already IN an event tracking it
            if (currentMag > lockTrigger && (calibInLockEvent || (now - calibLastLockEventMs) >= 2500)) {
                calibInLockEvent = true;
                if (currentMag > calibCurrentLockPeak) {
                    calibCurrentLockPeak = currentMag;
                }
            } else if (calibInLockEvent && currentMag < (lockTrigger / 2)) {
                // Event ended
                if (calibLockCount < 3) {
                    calibLockPeaks[calibLockCount] = calibCurrentLockPeak;
                }
                calibLockCount++;
                calibLastLockEventMs = now; // Start cooldown
                Serial.printf("[CALIB] Solenoid Actuation %d/3 Detected. Peak: %lu (Cooldown started)\n", calibLockCount, calibCurrentLockPeak);
                
                calibInLockEvent = false;
                calibCurrentLockPeak = 0;
                
                if (calibLockCount >= 3) {
                    uint32_t p[3] = {calibLockPeaks[0], calibLockPeaks[1], calibLockPeaks[2]};
                    std::sort(p, p + 3);
                    appCfg.lastLockPeak = p[1]; // Median of 3
                    
                    cfgSave(appCfg);
                    Serial.printf("[CALIB] Phase 2 Done. Median Lock Peak: %lu\n", appCfg.lastLockPeak);
                    calibState = CALIB_SEW_WAITING;
                    if (calibSewBuffer) delete[] calibSewBuffer;
                    calibSewBuffer = new uint32_t[CALIB_SEW_SAMPLES];
                    calibSewIdx = 0;
                    
                    if (attemptLowestPeaks) delete[] attemptLowestPeaks;
                    if (attemptMedians) delete[] attemptMedians;
                    if (attemptDurations) delete[] attemptDurations;
                    attemptLowestPeaks = new uint32_t[5];
                    attemptMedians = new uint32_t[5];
                    attemptDurations = new uint32_t[5];
                    fineTuneCount = 0;
                    
                    Serial.println("[CALIB] Phase 3: Waiting for 1st sewing attempt...");
                }
            }
        } else if (calibState == CALIB_SKIP_LOCK) {
            if (now - calibLastLockEventMs >= 1000) {
                skipCalibPhase2();
            }
        } else if (calibState == CALIB_SEW_WAITING || calibState == CALIB_FINE_TUNE) {
            if (calibSewBuffer && calibSewIdx < CALIB_SEW_SAMPLES) {
                calibSewBuffer[calibSewIdx++] = currentMag;
            }
            if (counted) {
                prevAttemptEndMs = imuGetLastVibEnd();
                calibState = CALIB_SEW_DONE;
                Serial.printf("[CALIB] Attempt %d auto finish detected.\n", fineTuneCount + 1);
            }
        }
        
        if (calibState == CALIB_SEW_DONE) {
            // Processing logic here
            uint32_t tempStart = appCfg.vib.threshold;
            uint32_t tempStop = appCfg.vib.stopThreshold;
            uint32_t cleanMax = tempStop * 2 - tempStart; // derived back since tempStop = (tempStart + cleanMax)/2
            
            // 1. Pre-processing: extract valid segments
            uint32_t* processedBuf = new uint32_t[CALIB_SEW_SAMPLES];
            uint16_t procIdx = 0;
            
            for (uint16_t i = 0; i < calibSewIdx; i++) {
                uint32_t mag = calibSewBuffer[i];
                if (mag < tempStop) continue; // Ignore Noise
                
                // Identify Spikes (rapid high magnitude > 80% of lastLockPeak)
                // For simplicity here, if it exceeds appCfg.lastLockPeak * 0.8, it's a spike.
                if (mag > (appCfg.lastLockPeak * 0.8)) {
                    if (mag > appCfg.lastLockPeak && appCfg.lastLockPeak != 22000) {
                        appCfg.lastLockPeak = (appCfg.lastLockPeak + mag) / 2; // Refine Lock Peak
                    }
                    continue; // Skip spike
                }
                
                processedBuf[procIdx++] = mag;
            }
            
            if (procIdx > 10) { // Safety check
                // 2. Middle 50% Extraction
                uint16_t startIdx = procIdx / 4;
                uint16_t endIdx = startIdx * 3;
                uint16_t midLen = endIdx - startIdx;
                
                // 3. Minimum Duration Calculation
                // midLen is half of procIdx. The duration of middle 50% is midLen * IMU_SAMPLE_MS
                // minDuration = duration / 2.5
                appCfg.vib.minDurationMs = (uint32_t)((midLen * IMU_SAMPLE_MS) / 2.5);
                if (appCfg.vib.minDurationMs < 150) appCfg.vib.minDurationMs = 150; // clamp bottom
                
                // 4. Threshold Calculation
                uint32_t* midBuf = new uint32_t[midLen];
                for (uint16_t i = 0; i < midLen; i++) midBuf[i] = processedBuf[startIdx + i];
                std::sort(midBuf, midBuf + midLen);
                
                uint32_t middleMedian = midBuf[midLen / 2];
                uint32_t middleLowest = midBuf[midLen / 10]; // 10th percentile
                delete[] midBuf;
                
                uint32_t newStart = (tempStart + middleMedian) / 2;
                
                // 5. Constraint Enforcement
                if (newStart > middleLowest) {
                    newStart = (middleLowest + tempStart) / 2;
                }
                uint32_t newStop = (newStart + cleanMax) / 2;
                
                // Store arrays
                attemptLowestPeaks[fineTuneCount] = middleLowest;
                attemptMedians[fineTuneCount] = middleMedian;
                attemptDurations[fineTuneCount] = appCfg.vib.minDurationMs;
                
                if (fineTuneCount == 0) {
                    // Update temp config for next 4 attempts
                    appCfg.vib.threshold = newStart;
                    appCfg.vib.stopThreshold = newStop;
                    cfgSave(appCfg);
                    Serial.printf("[CALIB] Phase 3 Done! StartThr:%lu, StopThr:%lu, MinDur:%lu\n", newStart, newStop, appCfg.vib.minDurationMs);
                    
                    calibState = CALIB_FINE_TUNE;
                    calibSewIdx = 0; // reset buffer for attempt 2
                    fineTuneCount++;
                } else {
                    fineTuneCount++;
                    if (fineTuneCount < 5) {
                        calibState = CALIB_FINE_TUNE;
                        calibSewIdx = 0;
                        Serial.printf("[CALIB] Phase 4: Attempt %d done.\n", fineTuneCount);
                    } else {
                        // Global Analysis
                        uint32_t meds[5], lows[5], durs[5];
                        for(int i=0; i<5; i++) {
                            meds[i] = attemptMedians[i];
                            lows[i] = attemptLowestPeaks[i];
                            durs[i] = attemptDurations[i];
                        }
                        std::sort(meds, meds + 5);
                        std::sort(durs, durs + 5);
                        
                        appCfg.vib.toleratingThr = meds[2]; // Median of medians
                        appCfg.vib.minDurationMs = durs[2]; // Median of durations
                        
                        bool thrTooHigh = false;
                        for(int i=0; i<5; i++) {
                            if (newStart > lows[i]) { thrTooHigh = true; break; }
                        }
                        
                        if (thrTooHigh) {
                            int32_t adjusted = ((newStart + appCfg.vib.toleratingThr) / 2) - 200;
                            if (adjusted < 100) adjusted = 100;
                            newStart = adjusted;
                            newStop = (newStart + cleanMax) / 2;
                        }
                        
                        appCfg.vib.threshold = newStart;
                        appCfg.vib.stopThreshold = newStop;
                        appCfg.lastCalibStatus = 1; // SUCCESS
                        cfgSave(appCfg);
                        Serial.printf("[CALIB] FULL CALIBRATION COMPLETE. Final Start:%lu, Final Stop:%lu, MinDur:%lu, TolThr:%ld\n", 
                                      newStart, newStop, appCfg.vib.minDurationMs, appCfg.vib.toleratingThr);
                                      
                        // Cleanup
                        delete[] attemptLowestPeaks; attemptLowestPeaks = nullptr;
                        delete[] attemptMedians; attemptMedians = nullptr;
                        delete[] attemptDurations; attemptDurations = nullptr;
                        if (calibSewBuffer) { delete[] calibSewBuffer; calibSewBuffer = nullptr; }
                        calibState = CALIB_IDLE;

                        if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) {
                            mqttPublishEvent(2, appCfg.deviceId); // calib_success
                        }
                        
                        displayShowMessage("CALIBRATION", "DONE");
                        delay(1000);
                    }
                }
            } else {
                Serial.println("[CALIB] Phase 3/4 Error: Not enough valid data!");
                if (fineTuneCount > 0) calibState = CALIB_FINE_TUNE;
                else calibState = CALIB_SEW_WAITING;
                calibSewIdx = 0; // safely retry instead of aborting
                calibWarningMs = millis(); // Trigger OLED warning
            }
            delete[] processedBuf;
        }

        // NEW: Real-time Serial Plotting
        Serial.print(">Magnitude:");
        Serial.print(imuGetMagnitude());
        Serial.print(",TempStart:");
        Serial.print(appCfg.vib.threshold);
        Serial.print(",TempStop:");
        Serial.print(appCfg.vib.stopThreshold);
        Serial.print(",Active:");
        Serial.println(newVib ? (appCfg.vib.threshold * 1.2) : 0.0);

        if      (!vibActive && !counted) sewState = 0;
        else if (vibActive)              sewState = 2;
        if      (counted)                sewState = 0;

        if (counted) {
            appCfg.count++;
            Serial.printf("[COUNT] %lu\n", (unsigned long)appCfg.count);
            cfgSaveCount(appCfg.count);
            if (wifiOk && appCfg.mqttEnabled && mqttIsConnected() && calibState == CALIB_IDLE) mqttPublish(appCfg.count, appCfg.deviceId);
            lastMqttMs = now;
        }
    }

    // ── Buttons ──
    handleButtons();

    // ── MQTT periodic heartbeat ──
    if (wifiOk && appCfg.mqttEnabled) {
        if (now - lastMqttMs >= appCfg.mqttIntervalMs) {
            lastMqttMs = now;
            if (mqttIsConnected() && calibState == CALIB_IDLE) mqttPublish(appCfg.count, appCfg.deviceId);
        }
    }

    // ── Web portal ──
    webServerLoop();
    webServerSetMqttOk(wifiOk && appCfg.mqttEnabled && mqttIsConnected());
    webServerSetVibActive(vibActive);
    webServerSetSewState(sewState);

    if (webServerHasUpdate()) {
        webServerClearUpdate();
        Serial.println("[MAIN] Portal config applied.");
    }

    // ── WiFi watchdog ──
    if (wifiOk && WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Lost — reconnecting...");
        WiFi.reconnect();
        displayShowError("WiFi lost", "Reconnecting...");
        delay(3000);
    }

    // ── Display update ────────────────────────────────────────────────────────
    if (now - lastDisplayMs >= 100) {
        lastDisplayMs = now;
        
        if (calibState != CALIB_IDLE) {
            if (calibWarningMs > 0 && (now - calibWarningMs < 2000)) {
                displayShowMessage("WARNING", "Invalid Data!");
            } else {
                uint8_t dispImuState = imuGetState();
                if (calibState == CALIB_LOCK_WAITING && calibLockCount > 0 && (now - calibLastLockEventMs < 2500)) {
                    dispImuState = 3;
                }
                displayShowCalibration((uint8_t)calibState, fineTuneCount, calibIdx, dispImuState, calibLockCount);
            }
        } else {
            String ip = wifiOk ? WiFi.localIP().toString() : "offline";
            displayShowRunning(appCfg.count, ip.c_str(), appCfg.mqttEnabled,
                               wifiOk && mqttIsConnected(), vibActive, appCfg.lastCalibStatus);
        }
    }
}