#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include "network_mgr.h"
#include "button_input.h"
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

// Four debounced buttons: INC, DEC, SELECT, BACK.
static ButtonInput buttons[4];
static const uint8_t buttonPins[]={PIN_BTN_INC,PIN_BTN_DEC,PIN_BTN_SELECT,PIN_BTN_BACK};
static bool chord=false, chordFired=false;
static uint32_t chordAt=0;
static bool menuOpen=false, resetConfirm=false, networkView=false;
static uint8_t menuSelection=0;
static bool otaStarted=false;
static bool portalWasActive=false;

static bool     vibActive     = false;
static uint8_t  sewState      = 0;
static bool     wifiOk        = false;

// ── Waveform Buffer ───────────────────────────────────────────────────────────
#define WAVEFORM_MAX_SAMPLES 2000
static uint32_t waveformBuffer[WAVEFORM_MAX_SAMPLES];
static uint16_t waveformIdx = 0;

static void flushWaveformToMqtt(const char* eventName) {
    if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) {
        mqttPublishWaveform(appCfg.count, eventName, waveformBuffer, waveformIdx, appCfg.deviceId);
    }
    waveformIdx = 0; // Instantly clear/flush the buffer from RAM
}

#include <algorithm> // for std::sort
// ── Calibration ───────────────────────────────────────────────────────────────
enum CalibState { CALIB_IDLE, CALIB_SAMPLING, CALIB_NOISE_DONE, CALIB_LOCK_WAITING, CALIB_SEW_WAITING, CALIB_FINE_TUNE, CALIB_SEW_DONE, CALIB_SKIP_LOCK };
static CalibState calibState = CALIB_IDLE;
static uint8_t calibLockCount = 0;
static uint32_t calibLockPeaks[3] = {0, 0, 0};
static uint32_t calibCurrentLockPeak = 0;
static uint32_t calibLastLockEventMs = 0;
static bool calibInLockEvent = false;
static uint8_t calibSpikeWidth = 0;
static bool calibPhase2Confirming = false;
static uint32_t calibPhase2QuietStartMs = 0;
static uint32_t calibPhase2SewingCooldownMs = 0;
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
    if(calibState != CALIB_IDLE) return;
    menuOpen=false; resetConfirm=false; networkView=false;
    fineTuneCount=0;
    // Defensively release any stale allocations from a prior session so we
    // cannot leak a buffer if a previous exit path left one behind.
    if (calibBuffer) { delete[] calibBuffer; calibBuffer = nullptr; }
    if (calibSewBuffer) { delete[] calibSewBuffer; calibSewBuffer = nullptr; }
    if (attemptLowestPeaks) { delete[] attemptLowestPeaks; attemptLowestPeaks = nullptr; }
    if (attemptMedians) { delete[] attemptMedians; attemptMedians = nullptr; }
    if (attemptDurations) { delete[] attemptDurations; attemptDurations = nullptr; }
    calibBuffer = new uint32_t[CALIB_SAMPLES];
    calibState = CALIB_SAMPLING;
    calibIdx = 0;
    calibLockCount = 0;
    calibLockPeaks[0] = 0; calibLockPeaks[1] = 0; calibLockPeaks[2] = 0;
    calibCurrentLockPeak = 0;
    calibLastLockEventMs = 0;
    calibInLockEvent = false;
    calibSpikeWidth = 0;
    calibPhase2Confirming = false;
    calibPhase2QuietStartMs = 0;
    calibPhase2SewingCooldownMs = 0;
    Serial.println("[CALIB] Phase 1: Started 3-second noise sampling...");
    
    displayShowMessage("CALIBRATION", "MODE");
    // Notification is timed by the display manager; keep sampling responsive.

    if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) {
        mqttPublishEventStr("calibration_start", "", appCfg.deviceId);
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
        // No lock solenoid configured: derive a spike-detection fallback from
        // the Phase 1 noise profile instead of a machine-blind constant. Lock
        // impacts are far stronger than sewing vibration, so scale the measured
        // clean maximum well above the calibrated threshold.
        uint32_t fallback = std::max((uint32_t)(appCfg.vib.threshold * 5),
                                     (uint32_t)2000);
        if (appCfg.lastCalibMax > 0 && appCfg.lastCalibMax < 0xFFFFFFF) {
            fallback = std::max(appCfg.lastCalibMax * 20u, fallback);
        }
        appCfg.lastLockPeak = fallback;
        cfgSave(appCfg);
        Serial.printf("[CALIB] Phase 2 Skipped. Lock peak set to %lu fallback.\n",
                      (unsigned long)appCfg.lastLockPeak);
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
        
        appCfg.vib.minDurationMs = 250;
        cfgSave(appCfg);
        Serial.println("[CALIB] Phase 3: Waiting for 1st sewing attempt...");
    }
}

void abortCalibration() {
    if (calibState != CALIB_IDLE) {
        calibState = CALIB_IDLE;
        appCfg.lastCalibStatus = 2; // CANCELED
        cfgSave(appCfg);
        
        displayShowMessage("CALIBRATION", "ABORTED");
        // Notification is timed by the display manager; keep sampling responsive.
        
        // Cleanup arrays
        if (attemptLowestPeaks) { delete[] attemptLowestPeaks; attemptLowestPeaks = nullptr; }
        if (attemptMedians) { delete[] attemptMedians; attemptMedians = nullptr; }
        if (attemptDurations) { delete[] attemptDurations; attemptDurations = nullptr; }
        if (calibSewBuffer) { delete[] calibSewBuffer; calibSewBuffer = nullptr; }
        if (calibBuffer) { delete[] calibBuffer; calibBuffer = nullptr; }
        
        Serial.println("[CALIB] Calibration aborted by user.");
        displayShowMessage("Aborting", "Calibration...");
        lastDisplayMs = millis();

        if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) {
            mqttPublishEventStr("calibration_done", "canceled", appCfg.deviceId);
        }
    }
}

// Button actions retain calibration capture/undo behavior from the original.
static void captureOrSkip(){
    if(calibState==CALIB_LOCK_WAITING||calibState==CALIB_SKIP_LOCK)skipCalibPhase2();
    else if(calibState==CALIB_SEW_WAITING||calibState==CALIB_FINE_TUNE)calibState=CALIB_SEW_DONE;
}
static void undoCapture(uint32_t now){
    if(calibState!=CALIB_FINE_TUNE||fineTuneCount==0)return;
    --fineTuneCount;
    if((now-prevAttemptEndMs)<=(appCfg.vib.silenceMs*2.5)){
        uint32_t start=imuGetLastVibStart();
        uint32_t gap=start>prevAttemptEndMs?start-prevAttemptEndMs+250:250;
        appCfg.vib.silenceMs=std::min(uint32_t(2000),gap);cfgSave(appCfg);
    }
}
static void handleButtons(){
    uint32_t now=millis();
    for(int i=0;i<4;i++)buttons[i].update(digitalRead(buttonPins[i])==LOW,now,DEBOUNCE_MS,LONG_PRESS_MS);
    // Suppress both individual release actions until BOTH chord keys are up.
    if(buttons[0].down&&buttons[1].down&&!chord){chord=true;chordAt=now;chordFired=false;}
    if(chord){
        buttons[0].clicked=buttons[1].clicked=false;
        if(buttons[0].down&&buttons[1].down&&!chordFired&&now-chordAt>=(calibState==CALIB_IDLE?2000u:3000u)){
            chordFired=true;
            if(calibState==CALIB_IDLE)startCalibration();else abortCalibration();
        }
        if(!buttons[0].down&&!buttons[1].down)chord=false;
    }
    if(calibState!=CALIB_IDLE){
        if(buttons[3].longPress){abortCalibration();return;}
        if(buttons[0].clicked||buttons[2].clicked)captureOrSkip();
        if(buttons[1].clicked){
            if(calibState==CALIB_LOCK_WAITING)skipCalibPhase2();
            else if(calibState==CALIB_FINE_TUNE)undoCapture(now);
        }
        return;
    }
    if(buttons[3].clicked){
        if(resetConfirm)resetConfirm=false;
        else if(menuOpen)menuOpen=false;
        else networkView=!networkView;
        return;
    }
    if(menuOpen){
        if(!resetConfirm){
            if(buttons[0].clicked)menuSelection=(menuSelection+2)%3;
            if(buttons[1].clicked)menuSelection=(menuSelection+1)%3;
        }
        if(buttons[2].clicked){
            if(resetConfirm){
                appCfg.count=0;cfgSaveCount(0);flushWaveformToMqtt("count_reset");
                resetConfirm=menuOpen=false;displayShowMessage("COUNT RESET","0");
            }else if(menuSelection==0){menuOpen=false;startCalibration();}
            else if(menuSelection==1){menuOpen=false;networkView=true;networkOpenPortal();}
            else resetConfirm=true;
        }
        return;
    }
    if(buttons[2].clicked){menuOpen=true;menuSelection=0;networkView=false;return;}
    if(buttons[0].clicked&&appCfg.count<UINT32_MAX){++appCfg.count;cfgSaveCount(appCfg.count);flushWaveformToMqtt("button_inc");}
    if(buttons[1].clicked&&appCfg.count>0){--appCfg.count;cfgSaveCount(appCfg.count);flushWaveformToMqtt("button_dec");}
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n[BOOT] Label Counter starting...");

    Wire.begin(PIN_SDA, PIN_SCL);

    for(uint8_t pin:buttonPins)pinMode(pin,INPUT_PULLUP);

    displayInit();
    cfgLoad(appCfg);

    // FIX: if NVS had a stale count from a previous session, show it on serial
    // so the user knows. Buttons now work reliably to correct it.
    Serial.printf("[BOOT] Restored count from NVS: %lu\n", (unsigned long)appCfg.count);

    imuInit();
    // networkInit() starts WiFi, which brings up the LwIP TCP/IP stack. The web
    // server and MQTT client both open sockets, so they must come AFTER this or
    // server.begin() asserts ("tcpip_send_msg_wait_sem ... Invalid mbox").
    networkInit();
    webServerInit(&appCfg);
    mqttInit(appCfg.deviceId);
    mqttSetEnabled(appCfg.mqttEnabled);
    Serial.println("[BOOT] Ready.");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    networkLoop();
    wifiOk=networkConnected();
    bool portalNow=networkPortalActive();
    if(portalNow&&!portalWasActive)networkView=true;
    portalWasActive=portalNow;
    mqttSetEnabled(appCfg.mqttEnabled);
    if(wifiOk&&!otaStarted){ArduinoOTA.setHostname(appCfg.deviceId);ArduinoOTA.begin();otaStarted=true;}
    if(wifiOk&&otaStarted)ArduinoOTA.handle();

    // ── IMU at fixed rate ──
    if (now - lastImuMs >= IMU_SAMPLE_MS) {
        lastImuMs += IMU_SAMPLE_MS;

        const bool calibrationSample = calibState != CALIB_IDLE;
        bool newVib  = false;
        bool counted = imuUpdate(appCfg.vib, appCfg.lastLockPeak, &newVib);
        vibActive    = newVib;
        uint32_t currentMag = imuGetMagnitude();

        // ── Record Waveform ───────────────────────────────────────────────────────────
        if (waveformIdx < WAVEFORM_MAX_SAMPLES) {
            waveformBuffer[waveformIdx++] = currentMag;
        }

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
            if (appCfg.vib.stopThreshold > 8000) appCfg.vib.stopThreshold = 8000;
            if (appCfg.vib.stopThreshold < appCfg.vib.threshold) appCfg.vib.stopThreshold = appCfg.vib.threshold;
            
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
            if (currentMag > appCfg.vib.threshold) {
                // If it vibrates, push the sewing cooldown forward
                calibPhase2SewingCooldownMs = now;
                
                if (calibPhase2Confirming) {
                    // It spiked again during the quiet confirmation window! It's continuous sewing.
                    calibPhase2Confirming = false;
                    calibInLockEvent = true;
                    calibSpikeWidth = 10; // Invalidate the width
                } else if (now - calibLastLockEventMs >= 2500) {
                    // Start or continue tracking a spike (only if not in lock cooldown)
                    calibInLockEvent = true;
                    calibSpikeWidth++;
                    if (currentMag > calibCurrentLockPeak) {
                        calibCurrentLockPeak = currentMag;
                    }
                }
            } else {
                // Magnitude dropped below threshold
                if (calibInLockEvent) {
                    calibInLockEvent = false;
                    
                    // Evaluate the spike we just saw
                    if (calibSpikeWidth >= 1 && calibSpikeWidth <= 3) {
                        // It was short enough! Start the 200ms quiet confirmation window
                        calibPhase2Confirming = true;
                        calibPhase2QuietStartMs = now;
                    } else {
                        // Too wide (>= 4). It was sewing vibration. Ignore it.
                        calibSpikeWidth = 0;
                        calibCurrentLockPeak = 0;
                    }
                }
                
                if (calibPhase2Confirming) {
                    if (now - calibPhase2QuietStartMs >= 200) {
                        // It stayed completely quiet for 200ms after the spike!
                        // CONFIRMED LOCK HIT!
                        if (calibLockCount < 3) {
                            calibLockPeaks[calibLockCount] = calibCurrentLockPeak;
                        }
                        calibLockCount++;
                        calibLastLockEventMs = now; // Start 2.5s cooldown
                        Serial.printf("[CALIB] Solenoid Actuation %d/3 Confirmed. Peak: %lu (Width: %d)\n", calibLockCount, calibCurrentLockPeak, calibSpikeWidth);
                        
                        calibPhase2Confirming = false;
                        calibSpikeWidth = 0;
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
                            
                            appCfg.vib.minDurationMs = 250;
                            cfgSave(appCfg);
                            Serial.println("[CALIB] Phase 3: Waiting for 1st sewing attempt...");
                        }
                    }
                }
                
                // If it's been quiet long enough, and we are in the 2.5s cooldown, make sure variables are clean
                if (!calibPhase2Confirming && !calibInLockEvent && (now - calibLastLockEventMs < 2500)) {
                    calibSpikeWidth = 0;
                    calibCurrentLockPeak = 0;
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
                // minDuration = duration / 2
                appCfg.vib.minDurationMs = (uint32_t)((midLen * IMU_SAMPLE_MS) / 2);
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
                            if (appCfg.vib.threshold > lows[i]) { thrTooHigh = true; break; }
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
                            mqttPublishEventStr("calibration_done", "success", appCfg.deviceId);
                        }
                        
                        displayShowMessage("CALIBRATION", "DONE");
                        // Notification is timed by the display manager; keep sampling responsive.
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
#ifdef SERIAL_PLOT_ENABLE
        Serial.print(">Magnitude:");
        Serial.print(imuGetMagnitude());
        Serial.print(",TempStart:");
        Serial.print(appCfg.vib.threshold);
        Serial.print(",TempStop:");
        Serial.print(appCfg.vib.stopThreshold);
        Serial.print(",Active:");
        Serial.println(newVib ? (appCfg.vib.threshold * 1.2) : 0.0);
#endif

        sewState = static_cast<uint8_t>(imuGetState());

        if (counted && !calibrationSample && appCfg.count < UINT32_MAX) {
            appCfg.count++;
            Serial.printf("[COUNT] %lu\n", (unsigned long)appCfg.count);
            cfgSaveCount(appCfg.count);
            // Publish auto-counts immediately so a power loss between now and the
            // next interval tick does not drop the update from the broker.
            flushWaveformToMqtt("auto_count");
        }
    }

    // ── Buttons ──
    handleButtons();

    // ── MQTT periodic heartbeat ──
    if (wifiOk && appCfg.mqttEnabled) {
        if (now - lastMqttMs >= appCfg.mqttIntervalMs) {
            lastMqttMs += appCfg.mqttIntervalMs;
            if (calibState == CALIB_IDLE) flushWaveformToMqtt("interval_update");
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

    // ── Display update ────────────────────────────────────────────────────────
    if (now - lastDisplayMs >= 100) {
        lastDisplayMs += 100;
        String stationIP=networkStationIP(), apIP=networkPortalIP();
        displayUpdateCount(appCfg.count);
        displayUpdateMachine(static_cast<uint8_t>(imuGetState()),appCfg.lastCalibStatus);
        displayUpdateNetwork(wifiOk,appCfg.mqttEnabled,mqttIsConnected(),stationIP.c_str(),networkPortalActive(),apIP.c_str());
        
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
            if(menuOpen)displayShowMenu(menuSelection,resetConfirm);
            else if(networkView&&networkPortalActive())displayShowPortal(WIFI_AP_NAME,apIP.c_str());
            else if(networkView&&!wifiOk)displayShowConnecting("Saved network");
            else displayShowRunning();
        }
        displayRender();
    }
}