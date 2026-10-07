#pragma once
#include <Arduino.h>
// Wall-clock time from NTP. The ESP32 system clock keeps running between
// syncs and SNTP re-syncs periodically while Wi-Fi is up.
void timeInit();
bool timeSynced();
// Writes "HH:MM" local time, or "--:--" until the first NTP sync.
void timeFormatClock(char* out, size_t n);
