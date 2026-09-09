#include "cfg_manager.h"
#include "config.h"
#include <Preferences.h>

static Preferences prefs;

void cfgLoad(AppConfig& cfg) {
    prefs.begin(NVS_NAMESPACE, true);   // read-only

    cfg.vib.threshold      = prefs.getInt("threshold",  DEF_VIB_THRESHOLD);
    cfg.vib.stopThreshold  = prefs.getInt("stopThr",    0);
    cfg.vib.toleratingThr  = prefs.getInt("tolThr",     0);
    cfg.vib.minDurationMs  = prefs.getUInt("minDur",    DEF_MIN_DURATION_MS);
    cfg.vib.silenceMs      = prefs.getUInt("silence",   DEF_SILENCE_MS);
    cfg.mqttIntervalMs     = prefs.getUInt("mqttInt",   DEF_MQTT_INTERVAL_MS);
    cfg.mqttEnabled        = prefs.getBool("mqttEn",    true);
    cfg.hasLockSolenoid    = prefs.getBool("hasSolen",  true);
    cfg.lastCalibMax       = prefs.getUInt("cMax",      0);
    cfg.lastCalibMin       = prefs.getUInt("cMin",      0);
    cfg.lastSpikeThr       = prefs.getUInt("cSThr",     0);
    cfg.lastLockPeak       = prefs.getUInt("cLock",     0);
    cfg.lastCalibStatus    = prefs.getUChar("cStat",    0);

    // FIX: sanity-check the stored count. NVS can hold garbage from a first
    // flash or a corrupted write. Any value above MAX_SANE_COUNT is treated as
    // corrupted and reset to zero so the display doesn't start at a random number.
    uint32_t storedCount   = prefs.getUInt("count", 0);
    cfg.count = (storedCount <= MAX_SANE_COUNT) ? storedCount : 0;

    String id = prefs.getString("deviceId", "");
    if (id.isEmpty()) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(cfg.deviceId, sizeof(cfg.deviceId),
                 "LC-%02X%02X%02X", mac[3], mac[4], mac[5]);
    } else {
        strncpy(cfg.deviceId, id.c_str(), sizeof(cfg.deviceId) - 1);
    }

    prefs.end();
    Serial.printf("[CFG] Loaded: thr=%ld minDur=%lu sil=%lu cnt=%lu id=%s\n",
                  (long)cfg.vib.threshold,
                  (unsigned long)cfg.vib.minDurationMs,
                  (unsigned long)cfg.vib.silenceMs,
                  (unsigned long)cfg.count,
                  cfg.deviceId);
}

void cfgSave(const AppConfig& cfg) {
    prefs.begin(NVS_NAMESPACE, false);

    prefs.putInt("threshold", cfg.vib.threshold);
    prefs.putInt("stopThr",   cfg.vib.stopThreshold);
    prefs.putInt("tolThr",    cfg.vib.toleratingThr);
    prefs.putUInt("minDur",   cfg.vib.minDurationMs);
    prefs.putUInt("silence",  cfg.vib.silenceMs);
    prefs.putUInt("count",    cfg.count);
    prefs.putUInt("mqttInt",  cfg.mqttIntervalMs);
    prefs.putBool("mqttEn",   cfg.mqttEnabled);
    prefs.putBool("hasSolen", cfg.hasLockSolenoid);
    prefs.putString("deviceId", cfg.deviceId);
    prefs.putUInt("cMax",     cfg.lastCalibMax);
    prefs.putUInt("cMin",     cfg.lastCalibMin);
    prefs.putUInt("cSThr",    cfg.lastSpikeThr);
    prefs.putUInt("cLock",    cfg.lastLockPeak);
    prefs.putUChar("cStat",   cfg.lastCalibStatus);

    prefs.end();
    Serial.println("[CFG] Saved.");
}

void cfgReset(AppConfig& cfg) {
    cfg.vib.threshold     = DEF_VIB_THRESHOLD;
    cfg.vib.minDurationMs = DEF_MIN_DURATION_MS;
    cfg.vib.silenceMs     = DEF_SILENCE_MS;
    cfg.hasLockSolenoid   = true;
    cfg.lastCalibStatus   = 0;
    cfgSave(cfg);
}

void cfgSaveCount(uint32_t count) {
    // Rate-limit NVS writes: a burst of button presses would otherwise rewrite
    // the same sector many times (~100K erase cycles per sector). Coalesce to at
    // most one write per second; an unsaved delta costs at most 1s of count on a
    // power loss, which the MQTT interval heartbeat would mirror anyway.
    static uint32_t lastWrite = 0;
    static uint32_t lastSaved = 0xFFFFFFFF;
    uint32_t now = millis();
    if (count == lastSaved) return;
    if (now - lastWrite < 1000) return;
    lastWrite = now;
    lastSaved = count;

    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt("count", count);
    prefs.end();
}