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

static bool     vibActive     = false;
static uint8_t  sewState      = 0;
static bool     wifiOk        = false;

#include <algorithm> // for std::sort
// ── Calibration ───────────────────────────────────────────────────────────────
enum CalibState { CALIB_IDLE, CALIB_SAMPLING, CALIB_NOISE_DONE, CALIB_LOCK_WAITING };
static CalibState calibState = CALIB_IDLE;
static uint8_t calibLockCount = 0;
static uint32_t calibLockPeaks[3] = {0, 0, 0};
static uint32_t calibCurrentLockPeak = 0;
static uint32_t calibLastLockEventMs = 0;
static bool calibInLockEvent = false;
#define CALIB_SAMPLES 150
static uint16_t calibIdx = 0;

// Local Buffer for calibration
uint32_t* calibBuffer = nullptr;

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
}

void getCalibStatus(int& state, int& lockCount) {
    state = (int)calibState;
    lockCount = calibLockCount;
}

void skipCalibPhase2() {
    if (calibState == CALIB_LOCK_WAITING) {
        calibState = CALIB_IDLE; // Skip to next (currently IDLE)
        appCfg.lastLockPeak = 0; // 0 indicates no lock detected/skipped
        cfgSave(appCfg);
        Serial.println("[CALIB] Phase 2 Skipped by user.");
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

    // ── INC button ──
    if (btnIncLast == HIGH && incNow == LOW) {
        btnIncDownMs = now;
        btnIncLong   = false;
    }
    if (incNow == LOW && !btnIncLong && (now - btnIncDownMs) >= LONG_PRESS_MS) {
        btnIncLong    = true;
        appCfg.count  = 0;
        cfgSaveCount(0);
        // FIX: guard MQTT publish with connection check
        if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
        Serial.println("[BTN] Long INC → reset");
    }
    if (btnIncLast == LOW && incNow == HIGH && !btnIncLong && (now - btnIncDownMs) >= DEBOUNCE_MS) {
        appCfg.count++;
        cfgSaveCount(appCfg.count);
        // FIX: guard MQTT publish with connection check
        if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
        Serial.printf("[BTN] +1 → %lu\n", (unsigned long)appCfg.count);
    }

    // ── DEC button ──
    if (btnDecLast == HIGH && decNow == LOW) {
        btnDecDownMs = now;
        btnDecLong   = false;
    }
    if (decNow == LOW && !btnDecLong && (now - btnDecDownMs) >= LONG_PRESS_MS) {
        btnDecLong    = true;
        appCfg.count  = 0;
        cfgSaveCount(0);
        // FIX: guard MQTT publish with connection check
        if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
        Serial.println("[BTN] Long DEC → reset");
    }
    if (btnDecLast == LOW && decNow == HIGH && !btnDecLong && (now - btnDecDownMs) >= DEBOUNCE_MS) {
        if (appCfg.count > 0) appCfg.count--;
        cfgSaveCount(appCfg.count);
        // FIX: guard MQTT publish with connection check
        if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
        Serial.printf("[BTN] -1 → %lu\n", (unsigned long)appCfg.count);
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
        bool counted = imuUpdate(appCfg.vib, &newVib);
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
            calibState = CALIB_LOCK_WAITING;
            
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
                    calibState = CALIB_IDLE;
                }
            }
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
            if (wifiOk && appCfg.mqttEnabled && mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
            lastMqttMs = now;
        }
    }

    // ── Buttons ──
    handleButtons();

    // ── MQTT periodic heartbeat ──
    if (wifiOk && appCfg.mqttEnabled) {
        mqttLoop();
        if (now - lastMqttMs >= appCfg.mqttIntervalMs) {
            lastMqttMs = now;
            if (mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
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

    // ── Display at 4 Hz ──
    if (now - lastDisplayMs >= 250) {
        lastDisplayMs = now;
        String ip = wifiOk ? WiFi.localIP().toString() : "offline";
        displayShowRunning(appCfg.count, ip.c_str(),
                           wifiOk && mqttIsConnected(), vibActive);
    }
}