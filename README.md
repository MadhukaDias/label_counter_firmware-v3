# Label Counter Firmware
**Platform:** XIAO ESP32-S3 + MPU-6050 + SSD1306 OLED  
**Framework:** PlatformIO / Arduino

---

## Hardware Wiring

| Signal       | XIAO Pin | Notes                        |
|--------------|----------|------------------------------|
| I2C SDA      | D1       | MPU-6050 + OLED shared bus   |
| I2C SCL      | D2       | MPU-6050 + OLED shared bus   |
| Button +1    | D3       | Pull-up, GND on press        |
| Button -1    | D4       | Pull-up, GND on press        |
| MPU-6050 AD0 | GND      | Sets I2C addr to 0x68        |
| MPU-6050 INT | NC       | Not used                     |

**I2C addresses:**
- MPU-6050 → `0x68`
- SSD1306 OLED → `0x3C`

---

## First Flash Steps

1. **Flash firmware:** `pio run -t upload`
2. **Flash filesystem (web portal):** `pio run -t uploadfs`
3. On first boot, device spins up AP: **`LabelCounter`**
4. Connect your phone/PC to that AP → browser opens to `192.168.4.1`
5. Enter your WiFi SSID + password → device reboots and connects
6. Note the IP shown on OLED → open `http://<ip>` for the config portal

---

## State Machine

```
IDLE ──[delta >= threshold]──► VIBRATING
VIBRATING ──[held >= minDuration]──► CONFIRMED
VIBRATING ──[drops before minDuration]──► IDLE
CONFIRMED ──[vibration stops]──► COOLING
COOLING ──[quiet >= silenceMs]──► IDLE + COUNT++
COOLING ──[vibration resumes]──► CONFIRMED  (same cycle)
```

---

## OLED Screens

| Situation         | Display shows                          |
|-------------------|----------------------------------------|
| WiFi setup AP     | AP name + IP `192.168.4.1`             |
| Connecting        | SSID name                              |
| Running (normal)  | Large count, IP, MQTT status, sew bar  |
| Error             | Error lines                            |

---

## Button Controls

| Press type        | Button INC (D3) | Button DEC (D4) |
|-------------------|-----------------|-----------------|
| Short press       | Count +1        | Count -1        |
| Long press (2s)   | Reset to 0      | Reset to 0      |

---

## MQTT

- **Broker:** `broker.emqx.io:1883` (public, no auth)
- **Publish topic:** `labelcounter/data`
- **Subscribe topic:** `labelcounter/config`
- **Payload:**
  ```json
  {"device":"LC-AABBCC","count":42,"timestamp":123456}
  ```
  `timestamp` = device uptime in ms. Replace with NTP epoch if needed.

---

## Web Config Portal

Available at `http://<device-ip>/` after WiFi connect.

- **Live stats:** count, vibration magnitude, MQTT status
- **Threshold tuning:** vibration threshold, min duration, silence window
- **Count reset:** zero the counter remotely

---

## Calibration Tips

| Symptom               | Fix                                    |
|-----------------------|----------------------------------------|
| Too many false counts | Increase `threshold` or `minDuration`  |
| Missing real sews     | Decrease `threshold`                   |
| Double-counting       | Increase `silenceMs`                   |
| Slow to register      | Decrease `minDuration`                 |

Use the **live vibration magnitude** reading in the web portal to find your machine's idle vs sewing values, then set the threshold between them.

---

## NVS Keys (Preferences namespace: `lc_cfg`)

| Key         | Type   | Default |
|-------------|--------|---------|
| `threshold` | int    | 800     |
| `minDur`    | uint   | 400     |
| `silence`   | uint   | 600     |
| `count`     | uint   | 0       |
| `deviceId`  | string | LC-XXYYZZ |
