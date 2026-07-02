#include "imu_detector.h"
#include "config.h"
#include <Wire.h>
#include <math.h>

// ── ADXL345 register map (datasheet rev. G) ─────────────────────────────────
#define ADXL_REG_DEVID         0x00
#define ADXL_REG_BW_RATE       0x2C
#define ADXL_REG_POWER_CTL     0x2D
#define ADXL_REG_DATA_FORMAT   0x31
#define ADXL_REG_DATAX0        0x32   // X0,X1,Y0,Y1,Z0,Z1 — 6 bytes, LSB first

#define ADXL_DEVID_EXPECTED    0xE5
#define ADXL_POWER_CTL_MEASURE 0x08
#define ADXL_DATA_FORMAT_FULLRES_2G 0x08  // FULL_RES=1, range=00 (±2g)

// Per-axis rolling buffers (orientation-independent vibration)
static int32_t  bufX[ROLL_AVG_SAMPLES] = {0};
static int32_t  bufY[ROLL_AVG_SAMPLES] = {0};
static int32_t  bufZ[ROLL_AVG_SAMPLES] = {0};
static uint8_t  rollIdx = 0;

// Idle baseline per axis (captured at boot)
static int32_t  baseX = 0, baseY = 0, baseZ = 0;

// Exposed for web portal
static int32_t  lastMag = 0;

// State machine
static SewState state        = SewState::IDLE;
static uint32_t stateEnterMs = 0;

// ── ADXL345 low-level I2C ────────────────────────────────────────────────────

static uint8_t adxlWrite8(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ADXL_ADDRESS);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission();
}

static uint8_t adxlRead8(uint8_t reg) {
    Wire.beginTransmission(ADXL_ADDRESS);
    Wire.write(reg);
    Wire.endTransmission(false);   // repeated start
    Wire.requestFrom((uint8_t)ADXL_ADDRESS, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

static bool adxlBegin() {
    uint8_t id = adxlRead8(ADXL_REG_DEVID);
    if (id != ADXL_DEVID_EXPECTED) {
        Serial.printf("[IMU] ADXL345 not found! DEVID=0x%02X (expected 0x%02X)\n",
                      id, ADXL_DEVID_EXPECTED);
        return false;
    }

    adxlWrite8(ADXL_REG_DATA_FORMAT, ADXL_DATA_FORMAT_FULLRES_2G);
    adxlWrite8(ADXL_REG_BW_RATE, ADXL_BW_RATE);
    adxlWrite8(ADXL_REG_POWER_CTL, ADXL_POWER_CTL_MEASURE);  // exit standby last
    delay(10);
    return true;
}

// ── helpers ───────────────────────────────────────────────────────────────────

static void readAxes(int32_t& ax, int32_t& ay, int32_t& az) {
    Wire.beginTransmission(ADXL_ADDRESS);
    Wire.write(ADXL_REG_DATAX0);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)ADXL_ADDRESS, (uint8_t)6);

    uint8_t b[6] = {0};
    for (int i = 0; i < 6 && Wire.available(); i++) b[i] = Wire.read();

    int16_t rx = (int16_t)((b[1] << 8) | b[0]);
    int16_t ry = (int16_t)((b[3] << 8) | b[2]);
    int16_t rz = (int16_t)((b[5] << 8) | b[4]);

    // Gain-compensated to keep raw-count scale comparable to the old MPU-6050
    // readings (see ADXL_MAG_GAIN in config.h).
    ax = (int32_t)rx * ADXL_MAG_GAIN;
    ay = (int32_t)ry * ADXL_MAG_GAIN;
    az = (int32_t)rz * ADXL_MAG_GAIN;
}

// Rolling median per axis → prevents single-sample massive spikes from stretching into 160ms pulses
static void rollingAvgAxes(int32_t ax, int32_t ay, int32_t az,
                            int32_t& avgX, int32_t& avgY, int32_t& avgZ) {
    bufX[rollIdx] = ax;
    bufY[rollIdx] = ay;
    bufZ[rollIdx] = az;
    rollIdx = (rollIdx + 1) % ROLL_AVG_SAMPLES;

    int32_t sortX[ROLL_AVG_SAMPLES], sortY[ROLL_AVG_SAMPLES], sortZ[ROLL_AVG_SAMPLES];
    for (int i = 0; i < ROLL_AVG_SAMPLES; i++) {
        sortX[i] = bufX[i];
        sortY[i] = bufY[i];
        sortZ[i] = bufZ[i];
    }
    std::sort(sortX, sortX + ROLL_AVG_SAMPLES);
    std::sort(sortY, sortY + ROLL_AVG_SAMPLES);
    std::sort(sortZ, sortZ + ROLL_AVG_SAMPLES);

    avgX = sortX[ROLL_AVG_SAMPLES / 2];
    avgY = sortY[ROLL_AVG_SAMPLES / 2];
    avgZ = sortZ[ROLL_AVG_SAMPLES / 2];
}

// 3-axis vibration magnitude = sqrt(dX² + dY² + dZ²)
// where dX/Y/Z = deviation of RAW sample from the rolling average (dynamic baseline).
// This is orientation-independent — only vibration energy counts.
static int32_t vibrMagnitude(int32_t ax, int32_t ay, int32_t az, int32_t avgX, int32_t avgY, int32_t avgZ) {
    int64_t dx = ax - avgX;
    int64_t dy = ay - avgY;
    int64_t dz = az - avgZ;
    return (int32_t)sqrt((float)(dx*dx + dy*dy + dz*dz));
}

// ── public ────────────────────────────────────────────────────────────────────

void imuInit() {
    if (!adxlBegin()) return;

    // Capture baseline — device must be stationary
    Serial.println("[IMU] Calibrating baseline (keep device still)...");
    delay(300);
    int64_t sx = 0, sy = 0, sz = 0;
    for (int i = 0; i < 80; i++) {
        int32_t ax, ay, az;
        readAxes(ax, ay, az);
        sx += ax; sy += ay; sz += az;
        delay(10);
    }
    baseX = (int32_t)(sx / 80);
    baseY = (int32_t)(sy / 80);
    baseZ = (int32_t)(sz / 80);

    // Pre-fill rolling buffers with baseline so first reads are stable
    for (int i = 0; i < ROLL_AVG_SAMPLES; i++) {
        bufX[i] = baseX; bufY[i] = baseY; bufZ[i] = baseZ;
    }
    Serial.printf("[IMU] Baseline: X=%ld Y=%ld Z=%ld\n",
                  (long)baseX, (long)baseY, (long)baseZ);
}

bool imuUpdate(const VibConfig& cfg, bool* vibActiveOut) {
    int32_t ax, ay, az;
    readAxes(ax, ay, az);

    int32_t avgX, avgY, avgZ;
    rollingAvgAxes(ax, ay, az, avgX, avgY, avgZ);

    // Calculate magnitude of the AC component (deviation from average)
    int32_t mag = vibrMagnitude(ax, ay, az, avgX, avgY, avgZ);
    lastMag = mag;

    bool vibrating;
    if (state == SewState::VIBRATING || state == SewState::CONFIRMED || state == SewState::COOLING) {
        vibrating = (mag >= cfg.stopThreshold);
    } else {
        vibrating = (mag >= cfg.threshold);
    }
    uint32_t now   = millis();
    bool counted   = false;

    if (vibActiveOut) *vibActiveOut = (state == SewState::VIBRATING ||
                                       state == SewState::CONFIRMED);

    switch (state) {
        case SewState::IDLE:
            if (vibrating) {
                state        = SewState::VIBRATING;
                stateEnterMs = now;
            }
            break;

        case SewState::VIBRATING:
            if (!vibrating) {
                state = SewState::IDLE;   // dropped before min duration
            } else if ((now - stateEnterMs) >= cfg.minDurationMs) {
                state        = SewState::CONFIRMED;
                stateEnterMs = now;
                Serial.println("[IMU] Sewing CONFIRMED");
            }
            break;

        case SewState::CONFIRMED:
            if (!vibrating) {
                state        = SewState::COOLING;
                stateEnterMs = now;
                if (vibActiveOut) *vibActiveOut = false;
            }
            break;

        case SewState::COOLING:
            if (vibrating) {
                state        = SewState::CONFIRMED;  // resumed same cycle
                stateEnterMs = now;
            } else if ((now - stateEnterMs) >= cfg.silenceMs) {
                state   = SewState::IDLE;
                counted = true;
                Serial.println("[IMU] COUNT triggered");
            }
            break;
    }

    return counted;
}

int32_t imuGetMagnitude() { return lastMag; }
