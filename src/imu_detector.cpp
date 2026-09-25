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
static int32_t  lastMag      = 0;

static uint32_t lastVibStartMs = 0;
static uint32_t lastVibEndMs   = 0;

// State machine
static SewState state        = SewState::IDLE;
static uint32_t stateEnterMs = 0;

// ── ADXL345 low-level I2C ────────────────────────────────────────────────────

// ADXL345 I2C address is strapped by the SDO/ALT-ADDRESS pin: low → 0x53,
// high → 0x1D. At init we probe both and remember whichever one answers, so a
// mis-strapped or re-wired module still works without a rebuild.
static uint8_t adxlAddr = ADXL_ADDRESS;
static const uint8_t ADXL_ADDRESSES[] = {0x53, 0x1D};

static uint8_t adxlWrite8(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(adxlAddr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission();
}

static bool adxlRead8(uint8_t reg, uint8_t& out) {
    Wire.beginTransmission(adxlAddr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;   // repeated start
    if (Wire.requestFrom((uint8_t)adxlAddr, (uint8_t)1) != 1) return false;
    out = Wire.read();
    return true;
}

// Apply the sensor register config. Returns false if any I2C step NACKs, so a
// bus hiccup cannot leave the part in a half-configured (standby) state.
static bool adxlConfigure() {
    if (adxlWrite8(ADXL_REG_DATA_FORMAT, ADXL_DATA_FORMAT_FULLRES_2G) != 0) return false;
    if (adxlWrite8(ADXL_REG_BW_RATE, ADXL_BW_RATE) != 0) return false;
    if (adxlWrite8(ADXL_REG_POWER_CTL, ADXL_POWER_CTL_MEASURE) != 0) return false;  // exit standby last
    delay(10);
    return true;
}

static bool adxlProbe(uint8_t addr) {
    adxlAddr = addr;
    uint8_t id = 0;
    return adxlRead8(ADXL_REG_DEVID, id) && id == ADXL_DEVID_EXPECTED;
}

static bool adxlBegin() {
    uint8_t id = 0;
    if (!adxlRead8(ADXL_REG_DEVID, id) || id != ADXL_DEVID_EXPECTED) {
        Serial.printf("[IMU] ADXL345 not ready! DEVID=0x%02X (expected 0x%02X)\n",
                      id, ADXL_DEVID_EXPECTED);
        return false;
    }
    if (!adxlConfigure()) {
        Serial.println("[IMU] ADXL345 NACK during register config");
        return false;
    }
    return true;
}

// ── Presence watchdog ─────────────────────────────────────────────────────────
// ADXL345 powers up in standby until POWER_CTL is written. If the bus or the
// sensor is slow to come up at boot (other device on the bus, marginal pull-ups),
// the first init fails and the sensor stays unconfigured forever — that is the
// "works sometimes, sometimes not" failure. So the watchdog not only checks the
// DEVID, it also RE-APPLIES the full config and re-seeds the rolling baseline
// when the part reconnects or was never configured.
static bool adxlPresent = false;
static uint32_t lastIdcCheck = 0;
static uint32_t consecutiveBusErrors = 0;

static bool isAdxlBusAlive() {
    uint8_t id = 0;
    return adxlRead8(ADXL_REG_DEVID, id) && id == ADXL_DEVID_EXPECTED;
}

static int32_t lastGoodX = 0, lastGoodY = 0, lastGoodZ = 0;
static void readAxes(bool& ok, int32_t& ax, int32_t& ay, int32_t& az);
static void refreshBaseline();

static void checkSensorPresent() {
    uint32_t now = millis();
    uint32_t wait = adxlPresent ? 2000 : 200;   // fast re-try while absent
    if (now - lastIdcCheck < wait) return;
    lastIdcCheck = now;

    if (adxlPresent) {
        if (!isAdxlBusAlive()) {
            adxlPresent = false;
            consecutiveBusErrors = 0;
            Serial.println("[IMU] ADXL345 LOST (check wiring/pull-ups)");
        }
    } else {
        // Re-probe both possible addresses before re-init: the module may have
        // been re-seated or strapped differently while the bus was down.
        for (uint8_t a : ADXL_ADDRESSES) {
            adxlAddr = a;
            if (adxlBegin()) {
                adxlPresent = true;
                Serial.printf("[IMU] ADXL345 reconnected at 0x%02X; reconfiguring\n", adxlAddr);
                // Re-seed the rolling baseline so the first post-reconnect samples
                // are not read against stale 0-average buffers (would fake a huge
                // magnitude).
                refreshBaseline();
                Serial.println("[IMU] ADXL345 baseline refreshed");
                break;
            }
        }
    }
}

static void refreshBaseline() {
    // Device must be still. Rolling buffers are re-seeded so the first readings
    // after a reconnect are not mistaken for a vibration burst.
    int64_t sx = 0, sy = 0, sz = 0;
    int okSamples = 0;
    for (int i = 0; i < 80; i++) {
        int32_t ax, ay, az;
        bool ok;
        readAxes(ok, ax, ay, az);
        if (ok) { sx += ax; sy += ay; sz += az; okSamples++; }
        delay(10);
    }
    if (okSamples == 0) { okSamples = 1; }
    baseX = (int32_t)(sx / okSamples);
    baseY = (int32_t)(sy / okSamples);
    baseZ = (int32_t)(sz / okSamples);
    for (int i = 0; i < ROLL_AVG_SAMPLES; i++) {
        bufX[i] = baseX; bufY[i] = baseY; bufZ[i] = baseZ;
    }
    lastGoodX = baseX; lastGoodY = baseY; lastGoodZ = baseZ;
}

bool imuSensorPresent() { return adxlPresent; }

// ── helpers ───────────────────────────────────────────────────────────────────

static void readAxes(bool& ok, int32_t& ax, int32_t& ay, int32_t& az) {
    Wire.beginTransmission(adxlAddr);
    Wire.write(ADXL_REG_DATAX0);
    if (Wire.endTransmission(false) != 0) { ok = false; return; }
    // requestFrom returns the actual number of bytes; a partial/interrupted
    // transfer must NOT be parsed into garbage axis values (a single bad read
    // can fake a huge vibration spike and miscount a stitch).
    uint8_t got = Wire.requestFrom((uint8_t)adxlAddr, (uint8_t)6);
    if (got < 6) { ok = false; return; }

    uint8_t b[6];
    for (int i = 0; i < 6; i++) b[i] = Wire.read();

    int16_t rx = (int16_t)((b[1] << 8) | b[0]);
    int16_t ry = (int16_t)((b[3] << 8) | b[2]);
    int16_t rz = (int16_t)((b[5] << 8) | b[4]);

    lastGoodX = ax = (int32_t)rx * ADXL_MAG_GAIN;
    lastGoodY = ay = (int32_t)ry * ADXL_MAG_GAIN;
    lastGoodZ = az = (int32_t)rz * ADXL_MAG_GAIN;
    ok = true;
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
    return (int32_t)sqrt((double)(dx*dx + dy*dy + dz*dz));
}

// ── public ────────────────────────────────────────────────────────────────────

void imuInit() {
    // Hard timeout so a hung/stretched bus cannot block the loop indefinitely.
    // Also enforce a minimum bus frequency: ESP32 Wire defaults to 100 kHz.
    Wire.setTimeOut(50);

    // Resolve which address the module is strapped to (SDO low → 0x53, high → 0x1D).
    for (uint8_t a : ADXL_ADDRESSES) {
        if (adxlProbe(a)) {
            Serial.printf("[IMU] ADXL345 found at 0x%02X\n", adxlAddr);
            break;
        }
    }

    // Retry init for a few seconds: ADXL345 may power up late, especially when
    // another I2C device shares the bus and the first DEVID read races power-on.
    adxlPresent = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (adxlBegin()) { adxlPresent = true; break; }
        Serial.println("[IMU] Init failed, retrying...");
        delay(200);
    }
    if (!adxlPresent) {
        Serial.println("[IMU] ADXL345 not ready; will keep retrying in background");
        return;
    }

    Serial.println("[IMU] Calibrating baseline (keep device still)...");
    refreshBaseline();
    Serial.printf("[IMU] Baseline: X=%ld Y=%ld Z=%ld\n",
                  (long)baseX, (long)baseY, (long)baseZ);
}

static int32_t prevMag = 0;
static uint32_t ignoreUntilMs = 0;

bool imuUpdate(const VibConfig& cfg, bool* vibActiveOut) {
    checkSensorPresent();

    int32_t ax, ay, az;
    bool readOk = false;
    if (adxlPresent) {
        readAxes(readOk, ax, ay, az);
        if (!readOk) {
            // Bus hiccup (NACK / partial transfer). Use the last-good sample so
            // one bad read cannot corrupt the magnitude or miscount a stitch.
            consecutiveBusErrors++;
            ax = lastGoodX; ay = lastGoodY; az = lastGoodZ;
            if (consecutiveBusErrors >= 5) {
                adxlPresent = false;
                lastIdcCheck = 0;      // force the watchdog to retry on the next tick
                Serial.printf("[IMU] %lu consecutive I2C errors; re-initializing\n",
                              (unsigned long)consecutiveBusErrors);
            }
        } else {
            consecutiveBusErrors = 0;
        }
    } else {
        ax = lastGoodX; ay = lastGoodY; az = lastGoodZ;
    }

    int32_t avgX, avgY, avgZ;
    rollingAvgAxes(ax, ay, az, avgX, avgY, avgZ);

    // Calculate magnitude of the AC component (deviation from average)
    int32_t mag = vibrMagnitude(ax, ay, az, avgX, avgY, avgZ);
    int32_t deltaMag = abs(mag - prevMag);
    prevMag = mag;
    lastMag = mag;

    uint32_t now   = millis();
    static uint32_t lastUpdateMs = now;
    uint32_t deltaMs = now - lastUpdateMs;
    lastUpdateMs = now;

    // If there is an extremely rapid magnitude change, it's a physical shock (solenoid)
    if (deltaMag > 15000) {
        ignoreUntilMs = now + 100; // Ignore all vibration for 100ms to let shockwave pass
        Serial.printf("[IMU] Massive rapid change (%ld) detected! Ignoring shockwave.\n", (long)deltaMag);
    }

    bool vibrating;
    if (now < ignoreUntilMs) {
        vibrating = false;
    } else if (state == SewState::VIBRATING || state == SewState::CONFIRMED || state == SewState::COOLING) {
        vibrating = (mag >= cfg.stopThreshold);
    } else {
        vibrating = (mag >= cfg.threshold);
    }
    
    static uint32_t lastVibratingMs = 0;
    if (vibrating) {
        lastVibratingMs = now;
    }
    
    bool counted   = false;

    if (vibActiveOut) *vibActiveOut = (state == SewState::VIBRATING ||
                                       state == SewState::CONFIRMED);

    switch (state) {
        case SewState::IDLE:
            if (vibrating) {
                state        = SewState::VIBRATING;
                stateEnterMs = now;
                lastVibStartMs = now;
            }
            break;

        case SewState::VIBRATING:
            if (!vibrating) {
                if ((now - lastVibratingMs) > cfg.dropoutMs) {
                    state = SewState::IDLE;   // dropped before min duration
                } else {
                    stateEnterMs += deltaMs;  // Pause the timer during dropout
                }
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
                lastVibEndMs = now;
                if (vibActiveOut) *vibActiveOut = false;
            }
            break;

        case SewState::COOLING:
            if (vibrating) {
                state        = SewState::CONFIRMED;  // resumed same cycle
                stateEnterMs = now;
                lastVibStartMs = now;
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

uint32_t imuGetLastVibStart() { return lastVibStartMs; }
uint32_t imuGetLastVibEnd()   { return lastVibEndMs; }
int imuGetState() { return (int)state; }
