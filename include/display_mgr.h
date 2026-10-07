#pragma once
#include <Arduino.h>
// Calls copy their strings. Redrawing is deferred until displayRender().
void displayInit();
void displayUpdateCount(uint32_t count);
void displayUpdateNetwork(bool wifi, bool mqttEnabled, bool mqttConnected,
                          const char* stationIP, bool portalActive, const char* portalIP);
void displayUpdateClock(const char* hhmm);
void displayUpdateMachine(uint8_t detectorState, uint8_t calibrationStatus);
void displayShowRunning();
void displayShowPortal(const char* apName, const char* apIP);
void displayShowConnecting(const char* ssid);
void displayShowCalibration(uint8_t state, uint8_t captures, uint16_t progress=0,
                            uint8_t imuState=0, uint8_t locks=0);
void displayShowMenu(uint8_t selected, bool resetConfirm=false);
void displayShowError(const char* line1, const char* line2=nullptr);
void displayShowMessage(const char* line1, const char* line2=nullptr);
void displayShowCfgIP(const char* ip);
void displayRender();
