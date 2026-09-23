#pragma once
#include <Arduino.h>

void mqttInit(const char* deviceId);
bool mqttIsConnected();
void mqttPublish(uint32_t count, const char* deviceId);
void mqttPublishEventStr(const char* eventName, const char* status, const char* deviceId);
void mqttPublishWaveform(uint32_t count, const char* eventName, const uint32_t* buffer, uint16_t length, const char* deviceId);
