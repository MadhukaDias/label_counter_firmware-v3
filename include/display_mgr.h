#pragma once
#include <Arduino.h>

enum class DisplayMode : uint8_t {
    PORTAL,      // WiFi config AP mode
    CONNECTING,  // connecting to saved WiFi
    RUNNING,     // normal operation
    ERROR        // fault screen
};

void displayInit();
void displayShowPortal(const char* apName, const char* apIP);
void displayShowConnecting(const char* ssid);
void displayShowRunning(uint32_t count, const char* ip,
                        bool mqttOk, bool vibActive);
void displayShowError(const char* line1, const char* line2 = nullptr);
void displayShowCfgIP(const char* ip);   // small overlay: config IP
