#pragma once
#include <Arduino.h>
#include "cfg_manager.h"

void webServerInit(AppConfig* cfg);
void webServerLoop();
bool webServerHasUpdate();
void webServerClearUpdate();
void webServerSetMqttOk(bool ok);
void webServerSetVibActive(bool va);
void webServerSetSewState(uint8_t s);   // 0=IDLE 1=VIB 2=CONF 3=COOL

void webServerPause();
void webServerResume();
