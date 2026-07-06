# Smart Label Counter Firmware (v3)

A high-performance, dual-core IoT firmware for the ESP32-S3 designed to accurately count sewing machine labels using physical vibration analysis. The system features a responsive OLED display, a rich Web UI portal, automated machine-learning-style calibration, and real-time MQTT telemetry.

---

## 📌 Core Features

1. **Vibration-Based Counting:** Uses an I2C IMU (Inertial Measurement Unit) to detect the precise mechanical vibration patterns of a sewing machine, isolating the start and stop phases of sewing a single label.
2. **Automated 4-Phase Calibration:** Eliminates manual threshold guessing. The firmware dynamically profiles ambient noise, lock-stitches, and sewing rhythms to auto-configure optimal sensitivity and debounce windows.
3. **Dual-Core Architecture:** Sensor polling runs completely unimpeded on Core 1, while Wi-Fi and MQTT network operations are offloaded to a FreeRTOS background task on Core 0. This guarantees zero dropped sensor frames due to network latency.
4. **Rich Web UI & Captive Portal:** Connect to the device via Wi-Fi to access a real-time dashboard. Monitor vibration graphs, toggle machine settings, trigger calibrations, and view the live count.
5. **MQTT Telemetry:** Streams real-time counts and calibration events to an external MQTT broker for factory-wide data aggregation.

---

## 🛠️ Hardware Requirements

* **Microcontroller:** ESP32-S3 (e.g., Seeed Studio XIAO ESP32S3)
* **Sensor:** I2C IMU (e.g., MPU6050 or similar) for 3-axis vibration magnitude.
* **Display:** 0.96" I2C OLED Display (SSD1306).
* **Inputs:** Two physical push buttons (INC and DEC) for manual overrides and calibration triggers.
* **Optional:** Hardware Lock Solenoid (can be bypassed in settings).

---

## 🚀 How to Use the Device

### 1. Basic Operations & Counting
* **Automatic Counting:** Turn on the machine and begin sewing. The device will automatically detect the sewing vibration. Once the sewing stops and the "Silence Window" passes, the count will increment by 1.
* **Manual Increment (+):** Tap the `INC` button to manually add 1 to the count.
* **Manual Decrement (-):** Tap the `DEC` button to subtract 1.
* **Reset Count:** Press and hold either the `INC` or `DEC` button for **2 seconds** until the OLED confirms the reset.

### 2. Auto-Calibration Workflow
Because different sewing machines vibrate differently, the device must be calibrated. Calibration can be triggered via the Web UI or physically by holding **both buttons simultaneously for 2 seconds**.

The Calibration follows 4 strict phases:
* **Phase 1 (Noise Scan):** Do not touch the machine for 3 seconds. The device measures ambient floor vibrations to establish an absolute baseline zero.
* **Phase 2 (Lock Scan):** Manually actuate the lock-stitch solenoid 3 times. The firmware learns the specific spike signature of the lock stitch. *(Note: If "Lock Solenoid" is unchecked in the Web UI, this step is skipped).*
* **Phase 3 (First Sew):** Sew exactly 1 normal label. The system records the raw magnitude array, extracts the middle 50% (the purest sewing signal), and establishes initial `Start` and `Stop` vibration thresholds.
* **Phase 4 (Fine Tuning):** Sew 4 more consecutive labels. The system analyzes the variance between these 4 attempts and the initial sew to dial in the perfect "Silence Window" (debounce gap) and finalize the tolerances. 

**Aborting Calibration:** If you mess up a sew, you can hold **both buttons down for 3 seconds** to abort the calibration and restore the last saved settings.

### 3. The Web UI Portal
If the device cannot find a saved Wi-Fi network, it will broadcast its own Access Point. Otherwise, it connects to your local network.
* Type the device's IP address into a web browser.
* **Dashboard:** View the live count, current machine state (IDLE, SEWING, DONE), and a live-updating bar graph of vibration magnitude.
* **Settings:** Toggle MQTT, bypass the Lock Solenoid requirement, or view the calculated silence window.
* **Calibration:** You can monitor the 4-phase calibration process in real-time on the Web UI, complete with progress bars and instructions.

---

## 🧠 Firmware Architecture & Modules

The codebase is heavily modularized to maintain high performance.

### `main.cpp`
The orchestrator. It runs the primary Arduino `loop()`. This loop is strictly optimized for speed: it reads the IMU, updates the sewing state machine (`SEW_IDLE` -> `SEW_ACTIVE` -> `SEW_DONE`), handles button debouncing, and manages the Calibration state machine. 

### `imu_mgr.cpp`
The mathematical core. It polls the IMU via I2C at high speeds, calculates the Euclidean magnitude of the X, Y, and Z axes, and applies a low-pass filter to smooth out errant electrical noise. It then compares this magnitude against the dynamically calibrated `appCfg.vib.threshold` to determine if the machine is actively sewing.

### `display_mgr.cpp`
Manages the I2C SSD1306 OLED. It handles standard operation layouts (Top: Count, Bottom: Status bar with `CAL:OK`, `MQTT:ON`, and a visual sewing indicator) as well as the dynamic UI layouts during the calibration phases.

### `web_server_mgr.cpp`
An asynchronous-style ESP32 Web Server. It serves the `index.html` file (compiled into a raw string via `update_html.py`) and exposes a RESTful JSON API (`/api/status`, `/api/calib_status`) that the frontend JavaScript polls at 10Hz to achieve "real-time" dashboard updates without reloading the page.

### `mqtt_mgr.cpp` (FreeRTOS)
A dual-core networking engine. Standard MQTT libraries block the CPU while sending TCP packets over Wi-Fi, which would cause the IMU to drop frames and miss sewing events. To solve this, `mqtt_mgr` spawns a FreeRTOS task on **Core 0**. 
When `main.cpp` (running on Core 1) wants to publish a label count or calibration event, it instantly pushes the data into a thread-safe memory queue and moves on. Core 0 processes that queue and handles the slow network transmission entirely in the background.

### `config.cpp`
Handles Non-Volatile Storage (NVS). It saves the user's label count and all mathematically derived calibration thresholds directly to the ESP32's flash memory so they survive reboots and power losses.

---

## 💻 Development & Deployment

1. **Modifying the Web UI:** If you change the frontend layout in `data/index.html`, you **must** run the `update_html.py` Python script before building. This script minifies the HTML and injects it directly into `web_server_mgr.cpp` as a raw C++ string to save memory.
2. **Uploading:** The project uses PlatformIO. When you click Upload, `auto_upload.py` will dynamically check if a USB Serial cable is connected. If a cable is found, it uploads via Serial. If no cable is found but the device is powered and on Wi-Fi, it will automatically route the firmware update Over-The-Air (OTA).
