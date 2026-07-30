#include "display_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "log_serial.h"
#include "coulomb_counter.h"
#include "data_logger.h"
#include "settings_manager.h"
#include "connectivity_manager.h"
#include "ui_manager.h"

#if HAS_DISPLAY
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

static Adafruit_SSD1306* display = nullptr;
static uint8_t current_page = 0;
static unsigned long last_page_switch = 0;
static bool wire_started = false;

// ─── SoC bar ─────────────────────────────────────────────────────

static void draw_soc_bar(int cx, int cy, int width, float soc) {
    display->drawRect(cx - width / 2, cy - 1, width, 3, SSD1306_WHITE);
    int fill = (int)((width - 2) * soc / 100.0f);
    if (fill > 0) {
        display->fillRect(cx - width / 2 + 1, cy, fill, 1, SSD1306_WHITE);
    }
}

// ─── Status page ───────────────────────────────────────────────

static void draw_status_page(const char* ip_str, float total_power, float temp_c) {
    display->setTextSize(1);
    display->setTextColor(SSD1306_WHITE);
    display->setCursor(0, 2);
    display->print(ip_str);

    char pbuf[12]; // Increased buffer size slightly for formatting safety
    dtostrf(total_power, 5, 1, pbuf);
    display->setTextSize(2);
    display->setCursor(0, 12);
    display->print(pbuf);
    display->setTextSize(1);
    display->setCursor(54, 14);
    display->print("W");

    display->setTextSize(1);
    display->setCursor(0, 30);
    display->print("LOG:");
    display->print(log_entries_count());
    display->print(" ent");
    if (log_has_overflow_file()) display->print(" [OVF]");

    // Temp + uptime on last line
    display->setCursor(0, 52);
    char tbuf[16];
    dtostrf(temp_c, 4, 1, tbuf);
    display->print(tbuf);
    display->print("C ");
    display->print((millis() / 1000) / 60);
    display->print("m");
}

// ─── Channel page ───────────────────────────────────────────────

static void draw_channel_page(uint8_t ch, float v, float i, float p, float charge_mAh) {
    // Moved to static allocation to protect the FreeRTOS stack from overflow
    static char name[24];
    static BatteryConfig bat;

    name[0] = '\0';
    settings_load_channel_name(ch, name, sizeof(name));
    if (!name[0]) {
        if (ch == 0) strlcpy(name, "Battery", sizeof(name));
        else if (ch == 1) strlcpy(name, "Solar", sizeof(name));
        else strlcpy(name, "Output", sizeof(name));
    }

    // Channel name in yellow band (y=2) + page number
    display->setTextSize(1);
    display->setTextColor(SSD1306_WHITE);
    display->setCursor(0, 2);
    display->print(name);
    display->setCursor(SCREEN_WIDTH - 24, 2);
    display->print("CH");
    display->print(ch);

    // V | I | P in blue area — below yellow band (y >= 16)
    char ibuf[16], pbuf[16];

    if (fabsf(i) < 1.0f) {
        snprintf(ibuf, sizeof(ibuf), "%d mA", (int)(i * 1000.0f));
    } else {
        snprintf(ibuf, sizeof(ibuf), "%.2f A", i);
    }

    if (fabsf(p) < 10.0f) {
        snprintf(pbuf, sizeof(pbuf), "%.2f W", p);
    } else {
        snprintf(pbuf, sizeof(pbuf), "%.1f W", p);
    }

    // V row at y=16, I row at y=26, P row at y=36
    display->setTextSize(1);
    display->setCursor(0, 16);
    display->print("V:");
    display->print(v, 2);
    display->print("V");

    display->setCursor(0, 26);
    display->print("I:");
    display->print(ibuf);

    display->setCursor(0, 36);
    display->print("P:");
    display->setTextSize(2);
    display->print(pbuf);
    display->setTextSize(1);

    // Bottom: SoC or mAh/Ah at y=50
    if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
        float soc = bat.initial_soc_pct + (charge_mAh / bat.capacity_mAh) * 100.0f;
        if (soc < 0) soc = 0;
        if (soc > 100) soc = 100;
        display->setTextSize(1);
        display->setCursor(0, 50);
        display->print("SoC ");
        display->print(soc, 0);
        display->print("%");
        draw_soc_bar(85, 57, 18, soc);
    } else {
        display->setTextSize(1);
        display->setCursor(0, 56);
        display->print("mAh:");
        if (fabsf(charge_mAh) < 1000.0f) {
            display->print(charge_mAh, 0);
        } else {
            display->print(charge_mAh / 1000.0f, 2);
        }
    }
}

// ─── Main display loop ───────────────────────────────────────────

void update_display(const TelemetrySnapshot& snap) {
    if (!display) return; // Guard clause against unallocated pointer references

    unsigned long now = millis();
    // A short press on Button 0 requests a page advance (ui_next_display_page
    // latches it). Honor it immediately and reset the auto-advance timer so the
    // user-chosen page stays visible for a full 3 s before cycling resumes.
    if (ui_next_display_page()) {
        current_page = (current_page + 1) % 5;
        last_page_switch = now;
    }
    if (now - last_page_switch >= 3000) {
        current_page = (current_page + 1) % 5;
        last_page_switch = now;
    }
    display->clearDisplay();

    if (current_page == 0) {
        // Status page
        float total_power = 0;
        for (int ch = 0; ch < snap.channel_count && ch < 4; ch++) {
            total_power += snap.channels[ch].P;
        }
        draw_status_page(snap.wifi.ip, total_power, temperatureRead());
    } else {
        uint8_t ch = current_page - 1;
        if (ch < snap.channel_count) {
            draw_channel_page(ch, snap.channels[ch].V, snap.channels[ch].I,
                             snap.channels[ch].P, snap.channels[ch].charge_mAh);
        }
    }
    display->display();
}

void init_display() {
    if (!wire_started) {
        Wire.begin(I2C_SDA, I2C_SCL);
        Wire.setClock(I2C_FREQ);  // 100KHz — bus must run at this speed
    }

    // Heap-allocate display — avoids static init order issues on ESP32-C3
    display = new Adafruit_SSD1306(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
    if (!display) {
        LOG_PRINTLN("OLED alloc failed");
        return;
    }

    // Fix: begin() must run completely before calling any drawing methods
    if (!display->begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        LOG_PRINTLN("OLED init failed");
        delete display;
        display = nullptr;
        return;
    }

    // Safely draw splash screen assets
    display->clearDisplay();
    display->setTextSize(1);
    display->setTextColor(SSD1306_WHITE);
    display->setCursor(0, 0);
    display->println("Init...");
    display->display();
    delay(500); // Changed from vTaskDelay to standard safe system delay

    display->clearDisplay();
    display->setTextSize(2);
    display->setTextColor(SSD1306_WHITE);
    display->setCursor(20, 20);
    display->println("Power");
    display->setCursor(20, 40);
    display->println("Monitor");
    display->display();
    delay(2000); // Changed from vTaskDelay to standard safe system delay
    
    wire_started = true;
}

#else
void init_display() {}
void update_display(const TelemetrySnapshot&) {}
#endif