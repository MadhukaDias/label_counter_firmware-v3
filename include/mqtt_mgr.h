#pragma once
#include <Arduino.h>

void mqttInit(const char* deviceId);
void mqttLoop();
bool mqttIsConnected();
void mqttPublish(uint32_t count, const char* deviceId);
