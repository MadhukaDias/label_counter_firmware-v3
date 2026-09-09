#pragma once
#include <Arduino.h>
constexpr int WL_CONNECTED=3,WIFI_STA=1;
struct IPStub{String value;String toString(){return value;}};
struct WiFiStub{
 int state=0,retries=0;
 int status(){return state;}
 void mode(int){}void setAutoReconnect(bool){}void begin(){}void reconnect(){++retries;}
 IPStub localIP(){return {"192.168.1.50"};}IPStub softAPIP(){return {"192.168.4.1"};}
};
static WiFiStub WiFi;
