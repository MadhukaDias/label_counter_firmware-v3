#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>

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
        if (wifiOk && mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
        Serial.println("[BTN] Long INC → reset");
    }
    if (btnIncLast == LOW && incNow == HIGH && !btnIncLong && (now - btnIncDownMs) >= DEBOUNCE_MS) {
        appCfg.count++;
        cfgSaveCount(appCfg.count);
        // FIX: guard MQTT publish with connection check
        if (wifiOk && mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
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
        if (wifiOk && mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
        Serial.println("[BTN] Long DEC → reset");
    }
    if (btnDecLast == LOW && decNow == HIGH && !btnDecLong && (now - btnDecDownMs) >= DEBOUNCE_MS) {
        if (appCfg.count > 0) appCfg.count--;
        cfgSaveCount(appCfg.count);
        // FIX: guard MQTT publish with connection check
        if (wifiOk && mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
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
    }

    Serial.println("[BOOT] Ready.");
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // ── IMU at fixed rate ──
    if (now - lastImuMs >= IMU_SAMPLE_MS) {
        lastImuMs = now;

        bool newVib  = false;
        bool counted = imuUpdate(appCfg.vib, &newVib);
        vibActive    = newVib;

        if      (!vibActive && !counted) sewState = 0;
        else if (vibActive)              sewState = 2;
        if      (counted)                sewState = 0;

        if (counted) {
            appCfg.count++;
            Serial.printf("[COUNT] %lu\n", (unsigned long)appCfg.count);
            cfgSaveCount(appCfg.count);
            if (wifiOk && mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
            lastMqttMs = now;
        }
    }

    // ── Buttons ──
    handleButtons();

    // ── MQTT periodic heartbeat ──
    if (wifiOk) {
        mqttLoop();
        if (now - lastMqttMs >= appCfg.mqttIntervalMs) {
            lastMqttMs = now;
            if (mqttIsConnected()) mqttPublish(appCfg.count, appCfg.deviceId);
        }
    }

    // ── Web portal ──
    webServerLoop();
    webServerSetMqttOk(wifiOk && mqttIsConnected());
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