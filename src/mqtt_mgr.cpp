#include "mqtt_mgr.h"
#include "config.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static WiFiClient   wifiClient;
static PubSubClient mqttClient(wifiClient);

static char         _clientId[32];
static char         _myDeviceId[32];
static uint32_t     _lastReconnectMs = 0;

struct MqttMessage {
    uint8_t type; // 0=count, 1=calib_start, 2=calib_success, 3=calib_canceled
    uint32_t count;
    char deviceId[32];
};
static QueueHandle_t mqttQueue = NULL;
static volatile bool _isMqttConnected = false;

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
        char topicCfg[64];
        snprintf(topicCfg, sizeof(topicCfg), "labelcounter/%s/config", _myDeviceId);
        mqttClient.subscribe(topicCfg);
    } else {
        Serial.printf("[MQTT] Failed rc=%d\n", mqttClient.state());
    }
}

static void mqttTaskRunner(void* pvParameters) {
    MqttMessage msg;
    for(;;) {
        bool connected = mqttClient.connected();
        _isMqttConnected = connected;

        if (!connected) {
            reconnect();
        } else {
            mqttClient.loop();
            if (xQueueReceive(mqttQueue, &msg, pdMS_TO_TICKS(10)) == pdPASS) {
                JsonDocument doc;
                doc["device"]    = msg.deviceId;
                doc["timestamp"] = millis(); // uptime ms

                if (msg.type == 0) {
                    doc["count"] = msg.count;
                } else if (msg.type == 1) {
                    doc["event"] = "calibration_start";
                } else if (msg.type == 2) {
                    doc["event"] = "calibration_done";
                    doc["status"] = "success";
                } else if (msg.type == 3) {
                    doc["event"] = "calibration_done";
                    doc["status"] = "canceled";
                }

                char buf[128];
                size_t n = serializeJson(doc, buf, sizeof(buf));
                char topicPub[64];
                snprintf(topicPub, sizeof(topicPub), "labelcounter/%s/data", msg.deviceId);
                mqttClient.publish(topicPub, buf, n);

                Serial.printf("[MQTT Task] Published to %s: %s\n", topicPub, buf);
            }
        }
        // Yield to watchdog
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── Public ────────────────────────────────────────────────────────────────────
void mqttInit(const char* deviceId) {
    snprintf(_clientId, sizeof(_clientId), "lc_%s", deviceId);
    strncpy(_myDeviceId, deviceId, sizeof(_myDeviceId));
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE);
    mqttClient.setCallback(onMessage);
    Serial.printf("[MQTT] Client ID: %s\n", _clientId);

    mqttQueue = xQueueCreate(10, sizeof(MqttMessage));
    
    xTaskCreatePinnedToCore(
        mqttTaskRunner,   // Function
        "MQTT_Task",      // Name
        4096,             // Stack size
        NULL,             // Params
        1,                // Priority
        NULL,             // Handle
        0                 // Core 0
    );
}

bool mqttIsConnected() {
    return _isMqttConnected;
}

void mqttPublish(uint32_t count, const char* deviceId) {
    if (mqttQueue == NULL) return;

    MqttMessage msg;
    msg.type = 0;
    msg.count = count;
    strncpy(msg.deviceId, deviceId, sizeof(msg.deviceId)-1);
    msg.deviceId[sizeof(msg.deviceId)-1] = '\0';

    // Don't wait if queue is full (timeout 0)
    if (xQueueSend(mqttQueue, &msg, 0) != pdPASS) {
        Serial.println("[MQTT] Warning: Queue full, dropped message.");
    }
}

void mqttPublishEvent(uint8_t type, const char* deviceId) {
    if (mqttQueue == NULL) return;

    MqttMessage msg;
    msg.type = type;
    msg.count = 0;
    strncpy(msg.deviceId, deviceId, sizeof(msg.deviceId)-1);
    msg.deviceId[sizeof(msg.deviceId)-1] = '\0';

    if (xQueueSend(mqttQueue, &msg, 0) != pdPASS) {
        Serial.println("[MQTT] Warning: Queue full, dropped event.");
    }
}
