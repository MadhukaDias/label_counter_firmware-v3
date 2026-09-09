#include "mqtt_mgr.h"
#include "config.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <atomic>
#include <esp_task_wdt.h>

static WiFiClient   wifiClient;
static PubSubClient mqttClient(wifiClient);

static char         _clientId[32];
static char         _myDeviceId[32];
static uint32_t     _lastReconnectMs = 0;

struct MqttMessage {
    uint8_t type; // 0=count, 1=event_str, 2=waveform
    uint32_t count;
    char deviceId[32];
    char eventName[32];
    char status[16];
    uint32_t* waveformData;
    uint16_t waveformLength;
};
static QueueHandle_t mqttQueue = NULL;
static std::atomic<bool> _isMqttConnected{false};
static std::atomic<bool> _enabled{true};

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
    // Subscribe this task to the TWDT so a hung connect/loop cannot stall reboot.
    esp_task_wdt_add(NULL);
    for(;;) {
        esp_task_wdt_reset();
        if(!_enabled.load() || WiFi.status()!=WL_CONNECTED){
            _isMqttConnected=false;
            if(mqttClient.connected())mqttClient.disconnect();
            while(xQueueReceive(mqttQueue,&msg,0)==pdPASS){
                if(msg.type==2&&msg.waveformData)free(msg.waveformData);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
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
                    doc["event"] = msg.eventName;
                    if (strlen(msg.status) > 0) {
                        doc["status"] = msg.status;
                    }
                } else if (msg.type == 2) {
                    doc["event"] = msg.eventName;
                    doc["count"] = msg.count;
                    if (msg.waveformData != nullptr && msg.waveformLength > 0) {
                        JsonArray dataArr = doc["data"].to<JsonArray>();
                        for (uint16_t i = 0; i < msg.waveformLength; i++) {
                            dataArr.add(msg.waveformData[i]);
                        }
                    }
                }

                char topicPub[64];
                snprintf(topicPub, sizeof(topicPub), "labelcounter/%s/data", msg.deviceId);
                
                size_t jsonSize = measureJson(doc);
                if (mqttClient.beginPublish(topicPub, jsonSize, false)) {
                    serializeJson(doc, mqttClient);
                    mqttClient.endPublish();
                    Serial.printf("[MQTT Task] Published %u bytes to %s\n", (unsigned int)jsonSize, topicPub);
                } else {
                    Serial.println("[MQTT Task] Failed to begin publish (too large?)");
                }
                
                if (msg.type == 2 && msg.waveformData != nullptr) {
                    free(msg.waveformData);
                }
            }
        }
        // Yield to watchdog
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ── Public ────────────────────────────────────────────────────────────────────
void mqttInit(const char* deviceId) {
    if(mqttQueue)return;
    snprintf(_clientId, sizeof(_clientId), "lc_%s", deviceId);
    snprintf(_myDeviceId, sizeof(_myDeviceId), "%s", deviceId);
    mqttClient.setBufferSize(16384);
    if (mqttClient.getBufferSize() < 16384) {
        Serial.println("[MQTT] ERROR: Failed to allocate 16KB client buffer; disabling MQTT");
        _enabled = false;
        return;
    }
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE);
    mqttClient.setSocketTimeout(3);
    mqttClient.setCallback(onMessage);
    Serial.printf("[MQTT] Client ID: %s\n", _clientId);

    mqttQueue = xQueueCreate(10, sizeof(MqttMessage));
    
    if(!mqttQueue){Serial.println("[MQTT] Queue allocation failed");return;}
    BaseType_t created=xTaskCreatePinnedToCore(
        mqttTaskRunner,   // Function
        "MQTT_Task",      // Name
        8192,             // Stack size (JSON serialization of waveform needs room)
        NULL,             // Params
        1,                // Priority
        NULL,             // Handle
        0                 // Core 0
    );
    if(created!=pdPASS){vQueueDelete(mqttQueue);mqttQueue=nullptr;Serial.println("[MQTT] Task creation failed");}
}

void mqttSetEnabled(bool enabled){_enabled=enabled;}
bool mqttIsConnected() {
    return _enabled.load() && WiFi.status()==WL_CONNECTED && _isMqttConnected.load();
}

void mqttPublish(uint32_t count, const char* deviceId) {
    if (mqttQueue == NULL || !mqttIsConnected()) return;

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

void mqttPublishEventStr(const char* eventName, const char* status, const char* deviceId) {
    if (mqttQueue == NULL || !mqttIsConnected()) return;

    MqttMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = 1;
    strncpy(msg.eventName, eventName, sizeof(msg.eventName)-1);
    if (status != nullptr) strncpy(msg.status, status, sizeof(msg.status)-1);
    strncpy(msg.deviceId, deviceId, sizeof(msg.deviceId)-1);

    if (xQueueSend(mqttQueue, &msg, 0) != pdPASS) {
        Serial.println("[MQTT] Warning: Queue full, dropped event.");
    }
}

void mqttPublishWaveform(uint32_t count, const char* eventName, const uint32_t* buffer, uint16_t length, const char* deviceId) {
    if (mqttQueue == NULL || !mqttIsConnected()) return;

    MqttMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = 2;
    msg.count = count;
    strncpy(msg.eventName, eventName, sizeof(msg.eventName)-1);
    strncpy(msg.deviceId, deviceId, sizeof(msg.deviceId)-1);
    
    if (buffer != nullptr && length > 0) {
        msg.waveformData = (uint32_t*)malloc(length * sizeof(uint32_t));
        if (msg.waveformData != nullptr) {
            memcpy(msg.waveformData, buffer, length * sizeof(uint32_t));
            msg.waveformLength = length;
        } else {
            Serial.println("[MQTT] ERROR: Out of memory allocating waveform array.");
            return;
        }
    }

    if (xQueueSend(mqttQueue, &msg, 0) != pdPASS) {
        if (msg.waveformData) free(msg.waveformData);
        Serial.println("[MQTT] Warning: Queue full, dropped waveform.");
    }
}
