#pragma once
struct OTAStub{void setHostname(const char*){}void begin(){}void handle(){}};
static OTAStub ArduinoOTA;
