#pragma once
#include <Arduino.h>

// ──────────────────────────────────────────────
//  Pin Definitions  (XIAO ESP32-S3)
// ──────────────────────────────────────────────
#define PIN_SDA          D1
#define PIN_SCL          D2
#define PIN_BTN_INC      D3   // +1 count correction
#define PIN_BTN_DEC      D4   // -1 count correction

// ──────────────────────────────────────────────
//  OLED
// ──────────────────────────────────────────────
#define OLED_ADDRESS     0x3C
#define OLED_WIDTH       128
#define OLED_HEIGHT      64

// ──────────────────────────────────────────────
//  ADXL345
// ──────────────────────────────────────────────
#define ADXL_ADDRESS     0x53   // SDO/ALT-ADDR pin low. Use 0x1D if SDO tied high.
#define ADXL_BW_RATE     0x0B   // BW_RATE reg code → 200 Hz output data rate (4x IMU_SAMPLE_HZ)

// ADXL345 full-resolution sensitivity is fixed at 3.9 mg/LSB (256 LSB/g) on every
// range, vs. the MPU-6050's 16384 LSB/g at ±2g — about 64x lower raw counts for
// the same physical vibration. We multiply raw readings by this gain so the old
// DEF_VIB_THRESHOLD, any values already saved in NVS, and the web portal's
// 100-8000 slider range all stay meaningful. If you'd rather start tuning from
// scratch, set this to 1 and just watch the live graph in the portal instead.
#define ADXL_MAG_GAIN    64

#define IMU_SAMPLE_HZ    50     // polling rate
#define IMU_SAMPLE_MS    (1000 / IMU_SAMPLE_HZ)
#define ROLL_AVG_SAMPLES 8      // rolling average window

// ──────────────────────────────────────────────
//  Default Vibration Parameters (NVS-overridable)
// ──────────────────────────────────────────────
#define DEF_VIB_THRESHOLD    800    // raw accel magnitude delta (mg units * 100)
#define DEF_MIN_DURATION_MS  400    // vibration must persist this long
#define DEF_SILENCE_MS       600    // quiet period before count triggers

// ──────────────────────────────────────────────
//  Button debounce
// ──────────────────────────────────────────────
#define DEBOUNCE_MS      50
#define LONG_PRESS_MS    2000   // long-press either button = reset count

// Sanity ceiling for NVS-restored count.
// Values above this are treated as corrupted and reset to 0 on boot.
// Raise if your production run legitimately exceeds this per session.
#define MAX_SANE_COUNT   99999

// ──────────────────────────────────────────────
//  MQTT  (EMQX public broker)
// ──────────────────────────────────────────────
#define MQTT_HOST        "broker.emqx.io"
#define MQTT_PORT        1883
#define MQTT_TOPIC_PUB   "labelcounter/data"
#define MQTT_TOPIC_CFG   "labelcounter/config"   // subscribe for remote cfg
#define MQTT_KEEPALIVE        60
#define MQTT_RECONNECT_MS     5000
#define DEF_MQTT_INTERVAL_MS  10000   // publish every 10s by default

// ──────────────────────────────────────────────
//  WiFiManager AP
// ──────────────────────────────────────────────
#define WIFI_AP_NAME     "LabelCounter"
#define WIFI_AP_PASS     ""          // open AP
#define WIFI_TIMEOUT_S   180         // portal timeout

// ──────────────────────────────────────────────
//  NVS namespace
// ──────────────────────────────────────────────
#define NVS_NAMESPACE    "lc_cfg"

// ──────────────────────────────────────────────
//  Sewing state machine states
// ──────────────────────────────────────────────
enum class SewState : uint8_t {
    IDLE,
    VIBRATING,
    CONFIRMED,
    COOLING
};