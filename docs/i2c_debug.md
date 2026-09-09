# ADXL345 I2C debug — findings & toolkit

Session findings for the Waveshare ESP32-S3-Nano + ADXL345 I2C bring-up.
This project (`label_counter_project_updated`) is a PlatformIO / Arduino-framework
sewing-machine stitch counter that already integrates the ADXL345 as a vibration
detector. This doc captures the environment, the verified tool inventory, the
wiring/pin map, the fault-isolation procedure, and the live bring-up results
(including the intermittent-probe root cause and its fix).

## 1. Project context (where ADXL345 lives)

| Item | Location |
|---|---|
| I2C pins (SDA/SCL) | `include/config.h`: `PIN_SDA=11`, `PIN_SCL=12` |
| ADXL345 I2C address | `include/config.h`: `ADXL_ADDRESS=0x53` primary. Driver auto-probes **both** 0x53 (SDO low) and 0x1D (SDO high) at init and on every reconnect |
| ADXL345 gain (+G) | `include/config.h`: `ADXL_MAG_GAIN=64` |
| IMU sample rate | `include/config.h`: `IMU_SAMPLE_HZ=50`, `ROLL_AVG_SAMPLES=8` |
| Sensor driver | `src/imu_detector.cpp` |
| Driver header | `include/imu_detector.h` |
| Fail points | `src/imu_detector.cpp` — DEVID mismatch, config NACK, partial reads, bus errors all handled |
| Wire init | `src/main.cpp:231` `Wire.begin(PIN_SDA, PIN_SCL)` |
| IMU init call | `src/main.cpp:242` `imuInit()` |

Firmware bring-up state: hardware IS now connected and verified (see §8). The
I2C layer was hardened during this session (see §6b).

## 2. Verified environment / tool inventory

| Tool | Path / version | Notes |
|---|---|---|
| ESP-IDF | `~/.espressif/v6.0.1/esp-idf` (v6.0.1) | Activate: `. ~/.espressif/v6.0.1/esp-idf/export.sh` then `idf.py --version` |
| ESP-IDF (extra) | `~/.platformio/packages/framework-espidf@3.50503.0` & `framework-espidf` (6.0.1, 5.5.3 venvs) | PlatformIO-managed copies |
| openocd-esp32 (JTAG) | `~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260304/bin/openocd` | For live register inspection via native USB-JTAG |
| PlatformIO | `espressif32@6.13.0` (project pins `6.9.0`) | Board env `esp32_s3_nano` (Arduino framework) |
| Host i2c-tools | `i2cdetect/i2cget/i2cset/i2cdump` | **HOST-ONLY** — talks to host `/dev/i2c-*`, cannot probe the ESP32 bus |

> Note on i2c-tools: it is present on this host (`/dev/i2c-0..14`) but those are
> host Linux I2C buses, NOT the ESP32's I2C. Do not try to use `i2cdetect` to
> find the ADXL345 attached to the ESP. The ESP32-side equivalent is an on-chip
> bus scan (Wire probe loop, snippet below).

## 3. Board bring-up status (live session)

- **Board CONNECTED** → `/dev/ttyACM1` (Espressif `VID:PID 303A:1001`, native USB
  CDC/JTAG serial console). Verify:
  ```sh
  ls /dev/ttyACM*
  # → /dev/ttyACM1
  ```
- The ESP32-S3 uses **native USB CDC/JTAG**, so the console doubles as the flash
  port. Upload = `esptool` over hardware CDC (auto-resets into ROM bootloader).
- **Port contention warning**: opening the port in two things at once (e.g. VS
  Code Serial Monitor + `esptool`) breaks uploads with
  `device reports readiness to read but returned no data`. Close the monitor
  before flashing.
- **Monitor under this environment**: `pio device monitor` fails with a termios
  `Inappropriate ioctl for device` (no TTY available). Use Python to read the
  console instead:
  ```python
  import serial, time
  s = serial.Serial('/dev/ttyACM1', 115200, timeout=1)
  s.setDTR(False); time.sleep(0.1); s.setDTR(True)   # USB reset
  time.sleep(0.5)
  t = time.time()
  while time.time() - t < 10:
      d = s.readline()
      if d: print(d.decode('utf-8', 'replace'), end='')
  ```
  Reset-to-log: `rst:0x15 (USB_UART_CHIP_RESET)` followed by `[BOOT] Label Counter...`.
- Serial plot lines are gated behind `-DSERIAL_PLOT_ENABLE=1` in `platformio.ini`
  (`build_flags`). Feed the `>Magnitude:` stream to the Arduino Serial Plotter.

## 4. Wiring reference (matches config.h)

All numbers are raw ESP32 GPIO. Board labels differ.

| ADXL345 pin | Nano label | ESP32 GPIO |
|---|---|---|
| VCC | 3V3 | — |
| GND | GND | — |
| SDA | A4 | **11** |
| SCL | A5 | **12** |
| SDO | GND | — (→ address 0x53) |

- ADXL345 is 3.3V logic — do not feed 5V to SDA/SCL.
- Address: SDO→GND = **0x53**, SDO→3V3 = 0x1D. The driver auto-probes both, so a
  mis-strapped module still works without a rebuild (`ADXL345 found at 0x53`).
- Pull-ups: breakout boards usually have onboard 4.7k–10k on SDA/SCL. A **bare**
  chip needs external 4.7k SDA→3V3 and SCL→3V3. Missing pull-ups = classic
  "no ACK / DEVID=0x00" and is a common cause of **intermittent** I2C.
- If a **second** I2C device shares the bus: it must not respond at the same
  address as the ADXL345, and it also needs pull-ups (one set per bus is enough,
  but two parts with none = none).
- Share GND between board, TFT, and sensor.

> Derived independently from the Arduino Nano ESP32 pin map (variant
> `arduino_nano_nora`, SDA=A4, SCL=A5) and confirmed identical to the values
> already in `include/config.h`.

## 5. ADXL345 register quick ref

| Reg | Addr | Expected | Purpose |
|---|---|---|---|
| DEVID | 0x00 | **0xE5** | identity check on init |
| BW_RATE | 0x2C | 0x0B (200 Hz) | output data rate |
| POWER_CTL | 0x2D | 0x08 (measure) | exit standby |
| DATA_FORMAT | 0x31 | 0x08 (full-res ±2g) | range/format |
| DATAX0..Z1 | 0x32–0x37 | — | 6 accel bytes, LSB first |

## 6. Fault-isolation decision tree

1. **Board present?**
   `lsusb`, `ls /dev/ttyACM*`. If absent → USB cable/port/board; solve first.
2. **Console output on boot?**
   `pio device monitor -b 115200` (or `idf.py monitor`). Look for the
   `[IMU] ADXL345 not found! DEVID=0x..` line (`imu_detector.cpp:56`).
   - If no `[IMU]` line at all → check `Wire.begin` / init ordering.
3. **Full bus scan (on-chip).** Probe addresses 0x08–0x77 with a Wire ACK probe;
   log an address table (i2cdetect-style). Expected ACK at **0x53**.
4. **No ACK @ 0x53 → hardware, in this order:**
   1. **Pull-ups** present? (bare chip needs 4.7k) — missing = no ACK.
   2. **SDA/SCL swapped?** — top wiring bug. Verify GPIO11↔SDA, GPIO12↔SCL.
   3. **Sensor powered?** — VCC=3V3, GND shared, not reversed polarity.
   4. **Slow the bus** — try **100 kHz** instead of 400 kHz (stuck/loaded bus,
      long wires, marginal pull-ups often respond to slower clock).
   5. **Address really 0x53?** — if SDO floating/high it may be 0x1D; probe both.
5. **ACK but DEVID wrong (not 0xE5)** → wrong chip at that address, or wiring/
   signal integrity; re-check power and pulls.
6. **Still stuck** → escalate to JTAG (openocd-esp32) for live register read while
   the Wire read runs.

## 6b. Intermittent I2C — root cause & hardening (live session)

Reproduced live: at boot the ADXL345 sometimes does not answer DEVID
(`DEVID=0x00`) — it loses the power-on race, especially with a second I2C device
on the bus or marginal pull-ups. The old driver treated one failed DEVID check as
fatal and **never configured the sensor**, leaving it stuck in standby forever:
"works sometimes, sometimes not" until reboot.

Fixes applied in `src/imu_detector.cpp`:

| # | Issue | Fix |
|---|---|---|
| 1 | No init retry; sensor slow to wake | `imuInit` retries 5×200 ms, then keeps re-probing in background every 200 ms while absent |
| 2 | First-fail left sensor in standby (POWER_CTL never written) | Watchdog re-applies the **full register config** on reconnect, not just DEVID |
| 3 | Partial 6-byte reads parsed as garbage → phantom counts | `readAxes` verifies `requestFrom` returned 6; on failure reuses last-good sample |
| 4 | Hung/stretched bus blocked loop forever | `Wire.setTimeOut(50)` in `imuInit` |
| 5 | Config write NACKs ignored → half-configured part | `endTransmission()` codes checked in `adxlConfigure()`; NACK aborts + retries |
| 6 | Address hard-coded 0x53 only | Auto-probe **0x53 and 0x1D** at init and on every reconnect (`ADXL345 found at 0x53`) |
| 7 | 5 consecutive read errors kept producing garbage | Error counter forces re-init via watchdog |
| 8 | Post-reconnect magnitude spike (stale 0-average buffers) | Reconnect refreshes rolling baseline (`refreshBaseline()`) |

Expected healthy boot console:

```
[IMU] ADXL345 found at 0x53
[IMU] Calibrating baseline (keep device still)...
[IMU] Baseline: X=12791 Y=-9561 Z=4036
[WEB] HTTP server started on port 80
[MQTT] Client ID: lc_LC-CD8510
[BOOT] Ready.
>Magnitude:606,...  
```

Slow-awake recovery (no reboot needed):

```
[IMU] ADXL345 not ready; will keep retrying in background
[BOOT] Ready.
[IMU] ADXL345 reconnected at 0x53; reconfiguring
[IMU] ADXL345 baseline refreshed
>Magnitude:149,...   ← noise floor restored
```

If you still see intermittent drops on hardware: pre-empt the most common
physical causes — confirm 4.7k–10k pull-ups on SDA/SCL, a solid shared GND, and
no second device ACKing at 0x53/0x1D.

## 7. Diagnostic snippets (reference only — not compiled into firmware)

On-chip bus scan (Arduino/Wire):

```cpp
#include <Wire.h>
#include "config.h"   // PIN_SDA=11, PIN_SCL=12

void i2cScan() {
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  Serial.println("\nI2C scan 0x08..0x77:");
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    uint8_t code = Wire.endTransmission();
    if (code == 0) {
      Serial.printf("0x%02X : present (addr 7-bit %u)\n", addr, addr);
      // ADXL345 should appear at 0x53 (SDO low) or 0x1D (SDO high)
    } else if (code != 2) {   // 2 = normal NACK (address unpopulated)
      Serial.printf("0x%02X : err code %u\n", addr, code);
    }
  }
}
```

Wire `endTransmission()` return codes:
| Code | Meaning |
|---|---|
| 0 | success |
| 1 | data too long |
| 2 | NACK on address transmit (no device at that addr) |
| 3 | NACK on data transmit |
| 4 | other error |

Key register check:

```cpp
uint8_t devid_read = 0;
Wire.beginTransmission(ADXL_ADDRESS);
Wire.write(0x00);                       // DEVID
Wire.endTransmission(false);            // repeated start
Wire.requestFrom((uint8_t)ADXL_ADDRESS, (uint8_t)1);
if (Wire.available()) devid_read = Wire.read();
// expect 0xE5
```

## 8. Results log

Live bring-up on real hardware (`LC-CD8510`, NVS config `thr=858 minDur=250 sil=600`).

| Date | Test | Result |
|---|---|---|
| 2026-09-09 | Flash via USB CDC/JTAG (`/dev/ttyACM1`, 921600) | PASS — 919 KB in ~8 s, hash verified |
| 2026-09-09 | Boot console | PASS — `[BOOT] Ready.`, count restored from NVS |
| 2026-09-09 | ADXL345 init | PASS — `ADXL345 found at 0x53`, baseline `X=12791 Y=-9561 Z=4036` |
| 2026-09-09 | Address auto-probe | PASS — resolves 0x53 (SDO low) |
| 2026-09-09 | Serial plot stream | PASS — `>Magnitude:` at 50 Hz (noise floor ~0–600) |
| 2026-09-09 | Slow-awake recovery | PASS — DEVID=0x00 at boot → background retry → `reconnected at 0x53; baseline refreshed`, noise floor restored |
| 2026-09-09 | MQTT publish | PASS — `Published 1655 bytes to labelcounter/LC-CD8510/data` |
| 2026-09-09 | WiFi connect | PASS — `Connected: 192.168.1.48` |
| 2026-09-09 | Host tests (4 suites) | PASS — all green after I2C changes |

Notes: concurrent port open (VS Code monitor + esptool) causes upload failures —
close the monitor first. `pio device monitor` needs a TTY; use the Python reader
in §3 here when headless.

---
*Source: session investigation + live bring-up 2026-09-08/09. Firmware was
modified during this session (see §6b).*
