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
                        bool mqttOk, bool vibActive) {
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

    // Status row
    oled.setTextSize(1);
    oled.setCursor(0, 40);
    oled.print(ip ? ip : "No IP");

    // MQTT dot
    oled.setCursor(100, 40);
    oled.print(mqttOk ? "MQ:OK" : "MQ:--");

    // Vibration activity bar
    oled.setCursor(0, 52);
    oled.print("SEW:");
    if (vibActive) {
        oled.fillRect(30, 53, 90, 8, SSD1306_WHITE);
    } else {
        oled.drawRect(30, 53, 90, 8, SSD1306_WHITE);
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

void displayShowCfgIP(const char* ip) {
    // Small info strip at bottom without clearing rest of screen
    oled.fillRect(0, 54, 128, 10, SSD1306_BLACK);
    oled.setTextSize(1);
    oled.setCursor(0, 54);
    oled.print("CFG: ");
    oled.print(ip);
    oled.display();
}
