#include "display_mgr.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

void displayInit() {
    if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
        Serial.println("[OLED] Init failed");
        return;
    }
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextWrap(false);

    // Splash
    oled.setTextSize(1);
    oled.setCursor(20, 10);
    oled.println("Label Counter");
    oled.setCursor(32, 24);
    oled.println("v1.0  Idea8");
    oled.drawRect(0, 0, 128, 64, SSD1306_WHITE);
    oled.display();
    delay(1500);
}

void displayShowPortal(const char* apName, const char* apIP) {
    oled.clearDisplay();
    oled.setTextSize(1);

    oled.setCursor(0, 0);
    oled.println("-- WiFi Setup --");
    oled.println();
    oled.println("Connect to AP:");
    oled.setTextSize(1);
    oled.println(apName);
    oled.println();
    oled.println("Then open:");
    oled.println(apIP);
    oled.display();
}

void displayShowConnecting(const char* ssid) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("Connecting to:");
    oled.println(ssid);
    oled.println();
    oled.println("Please wait...");
    oled.display();
}

void displayShowRunning(uint32_t count, const char* ip,
                        bool mqttEnabled, bool mqttOk, bool vibActive, uint8_t calibStatus) {
    oled.clearDisplay();

    // Big count
    oled.setTextSize(3);
    // centre the number
    char buf[10];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)count);
    int16_t x1, y1; uint16_t w, h;
    oled.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    oled.setCursor((OLED_WIDTH - w) / 2, 4);
    oled.print(buf);

    // Divider
    oled.drawFastHLine(0, 36, 128, SSD1306_WHITE);

    // Status row (IP)
    oled.setTextSize(1);
    oled.setCursor(0, 40);
    oled.print(ip ? ip : "No IP");

    // Bottom row layout: CAL | Sewing Bar | MQTT
    
    // 1. Calibration state (Left)
    oled.setCursor(0, 54);
    if (calibStatus == 1) {
        oled.print("CAL:OK");
    } else {
        oled.print("CAL:BAD");
    }

    // 2. Sewing indication bar (Middle)
    // Reduce width and place in middle (x=48, w=26)
    if (vibActive) {
        oled.fillRect(48, 54, 26, 8, SSD1306_WHITE);
    } else {
        oled.drawRect(48, 54, 26, 8, SSD1306_WHITE);
    }

    // 3. MQTT state (Right)
    oled.setCursor(80, 54);
    if (!mqttEnabled) {
        oled.print("MQTT:OFF");
    } else {
        oled.print(mqttOk ? "MQTT:OK " : "MQTT:ON ");
    }

    oled.display();
}

void displayShowError(const char* line1, const char* line2) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("!! ERROR !!");
    oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    oled.setCursor(0, 16);
    oled.println(line1);
    if (line2) {
        oled.setCursor(0, 30);
        oled.println(line2);
    }
    oled.display();
}

void displayShowMessage(const char* line1, const char* line2) {
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("== SYSTEM ==");
    oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    oled.setCursor(0, 20);
    oled.println(line1);
    if (line2) {
        oled.setCursor(0, 34);
        oled.println(line2);
    }
    oled.display();
}

void displayShowCfgIP(const char* ip) {
    // Small info strip at bottom without clearing rest of screen
    oled.fillRect(0, 54, 128, 10, SSD1306_BLACK);
    oled.setTextSize(1);
    oled.setCursor(0, 54);
    oled.print("CFG: ");
    oled.print(ip);
    oled.display();
}

void displayShowCalibration(uint8_t state, uint8_t ftCount, uint16_t progress, uint8_t imuState, uint8_t lockCount) {
    oled.clearDisplay();
    oled.setTextSize(1);
    
    // Header
    oled.setCursor(0, 0);
    oled.println("== CALIBRATION ==");
    oled.drawFastHLine(0, 10, 128, SSD1306_WHITE);
    
    oled.setCursor(0, 20);
    switch (state) {
        case 1: // CALIB_SAMPLING
            oled.println("Step 1: Noise Scan");
            oled.println("Keep machine off.");
            
            // Draw loading bar with 3 splits (width 120, height 10)
            oled.drawRect(4, 42, 120, 10, SSD1306_WHITE);
            oled.drawFastVLine(44, 42, 10, SSD1306_WHITE);
            oled.drawFastVLine(84, 42, 10, SSD1306_WHITE);
            
            if (progress > 0) {
                int fillW = (progress * 120) / 150; // max samples is 150
                if (fillW > 120) fillW = 120;
                oled.fillRect(4, 42, fillW, 10, SSD1306_WHITE);
            }
            break;
        case 3: // CALIB_LOCK_WAITING
            oled.println("Step 2: Lock Scan");
            oled.println("Trigger lock stitch");
            oled.setCursor(0, 34);
            oled.print("Count: ");
            oled.print(lockCount);
            oled.println(" / 3");
            break;
        case 4: // CALIB_SEW_WAITING
            oled.println("Step 3: First Sew");
            if (imuState == 3) {
                oled.setCursor(0, 32);
                oled.println("CONFIRMING...");
            } else {
                oled.println("Sew a normal label");
            }
            break;
        case 5: // CALIB_FINE_TUNE
            oled.println("Step 4: Fine Tune");
            oled.setCursor(0, 32);
            if (imuState == 3) {
                oled.println("CONFIRMING...");
            } else {
                oled.print("Attempt ");
                oled.print(ftCount);
                oled.println("/5");
            }
            break;
        case 6: // CALIB_SEW_DONE
            oled.println("Processing Data...");
            break;
        case 7: // CALIB_SKIP_LOCK
            oled.setCursor(0, 32);
            oled.println("NO LOCK SOLENOID");
            break;
        default:
            oled.println("Please Wait...");
            break;
    }
    
    // Footer hint for manual finish
    if (state == 4 || state == 5) {
        oled.setCursor(0, 54);
        oled.print("Press BTN to finish");
    }
    
    oled.display();
}
