#include "display_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "config.h"
#include "coulomb_counter.h"
#include "data_logger.h"
#include "settings_manager.h"
#include "connectivity_manager.h"

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

    char pbuf[8];
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

static void draw_channel_page(uint8_t ch, const SensorData& data) {
    float v, i, p;
    char name[24] = "";
    VirtualChannelConfig vc;
    bool has_vc = settings_load_virtual_channel(ch, &vc);

    if (has_vc && (vc.voltage_src > 0 || vc.current_src > 0)) {
        // Use virtual channel sources
        v = (vc.voltage_src > 0) ? get_sensor_voltage(vc.voltage_src, vc.voltage_idx, data) : 0.0f;
        i = (vc.current_src > 0) ? get_sensor_current(vc.current_src, vc.current_idx, data) : 0.0f;
        if (vc.current_src == 3) {
            p = get_sensor_power(vc.current_src, vc.current_idx, data);  // INA226 has built-in power
        } else if (vc.voltage_src > 0 && vc.current_src > 0) {
            p = v * i;
        } else {
            p = 0.0f;
        }
        settings_load_channel_name(ch, name, sizeof(name));
        if (!name[0]) {
            snprintf(name, sizeof(name), "CH%d", ch);
        }
    } else if (ch < 3) {
        // Default: hardcoded mapping for backward compat
        v = data.ads1115_volts[ch];
        i = data.ina3221_current[ch];
        p = v * i;
        settings_load_channel_name(ch, name, sizeof(name));
        if (!name[0]) {
            if (ch == 0) strlcpy(name, "Battery", sizeof(name));
            else if (ch == 1) strlcpy(name, "Solar", sizeof(name));
            else strlcpy(name, "Output", sizeof(name));
        }
    } else {
        v = data.ina226_busV; i = data.ina226_current; p = data.ina226_power;
        strlcpy(name, "INA226", sizeof(name));
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
    float mAh = get_coulomb_mAh(ch);
    BatteryConfig bat;
    float soc = -1;
    if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
        soc = bat.initial_soc_pct + (mAh / bat.capacity_mAh) * 100.0f;
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
        if (fabsf(mAh) < 1000.0f) {
            display->print(mAh, 0);
        } else {
            display->print(mAh / 1000.0f, 2);
        }
    }
}

// ─── Main display loop ───────────────────────────────────────────

void update_display(const SensorData& data, const char* ip_str, float total_power) {
    if (!display) return;
    unsigned long now = millis();
    if (now - last_page_switch >= 3000) {
        current_page = (current_page + 1) % 4;
        last_page_switch = now;
    }
    display->clearDisplay();
    display->setTextSize(1);
    display->setTextColor(SSD1306_WHITE);
    if (current_page == 0) draw_status_page(ip_str, total_power, temperatureRead());
    else draw_channel_page(current_page - 1, data);
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
        Serial.println("OLED alloc failed");
        return;
    }

    // begin() MUST be called before any drawing — it allocates the internal buffer
    if (!display->begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("OLED init failed");
        delete display;
        display = nullptr;
        return;
    }

    // Now safe to draw
    display->clearDisplay();
    display->setTextSize(1);
    display->setTextColor(SSD1306_WHITE);
    display->setCursor(0, 0);
    display->println("Init...");
    display->display();
    vTaskDelay(pdMS_TO_TICKS(500));

    display->clearDisplay();
    display->setTextSize(2);
    display->setTextColor(SSD1306_WHITE);
    display->setCursor(20, 20);
    display->println("Power");
    display->setCursor(20, 40);
    display->println("Monitor");
    display->display();
    vTaskDelay(pdMS_TO_TICKS(2000));
    wire_started = true;
}

#else
void init_display() {}
void update_display(const SensorData&, const char*, float) {}
#endif