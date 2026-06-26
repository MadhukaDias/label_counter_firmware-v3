#pragma once
#include <Arduino.h>
#include "config.h"

struct VibConfig {
    int32_t threshold;      // magnitude delta to be "vibrating"
    uint32_t minDurationMs; // must vibrate this long to confirm
    uint32_t silenceMs;     // silence after vib to trigger count
};

void imuInit();

// Call every IMU_SAMPLE_MS ms.
// Returns true when a new sewing cycle has been COUNTED.
bool imuUpdate(const VibConfig& cfg, bool* vibActiveOut);

// Raw magnitude (for live display / calibration)
int32_t imuGetMagnitude();
