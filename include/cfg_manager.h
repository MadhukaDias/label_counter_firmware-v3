#pragma once
#include <Arduino.h>
#include "imu_detector.h"

struct AppConfig {
    VibConfig vib;
    uint32_t  count;             // persisted count
    uint32_t  mqttIntervalMs;    // how often to publish (ms)
    bool      mqttEnabled;       // whether MQTT is enabled
    char      deviceId[24];
    uint32_t  lastCalibMax;
    uint32_t  lastCalibMin;
    uint32_t  lastSpikeThr;
};

void cfgLoad(AppConfig& cfg);
void cfgSave(const AppConfig& cfg);
void cfgReset(AppConfig& cfg);    // reset to defaults (keeps deviceId)
void cfgSaveCount(uint32_t count);
