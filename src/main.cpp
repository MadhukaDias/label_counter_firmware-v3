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
enum CalibState { CALIB_IDLE, CALIB_SAMPLING, CALIB_DONE };
static CalibState calibState = CALIB_IDLE;
#define CALIB_SAMPLES 150
static uint16_t calibIdx = 0;

// Exported for Web Server
uint32_t* calibBuffer = nullptr;
bool* calibValidBuf = nullptr;
uint32_t calibMedian = 0;
uint32_t calibSpikeThr = 0;
uint32_t calibCleanMax = 0;
bool calibHasData = false;

void startCalibration() {
    if (!calibBuffer) calibBuffer = new uint32_t[CALIB_SAMPLES];
    if (!calibValidBuf) calibValidBuf = new bool[CALIB_SAMPLES];
    calibState = CALIB_SAMPLING;
    calibIdx = 0;
    Serial.println("[CALIB] Started 3-second noise sampling...");
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
                calibState = CALIB_DONE;
            }
        } else if (calibState == CALIB_DONE) {
            uint32_t sortedBuf[CALIB_SAMPLES];
            memcpy(sortedBuf, calibBuffer, sizeof(calibBuffer));
            std::sort(sortedBuf, sortedBuf + CALIB_SAMPLES);
            uint32_t median = sortedBuf[CALIB_SAMPLES / 2];

            uint32_t spikeThreshold = (median * 2) + 100;
            bool valid[CALIB_SAMPLES];
            for (int i = 0; i < CALIB_SAMPLES; i++) valid[i] = true;

            for (int i = 0; i < CALIB_SAMPLES; i++) {
                if (calibBuffer[i] > spikeThreshold) {
                    int start = std::max(0, i - 8);
                    int end = std::min(CALIB_SAMPLES - 1, i + 8);
                    for (int k = start; k <= end; k++) valid[k] = false;
                }
            }

            uint32_t cleanMax = 0;
            for (int i = 0; i < CALIB_SAMPLES; i++) {
                if (valid[i] && calibBuffer[i] > cleanMax) {
                    cleanMax = calibBuffer[i];
                }
            }

            if (cleanMax == 0) cleanMax = median; // Fallback

            appCfg.vib.threshold = cleanMax * 3;
            if (appCfg.vib.threshold < 100) appCfg.vib.threshold = 100;
            if (appCfg.vib.threshold > 8000) appCfg.vib.threshold = 8000;

            cfgSave(appCfg);
            
            // Save results for Web UI
            memcpy(calibValidBuf, valid, sizeof(valid));
            calibMedian = median;
            calibSpikeThr = spikeThreshold;
            calibCleanMax = cleanMax;
            calibHasData = true;

            Serial.printf("[CALIB] Done. Median: %lu, SpikeThr: %lu, CleanMax: %lu, NewThr: %lu\n", 
                          median, spikeThreshold, cleanMax, appCfg.vib.threshold);

            calibState = CALIB_IDLE;
        }

        // NEW: Real-time Serial Plotting
        Serial.print(">Magnitude:");
        Serial.print(imuGetMagnitude());
        Serial.print(",Threshold:");
        Serial.print(appCfg.vib.threshold);
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