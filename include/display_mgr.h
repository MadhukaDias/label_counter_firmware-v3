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
                        bool mqttEnabled, bool mqttOk, bool vibActive, uint8_t calibStatus);
void displayShowError(const char* line1, const char* line2 = nullptr);
void displayShowMessage(const char* line1, const char* line2 = nullptr);
void displayShowCfgIP(const char* ip);   // small overlay: config IP
void displayShowCalibration(uint8_t state, uint8_t ftCount, uint16_t progress = 0, uint8_t imuState = 0);
