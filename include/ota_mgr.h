#pragma once
#include <Arduino.h>

// Firmware version. CI overrides it from the git tag (-DFW_VERSION="v1.2.0").
// Local builds are "dev" and never auto-update, so USB flashing is not overwritten.
#ifndef FW_VERSION
#define FW_VERSION "v0.0.0-dev"
#endif

// Starts the background update task. Call after networkInit().
void otaMgrInit();
// Call every loop(); marks the new firmware valid once it has run healthy.
void otaMgrLoop();
// Ask the task to check for an update now.
void otaRequestCheck();
// 0-100 while a download is in progress, -1 otherwise.
int  otaProgress();
