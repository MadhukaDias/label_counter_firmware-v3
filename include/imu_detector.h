#pragma once
#include <Arduino.h>
#include "config.h"

struct VibConfig {
    int32_t threshold;      // magnitude delta to be "vibrating"
    int32_t stopThreshold;  // dynamically calculated threshold for stopping
    uint32_t minDurationMs; // must vibrate this long to confirm
    uint32_t silenceMs;     // silence after vib to trigger count
    int32_t  toleratingThr; // median of 5-attempt medians
};

void imuInit();
void imuCalibrateBase();

// Call every IMU_SAMPLE_MS ms.
// Returns true when a new sewing cycle has been COUNTED.
bool imuUpdate(const VibConfig& cfg, bool* vibActiveOut = nullptr);

// Raw magnitude (for live display / calibration)
int32_t imuGetMagnitude();

// Phase 4 tracking getters
uint32_t imuGetLastVibStart();
uint32_t imuGetLastVibEnd();
int imuGetState();
