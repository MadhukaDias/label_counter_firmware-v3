#include "network_mgr.h"
#include "config.h"
#include "web_server_mgr.h"
#include <WiFi.h>
#include <WiFiManager.h>

namespace {
WiFiManager manager;
bool portal=false, wasConnected=false;
uint32_t disconnectedAt=0, lastRetry=0;
constexpr uint32_t FALLBACK_MS=WIFI_FALLBACK_MS, RETRY_MS=WIFI_RETRY_MS;
}
void networkInit(){
    manager.setConfigPortalBlocking(false);
    manager.setConfigPortalTimeout(0); // Remain reachable until credentials succeed.
    manager.setConnectTimeout(5);
    manager.setSaveConnectTimeout(5);
    manager.setBreakAfterConfig(false);
    WiFi.mode(WIFI_STA);WiFi.setAutoReconnect(true);WiFi.begin();
    disconnectedAt=lastRetry=millis();
}
bool networkConnected(){return WiFi.status()==WL_CONNECTED;}
bool networkPortalActive(){return portal;}
String networkStationIP(){return networkConnected()?WiFi.localIP().toString():String("");}
String networkPortalIP(){return portal?WiFi.softAPIP().toString():String("");}
void networkOpenPortal(){
    if(portal)return;
    // WiFiManager and our dashboard both bind port 80; only one may own it.
    webServerPause();
    manager.startConfigPortal(WIFI_AP_NAME,WIFI_AP_PASS);
    portal=manager.getConfigPortalActive();
    if(!portal)webServerResume();
    Serial.printf("[WiFi] Setup portal %s: %s\n",portal?"active":"failed",networkPortalIP().c_str());
}
void networkLoop(){
    const uint32_t now=millis();
    if(portal){
        manager.process();
        if(!manager.getConfigPortalActive()){
            portal=false;webServerResume();disconnectedAt=now;
        }
    }
    const bool connected=networkConnected();
    if(connected!=wasConnected){
        if(connected){
            // An automatically opened fallback closes after connectivity returns.
            if(portal){manager.stopConfigPortal();portal=false;webServerResume();}
            Serial.printf("[WiFi] Connected: %s\n",WiFi.localIP().toString().c_str());
        }else{disconnectedAt=now;Serial.println("[WiFi] Lost; reconnecting, AP fallback in 10s");}
        wasConnected=connected;
    }
    if(!connected){
        if(!portal&&now-disconnectedAt>=FALLBACK_MS)networkOpenPortal();
        if(now-lastRetry>=RETRY_MS){
            lastRetry=now;
            // Keep the AP up while retrying saved credentials when the router returns.
            WiFi.reconnect();
        }
    }
}
