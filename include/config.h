#pragma once
#include <Arduino.h>

// Waveshare ESP32-S3-Nano. Raw ESP32 GPIO numbers (not Arduino D numbers).
#define PIN_SDA 11             // A4 -> ADXL345 SDA
#define PIN_SCL 12             // A5 -> ADXL345 SCL
#define PIN_BTN_INC 8          // D5 -> button -> GND
#define PIN_BTN_DEC 7          // D4 -> button -> GND
#define PIN_BTN_SELECT 6       // D3 -> button -> GND
#define PIN_BTN_BACK 5         // D2 -> button -> GND
#define TFT_SCLK 48            // D13
#define TFT_MOSI 38            // D11
#define TFT_CS 21              // D10
#define TFT_DC 18              // D9
#define TFT_RST 17             // D8
#define TFT_ROTATION 1         // 160 x 128 landscape
#define TFT_TAB INITR_BLACKTAB // Try INITR_GREENTAB / INITR_REDTAB if needed.
#define TFT_SPI_HZ 8000000

// ──────────────────────────────────────────────
//  ADXL345
// ──────────────────────────────────────────────
#define ADXL_ADDRESS                                                           \
  0x53 // Primary address (SDO/ALT-ADDR pin low). Use 0x1D
       // if SDO tied high. The driver auto-probes both 0x53
       // and 0x1D at init and uses whichever answers, so a
       // mis-strapped module still works without a rebuild.
#define ADXL_BW_RATE                                                           \
  0x0B // BW_RATE reg code → 200 Hz output data rate (4x IMU_SAMPLE_HZ)

// ADXL345 full-resolution sensitivity is fixed at 3.9 mg/LSB (256 LSB/g) on
// every range, vs. the MPU-6050's 16384 LSB/g at ±2g — about 64x lower raw
// counts for the same physical vibration. We multiply raw readings by this gain
// so the old DEF_VIB_THRESHOLD, any values already saved in NVS, and the web
// portal's 100-8000 slider range all stay meaningful. If you'd rather start
// tuning from scratch, set this to 1 and just watch the live graph in the
// portal instead.
#define ADXL_MAG_GAIN 64

#define IMU_SAMPLE_HZ 50 // polling rate
#define IMU_SAMPLE_MS (1000 / IMU_SAMPLE_HZ)
#define ROLL_AVG_SAMPLES 8 // rolling average window

// ──────────────────────────────────────────────
//  Default Vibration Parameters (NVS-overridable)
// ──────────────────────────────────────────────
#define DEF_VIB_THRESHOLD 800   // raw accel magnitude delta (mg units * 100)
#define DEF_MIN_DURATION_MS 400 // vibration must persist this long
#define DEF_SILENCE_MS 600      // quiet period before count triggers

// ──────────────────────────────────────────────
//  Button debounce
// ──────────────────────────────────────────────
#define DEBOUNCE_MS 50
#define LONG_PRESS_MS 2000 // long-press either button = reset count

// Sanity ceiling for NVS-restored count.
// Values above this are treated as corrupted and reset to 0 on boot.
// Raise if your production run legitimately exceeds this per session.
#define MAX_SANE_COUNT 99999

// ──────────────────────────────────────────────
//  MQTT  (EMQX public broker)
// ──────────────────────────────────────────────
#define MQTT_HOST "broker.emqx.io"
#define MQTT_PORT 1883
#define MQTT_TOPIC_PUB "labelcounter/data"
#define MQTT_TOPIC_CFG "labelcounter/config" // subscribe for remote cfg
#define MQTT_KEEPALIVE 60
#define MQTT_RECONNECT_MS 5000
#define DEF_MQTT_INTERVAL_MS 10000 // publish every 10s by default

// ──────────────────────────────────────────────
//  WiFiManager AP
// ──────────────────────────────────────────────
#define WIFI_AP_NAME "LabelCounter"
#define WIFI_AP_PASS ""        // open AP
#define WIFI_FALLBACK_MS 10000 // start AP after station disconnect
#define WIFI_RETRY_MS 30000    // retry saved station credentials

// ──────────────────────────────────────────────
//  NVS namespace
// ──────────────────────────────────────────────
#define NVS_NAMESPACE "lc_cfg"

// ──────────────────────────────────────────────
//  Sewing state machine states
// ──────────────────────────────────────────────
enum class SewState : uint8_t { IDLE, VIBRATING, CONFIRMED, COOLING };