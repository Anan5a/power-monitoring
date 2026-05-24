#include "display_manager.h"
#include "config.h"
#include "coulomb_counter.h"
#include "data_logger.h"
#include "settings_manager.h"

#if HAS_DISPLAY
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static uint8_t current_page = 0;
static unsigned long last_page_switch = 0;

static bool wire_started = false;

void init_display() {
    if (!wire_started) {
        Wire.begin(I2C_SDA, I2C_SCL);
        wire_started = true;
    }
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR, false)) {
        Serial.println("SSD1306 init failed");
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Power Monitor");
    display.display();
    last_page_switch = millis();
}

static void draw_status_page(const char* ip_str, float total_power) {
    display.setCursor(0, 0);
    display.println("Status");
    display.print("IP: "); display.println(ip_str);
    display.print("P: "); display.print(total_power, 1); display.println("W");
    display.print("Log: "); display.println(log_entries_count());
}

static void draw_channel_page(uint8_t ch, const SensorData& data) {
    float v, i, p;
    const char* label;
    if (ch < 3) {
        v = data.ina3221_busV[ch]; i = data.ina3221_current[ch]; p = v * i;
        label = (ch == 0) ? "Ch0" : (ch == 1) ? "Ch1" : "Ch2";
    } else {
        v = data.ina226_busV; i = data.ina226_current; p = data.ina226_power;
        label = "Ch3";
    }
    float mAh = get_coulomb_mAh(ch);
    float soc = -1;
    BatteryConfig bat;
    if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
        soc = bat.initial_soc_pct + (mAh / bat.capacity_mAh) * 100.0f;
        if (soc < 0) soc = 0;
        if (soc > 100) soc = 100;
    }
    display.setCursor(0, 0);
    display.println(label);
    display.print("V:"); display.print(v, 2); display.println("V");
    display.print("I:"); display.print(i, 3); display.println("A");
    display.print("P:"); display.print(p, 2); display.println("W");
    if (soc >= 0) {
        display.print("SoC:"); display.print(soc, 1); display.println("%");
    } else {
        display.print("mAh:"); display.print(mAh, 1);
    }
}

void update_display(const SensorData& data, const char* ip_str, float total_power) {
    unsigned long now = millis();
    if (now - last_page_switch >= 3000) {
        current_page = (current_page + 1) % 5;
        last_page_switch = now;
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    if (current_page == 0) draw_status_page(ip_str, total_power);
    else draw_channel_page(current_page - 1, data);
    display.display();
}

#else
void init_display() {}
void update_display(const SensorData&, const char*, float) {}
#endif
