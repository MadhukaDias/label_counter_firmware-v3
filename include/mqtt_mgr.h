#pragma once
#include <Arduino.h>

void mqttInit(const char* deviceId);
bool mqttIsConnected();
void mqttPublish(uint32_t count, const char* deviceId);
void mqttPublishEvent(uint8_t type, const char* deviceId);
