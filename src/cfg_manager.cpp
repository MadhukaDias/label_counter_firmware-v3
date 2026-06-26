#include "cfg_manager.h"
#include "config.h"
#include <Preferences.h>

static Preferences prefs;

void cfgLoad(AppConfig& cfg) {
    prefs.begin(NVS_NAMESPACE, true);   // read-only

    cfg.vib.threshold      = prefs.getInt("threshold",  DEF_VIB_THRESHOLD);
    cfg.vib.minDurationMs  = prefs.getUInt("minDur",    DEF_MIN_DURATION_MS);
    cfg.vib.silenceMs      = prefs.getUInt("silence",   DEF_SILENCE_MS);
    cfg.mqttIntervalMs     = prefs.getUInt("mqttInt",   DEF_MQTT_INTERVAL_MS);

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
    prefs.putUInt("minDur",   cfg.vib.minDurationMs);
    prefs.putUInt("silence",  cfg.vib.silenceMs);
    prefs.putUInt("count",    cfg.count);
    prefs.putUInt("mqttInt",  cfg.mqttIntervalMs);
    prefs.putString("deviceId", cfg.deviceId);

    prefs.end();
    Serial.println("[CFG] Saved.");
}

void cfgReset(AppConfig& cfg) {
    cfg.vib.threshold     = DEF_VIB_THRESHOLD;
    cfg.vib.minDurationMs = DEF_MIN_DURATION_MS;
    cfg.vib.silenceMs     = DEF_SILENCE_MS;
    cfgSave(cfg);
}

void cfgSaveCount(uint32_t count) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt("count", count);
    prefs.end();
}