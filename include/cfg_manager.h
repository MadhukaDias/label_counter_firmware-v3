#pragma once
#include <Arduino.h>
#include "imu_detector.h"

struct AppConfig {
    VibConfig vib;
    uint32_t  count;             // persisted count
    uint32_t  mqttIntervalMs;    // how often to publish (ms)
    bool      mqttEnabled;       // whether MQTT is enabled
    bool      hasLockSolenoid;   // whether the machine has a lock solenoid
    char      deviceId[24];
    uint32_t  lastCalibMax;
    uint32_t  lastCalibMin;
    uint8_t   lastCalibStatus;   // 0: NONE, 1: SUCCESS, 2: CANCELED
};

void cfgLoad(AppConfig& cfg);
void cfgSave(const AppConfig& cfg);
void cfgReset(AppConfig& cfg);    // reset to defaults (keeps deviceId)
void cfgSaveCount(uint32_t count);
