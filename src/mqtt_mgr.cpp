#include "mqtt_mgr.h"
#include "config.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static WiFiClient   wifiClient;
static PubSubClient mqttClient(wifiClient);

static char         _clientId[32];
static uint32_t     _lastReconnectMs = 0;

// ── Subscription callback ─────────────────────────────────────────────────────
static void onMessage(char* topic, byte* payload, unsigned int len) {
    // Remote config update via MQTT (optional future use)
    Serial.printf("[MQTT] Msg on %s: %.*s\n", topic, (int)len, payload);
}

// ── Reconnect (non-blocking) ──────────────────────────────────────────────────
static void reconnect() {
    if (millis() - _lastReconnectMs < MQTT_RECONNECT_MS) return;
    _lastReconnectMs = millis();

    Serial.printf("[MQTT] Connecting to %s ...\n", MQTT_HOST);
    if (mqttClient.connect(_clientId)) {
        Serial.println("[MQTT] Connected");
        mqttClient.subscribe(MQTT_TOPIC_CFG);
    } else {
        Serial.printf("[MQTT] Failed rc=%d\n", mqttClient.state());
    }
}

// ── Public ────────────────────────────────────────────────────────────────────
void mqttInit(const char* deviceId) {
    snprintf(_clientId, sizeof(_clientId), "lc_%s", deviceId);
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE);
    mqttClient.setCallback(onMessage);
    Serial.printf("[MQTT] Client ID: %s\n", _clientId);
}

void mqttLoop() {
    if (!mqttClient.connected()) reconnect();
    mqttClient.loop();
}

bool mqttIsConnected() {
    return mqttClient.connected();
}

void mqttPublish(uint32_t count, const char* deviceId) {
    if (!mqttClient.connected()) return;

    JsonDocument doc;
    doc["device"]    = deviceId;
    doc["count"]     = count;
    doc["timestamp"] = millis();     // uptime ms (replace with NTP if needed)

    char buf[128];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqttClient.publish(MQTT_TOPIC_PUB, buf, n);

    Serial.printf("[MQTT] Published: %s\n", buf);
}
