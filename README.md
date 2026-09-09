# Label Counter — ESP32-S3-Nano TFT edition

Updated from the supplied label_counter_project.zip for the Waveshare ESP32-S3-Nano,
a 1.8-inch 128x160 ST7735 SPI TFT used in landscape (160x128), and four buttons.
This is a PlatformIO project using the Arduino framework, not a single .ino sketch.

## Wiring

All numbers in config.h are raw ESP32 GPIO numbers. Board labels are different.

| Peripheral pin | Nano label | GPIO |
|---|---|---:|
| TFT SCK | D13 | 48 |
| TFT SDA / MOSI | D11 | 38 |
| TFT CS | D10 | 21 |
| TFT A0 / DC | D9 | 18 |
| TFT RESET | D8 | 17 |
| TFT GND | GND | — |
| TFT LED | 3V3 | — |
| TFT VCC | See supply note below | — |
| ADXL345 SDA | A4 | 11 |
| ADXL345 SCL | A5 | 12 |
| Button INC | A3 | 4 |
| Button DEC | D2 | 5 |
| Button SELECT | D3 | 6 |
| Button BACK | D4 | 7 |

Connect each normally open button between its GPIO and GND. Internal pull-ups
are enabled; pressed = LOW. Share GND between the board, TFT, and sensor.
ADXL345 uses 3.3V-compatible power/logic; its configured I2C address is 0x53.
The TFT SDA label means SPI MOSI. TFT MISO and microSD connections are not used.

TFT supply: the module in the supplied Random Nerd Tutorials link uses VCC=5V
and LED=3.3V. Use the board's USB-powered VBUS for that 5V-input module only.
For a module rated 3.3V-only, use 3V3 for VCC instead. All GPIO signals are 3.3V.
Backlight wiring assumes the referenced module's onboard current limiting.

## Build and upload

1. Install VS Code and the PlatformIO IDE extension.
2. Open this folder (the folder containing platformio.ini).
3. Connect the board with a USB data cable.
4. Run PlatformIO Build, then Upload. Open Monitor at 115200 baud.

Upload uses esptool over the board's hardware USB CDC/JTAG port; esptool
auto-resets the board into the ROM bootloader, so no button press is needed.
If a build ever crashes in a boot loop badly enough that auto-reset misses,
hold BOOT/B1, tap RESET, release BOOT, then upload.

Equivalent terminal commands:

```sh
pio run -e esp32_s3_nano
pio run -e esp32_s3_nano -t upload
pio device monitor -b 115200
```

The build targets a generic ESP32-S3 module: 4MB flash, no PSRAM, hardware
USB CDC/JTAG (build flag ARDUINO_USB_MODE=1), matching the known-good Arduino
IDE settings ("ESP32S3 Dev Module", Flash 4MB, PSRAM Disabled). It uses the
min_spiffs 4MB partition table (1.9MB app x2, so ArduinoOTA still works) and
raw GPIO numbers. The pinned libraries are downloaded by PlatformIO.

setup() calls networkInit() before webServerInit()/mqttInit(): starting WiFi
brings up the LwIP TCP/IP stack, and binding a socket before that stack exists
panics the board on boot ("tcpip_send_msg_wait_sem ... Invalid mbox"). No filesystem upload is needed: the
web page is embedded in src/web_server_mgr.cpp. data/index.html is a copy.

The old auto_upload.py utility is included for reference but is not enabled:
USB upload is deterministic and does not inspect unrelated USB devices.
For OTA after connecting to Wi-Fi, explicitly configure upload_protocol=espota
and upload_port=<the displayed station IP> in platformio.ini, then upload.
Initial programming must use USB. Existing OTA behavior is retained.

For blank/offset/wrong-color panels, change TFT_TAB in include/config.h from
INITR_BLACKTAB to the panel's correct INITR_GREENTAB or INITR_REDTAB setting.
The default rotation is 1 (landscape), with an 8MHz SPI clock. Use rotation 3
for upside-down landscape. Portrait layouts would need a separate UI layout.

## Button controls

| Context | INC (GPIO4) | DEC (GPIO5) | SELECT (GPIO6) | BACK (GPIO7) |
|---|---|---|---|---|
| Running | +1 | -1, minimum zero | Open menu | Toggle network details / count |
| Menu | Previous item | Next item | Choose item | Exit |
| Reset confirmation | — | — | Confirm reset | Cancel |
| Noise scan | Ignored | Ignored | Ignored | Hold 2s to abort |
| Lock scan | Skip | Skip | Skip | Hold 2s to abort |
| First sew | Capture | Ignored | Capture | Hold 2s to abort |
| Fine tuning | Capture | Undo last capture | Capture | Hold 2s to abort |

Menu: Start calibration / Wi-Fi setup / Reset count.
Reset requires a second SELECT press. Holding INC or DEC alone no longer
resets the count. Automatic detection remains active while menu/network screens
are shown.

The original two-button shortcut remains: hold INC+DEC for 2 seconds to start
calibration, or 3 seconds to abort it. Staggered releases cannot cause an
accidental increment/decrement. Calibration captures do not increment the
production count. The existing detection thresholds/analysis remain in place.

## Display

The main screen shows only the live count, detector state, calibration status,
Wi-Fi/MQTT status, and IP information. There is no job, ID, target total, or
production progress bar. Calibration trial counts and its sampling bar remain.

- Detector: IDLE / DETECTING / SEWING / CONFIRMING. CONFIRMING represents the
  detector's COOLING quiet-time window before a count is accepted.
- Calibration: CAL OK / CAL NEEDED / CAL ABORT.
- MQTT: OK = enabled and connected; LOST = enabled but disconnected;
  OFF = disabled. WiFi LOST means the station is not connected, even if the
  device's setup access point is active.
- Main footer: IP for the connected station; AP plus actual portal IP while
  setup is active; reconnecting text before the fallback portal starts.
- Calibration screens: noise scan, lock scan (three detections), first sew,
  fine tuning (five captures total including first sew), processing, optional
  lock skip, invalid-data warning, completion, and cancellation.

The display uses the built-in Adafruit 6x8 font and a 160x128 RGB565 framebuffer.
Only changed scanline regions are transferred to the TFT; an unchanged frame
causes no SPI drawing. Text scales down for longer counts. No TFT_eSPI User_Setup
file is required.

### Display update functions

The display module owns the screen contents; call it only from the main task.
String arguments are copied, so temporary IP strings are safe.

```cpp
displayUpdateCount(count);
displayUpdateMachine(detectorState, calibrationStatus);
displayUpdateNetwork(wifiConnected, mqttEnabled, mqttConnected,
                     stationIP, portalActive, portalIP);
displayShowRunning();
displayRender();
```

Other screen functions: displayShowPortal, displayShowConnecting,
displayShowCalibration, displayShowMenu, displayShowMessage, displayShowError.
Timed notifications remain visible while the loop continues. The main loop
requests screen refreshes every 100ms; changed regions are sent on render.

## Wi-Fi and MQTT behavior

Startup attempts saved Wi-Fi credentials. After 10 seconds without connection,
the open setup AP `LabelCounter` starts. The actual AP IP appears on the TFT;
192.168.4.1 is typical. Connect a phone/computer to that AP and open the shown
address to enter Wi-Fi credentials. No fixed station IP is assumed.

The portal remains available until connected; there is no old 180-second
portal timeout. Loss of a working Wi-Fi connection also triggers fallback
after 10 seconds. Saved credentials are retried every 30 seconds, including
while the AP is up. The menu can open setup manually.

WiFiManager and the existing dashboard both use HTTP port 80. The dashboard
is paused during setup and resumed after the setup server stops. During setup,
the AP address serves Wi-Fi configuration, not the machine configuration UI.
BACK changes the displayed screen; it does not shut down the portal.
After connecting, open the station IP to use the existing machine dashboard.

Normal portal polling is nonblocking. WiFiManager's scan and credential-save
connection attempt can still pause the main loop (connection timeout set to
5 seconds); configure Wi-Fi while the machine is idle. The firmware is not a
hard real-time guarantee during network setup or OTA.

MQTT runs on its existing background task. It now respects the enabled switch
and actual Wi-Fi connectivity, starts once even after an offline boot, and
disconnects when disabled. Automatic counts publish at the configured interval;
manual corrections publish immediately when connected. Offline messages are
not durably queued. Existing broker/topic defaults are retained in config.h.
The browser MQTT status now uses LOST instead of the ambiguous ON state.

## Source map

- include/config.h: wiring, TFT variant/rotation/clock, sensor/MQTT defaults.
- include/display_mgr.h, src/display_mgr.cpp: reusable TFT rendering functions.
- include/button_input.h: debouncer with rollover-safe timers.
- src/main.cpp: button actions, menu, calibration integration, count updates.
- src/network_mgr.cpp: station connection and setup portal lifecycle.
- src/mqtt_mgr.cpp: background MQTT task and enable/connectivity gating.
- src/web_server_mgr.cpp: existing dashboard plus pause/resume and status labels.
- src/imu_detector.cpp: original ADXL345 detection algorithm.
- src/cfg_manager.cpp: original NVS configuration/count persistence.

The original NVS sanity limit of 99,999 is retained. Raising it requires changing
MAX_SANE_COUNT in config.h; values above that limit are reset on reboot.

## Tools

- docs/ui-flow.svg / .png: diagram of the on-device screen flow and the web dashboard.
- tools/mqtt_waveform_viewer.py: desktop viewer that subscribes to
  `labelcounter/+/data`, decodes the JSON waveform packets, and plots vibration
  magnitude against time (samples / IMU_SAMPLE_HZ). It also loads saved JSON/JSONL
  payloads offline and exports a selected waveform to CSV.

  ```sh
  pip install -r tools/requirements.txt
  python tools/mqtt_waveform_viewer.py            # then click Connect
  python tools/mqtt_waveform_viewer.py --connect  # connect to broker.emqx.io on start
  ```

## Firmware audit findings (bring-up review)

A bug/robustness audit of the current source identified the issues listed below.
All items were fixed in this update. Each entry notes the fix applied (and, where
a fix changed behavior, the tradeoff it intentionally makes).

### Critical

- **`src/main.cpp:78` — calibration buffer leak on re-entry.**
  `startCalibration()` now defensively frees any stale `calibBuffer`,
  `calibSewBuffer`, `attemptLowestPeaks`/`Medians`/`Durations` before allocating,
  so a desync between the state machine and allocation state cannot leak RAM.
- **`src/mqtt_mgr.cpp:187` — heap ownership crosses a FreeRTOS queue.**
  Confirmed the existing contract is sound: the producer frees `waveformData` when
  the queue is full (`:198`), the consumer frees after publish (`:104-106`). No
  code change required; ownership stays with whoever holds the struct copy.
- **`src/mqtt_mgr.cpp:129-137` — MQTT task stack may be too small for JSON.**
  Task stack raised 4096 → 8192 bytes to cover ArduinoJson serialization of a
  2000-element waveform array plus PubSubClient call chains. Stress-test waveform
  publishing on the unit.

### High

- **`src/main.cpp:615` / `:267` / `:633` — timing drift in interval checks.**
  IMU, MQTT heartbeat, and display intervals now advance with `lastXXX += interval`
  instead of `lastXXX = now`, so periods no longer drift late under load.
- **`src/main.cpp:602-606` — auto-counts not flushed to MQTT immediately.**
  Detector `counted` events now call `flushWaveformToMqtt("auto_count")` exactly
  like button inc/dec, so an interval-tick delay cannot drop the broker update.
- **`src/imu_detector.cpp:120` — float cast loses precision.**
  Magnitude uses `sqrt(double)` now; the `int64_t` sum-of-squares keeps full
  precision through the sqrt.
- **`src/web_server_mgr.cpp:619-626` — stopThreshold floor is 0.**
  `handlePostConfig` floors `stopThreshold` at the new threshold (and caps at
  8000) so the detector can still leave the VIBRATING state after a portal edit.
- **`src/main.cpp:329-337` — stopThreshold derived without its own clamp.**
  Phase-1 calibration clamps `stopThreshold` to `[threshold, 8000]` after
  derivation.
- **`src/cfg_manager.cpp:81-84` — `cfgSaveCount` opens/closes NVS per press.**
  `cfgSaveCount` now coalesces writes: no-op when the value is unchanged, and at
  most one write per second. Tradeoff: a burst of presses risks at most 1s of
  count on a crash, which the MQTT heartbeat mirrors anyway.
- **`src/web_server_mgr.cpp:586` & `src/mqtt_mgr.cpp` — shared flags not atomic.**
  `_sewState`, `_vibActive`, `_mqttOk`, `_updated` are now `volatile` so a future
  move of the web server to its own task cannot read stale values.
- **`src/mqtt_mgr.cpp:119` — `setBufferSize(16384)` return unchecked.**
  `mqttInit` verifies the buffer size after setup and disables MQTT with an ERROR
  log if the 16KB allocation failed, instead of risking a later null deref.

### Medium

- **`src/main.cpp:548` — `thrTooHigh` compares the wrong value.**
  The check now compares the accumulated `appCfg.vib.threshold` against each of
  the five stored lows, instead of this attempt's local `newStart`.
- **No FreeRTOS Task Watchdog subscription in the MQTT task.**
  `mqttTaskRunner` now subscribes via `esp_task_wdt_add(NULL)` and calls
  `esp_task_wdt_reset()` each loop, so a hung `connect()`/`loop()` cannot stall
  a watchdog reboot.
- **`src/main.cpp:591-598` — unconditional per-sample Serial plotting (20ms).**
  The realtime serial plot is now gated behind `-DSERIAL_PLOT_ENABLE`.
  Add that build flag in `platformio.ini` only when you need the plotter.
- **`src/imu_detector.cpp:45-51` — sensor loss not detected after boot.**
  `imuUpdate` now re-checks DEVID every ~2s via `checkSensorPresent()` and
  exposes `imuSensorPresent()` so the UI can surface a loose sensor. On
  reconnect it re-applies the full register config **and** refreshes the rolling
  baseline, so a slow-awake sensor self-heals without a reboot.
- **`src/imu_detector.cpp` — hardcoded 0x53 only.**
  The driver now auto-probes both strapped addresses (0x53 with SDO low, 0x1D
  with SDO high) at init and on every reconnect, using whichever answers. A
  mis-strapped or re-wired module needs no rebuild.
- **`src/main.cpp:263` — OTA ownership unresolved vs web server.**
  `ArduinoOTA.begin()` binds network resources on the same core as the web
  server. Fix: confirm mDNS/TCP coexistence or sequence startup explicitly.

### Low

- **`src/main.cpp:534-541` — `lows` sorted but never read.**
  Confirmed the `std::sort(lows,...)` was already absent from the final build.
  No change needed.
- **`src/web_server_mgr.cpp:20-579` — `data/index.html` vs PROGMEM HTML drift.**
  Still open. Two copies of the dashboard must stay in sync via `update_html.py`;
  a manual edit to one is silently lost. Fix: single source of truth + generated
  header.
- **`src/main.cpp:116-118` — hardcoded `lastLockPeak = 22000` on lock skip.**
  `skipCalibPhase2` now derives the fallback from the Phase-1 profile:
  `max(threshold*5, 2000)`, and `max(lastCalibMax*20, that)` when a clean max was
  measured — no machine-blind constant.
- **`src/imu_detector.cpp:151` — `ignoreUntilMs` starts at 0 (benign).**
  Only the first 100ms of boot is not shock-filtered. No action needed.
- **`src/cfg_manager.cpp:30-38` — `prefs.getString` heap allocation on boot.**
  Corrupt/large NVS strings could pressure the heap. Minor.

### Open availability/reliability recommendations

1. **Heap guardrail**: warn or clamp when `ESP.getFreeHeap()` drops low;
   abort calibration (which allocates ~26KB) below a safe floor.
2. **Graceful degradation**: surface `imuSensorPresent()` on the TFT / portal
   (the Getters exist now; the UI wiring is optional).
3. **Docs-kept-in-sync**: keep `data/index.html` and the PROGMEM copy identical.

## Verification and limits

Host tests pass for debounce, timer rollover, the actual four-button handlers,
count limits, chord release suppression, capture/undo, reset confirmation,
network fallback/retry/recovery and server ownership, display text/rectangle
bounds, ten-digit display values, and unchanged-frame transfer suppression.

Run on a host with a C++ compiler:

```sh
sh tests/run_host_tests.sh
```

Host mocks simulate GPIO, time, Wi-Fi, and drawing; they are not ESP32 drivers
and are never compiled into firmware. They do not validate the actual TFT font,
SPI transfer, RF behavior, sensor response, or library binary compatibility.

### Live bring-up (verified on hardware)

The updated firmware builds clean and was flashed + verified on the unit
(`LC-CD8510`); boot log confirms:

- NVS restore: `[CFG] Loaded: thr=858 minDur=250 sil=600 cnt=..`
- ADXL345 init + address auto-probe: `[IMU] ADXL345 found at 0x53`
- Baseline: `[IMU] Baseline: X=12791 Y=-9561 Z=4036`
- Web server + MQTT + station Wi-Fi connected, waveforms published
- Intermittent-I2C self-recovery exercised live (see `docs/i2c_debug.md` §6b)
- Serial plotter stream at 50 Hz (gated behind `-DSERIAL_PLOT_ENABLE=1`)

Not yet verified on hardware: TFT panel rendering, all four button handlers,
sensor counts through the calibration process, Wi-Fi fallback portal, OTA, and
long-run NVS wear behaviour. Verify these on the actual unit.

### Troubleshooting notes

- Serial console = the native USB CDC/JTAG port (`/dev/ttyACM*`). Close the
  Serial Monitor / plotter tab **before** flashing, or esptool fails with
  "device reports readiness to read but returned no data".
- `pio device monitor` needs an interactive TTY; headless, use a pySerial reader.
- ADXL345 address is auto-probed (0x53 / 0x1D); a persistent intermittent sensor
  is almost always pull-ups or address collision — see the I2C debug doc.
