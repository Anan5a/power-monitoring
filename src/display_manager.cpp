#include "display_manager.h"
#include "config.h"
#include "coulomb_counter.h"
#include "data_logger.h"
#include "settings_manager.h"

#if 1 // HAS_DISPLAY
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

// ─── Tiny 8x8 bitmap helpers ────────────────────────────────────
// All icons are 8x8 pixels, drawn as filled shapes using GFX methods.
// To draw centered on a point (cx, cy) in an 8x8 grid: offset by -4,+4.

static void draw_battery_icon(int cx, int cy) {
    // Body
    display.drawRect(cx - 4, cy - 2, 8, 5, SSD1306_WHITE);
    // Tip
    display.drawRect(cx + 4, cy - 1, 1, 2, SSD1306_WHITE);
    // Charge bars
    display.fillRect(cx - 3, cy + 1, 2, 2, SSD1306_WHITE);
    display.fillRect(cx - 1, cy + 1, 2, 2, SSD1306_WHITE);
}

static void draw_solar_icon(int cx, int cy) {
    // Sun circle
    display.drawCircle(cx, cy, 2, SSD1306_WHITE);
    // Rays (4 directions)
    display.drawLine(cx - 4, cy, cx - 2, cy, SSD1306_WHITE);
    display.drawLine(cx + 2, cy, cx + 4, cy, SSD1306_WHITE);
    display.drawLine(cx, cy - 4, cx, cy - 2, SSD1306_WHITE);
    display.drawLine(cx, cy + 2, cx, cy + 4, SSD1306_WHITE);
    // Diagonal rays
    display.drawLine(cx - 3, cy - 3, cx - 2, cy - 2, SSD1306_WHITE);
    display.drawLine(cx + 2, cy - 2, cx + 3, cy - 3, SSD1306_WHITE);
    display.drawLine(cx - 3, cy + 3, cx - 2, cy + 2, SSD1306_WHITE);
    display.drawLine(cx + 2, cy + 2, cx + 3, cy + 3, SSD1306_WHITE);
}

static void draw_load_icon(int cx, int cy) {
    // Lightbulb base
    display.drawRect(cx - 2, cy, 4, 3, SSD1306_WHITE);
    // Filament arc
    display.drawCircle(cx, cy - 1, 2, SSD1306_WHITE);
    // Rays
    display.drawLine(cx - 4, cy - 2, cx - 3, cy - 1, SSD1306_WHITE);
    display.drawLine(cx + 3, cy - 1, cx + 4, cy - 2, SSD1306_WHITE);
    display.drawLine(cx - 4, cy + 2, cx - 3, cy + 1, SSD1306_WHITE);
    display.drawLine(cx + 3, cy + 1, cx + 4, cy + 2, SSD1306_WHITE);
}

static void draw_soc_bar(int cx, int cy, int width, float soc) {
    // Background bar
    display.drawRect(cx - width / 2, cy - 1, width, 3, SSD1306_WHITE);
    // Filled portion
    int fill = (int)((width - 2) * soc / 100.0f);
    if (fill > 0) {
        display.fillRect(cx - width / 2 + 1, cy, fill, 1, SSD1306_WHITE);
    }
}

// ─── Page helpers ───────────────────────────────────────────────

static void draw_header(const char* title, int page, int total) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.print(title);
    // Page indicator top-right
    display.setCursor(SCREEN_WIDTH - 24, 0);
    display.print("P.");
    display.print(page);
    display.print("/");
    display.print(total);
    // Separator line
    display.drawLine(0, 9, SCREEN_WIDTH - 1, 9, SSD1306_WHITE);
}

static void draw_big_value(float value, const char* unit, int x, int y) {
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(x, y);
    display.print(value, 2);
    display.setTextSize(1);
    display.setCursor(x, y + 18);
    display.print(unit);
}

static void draw_small_value(float value, const char* label, int x, int y) {
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(x, y);
    display.print(label);
    display.print(value, 3);
}

// ─── Status page ───────────────────────────────────────────────

static void draw_status_page(const char* ip_str, float total_power) {
    // BLUE area: single line with IP and total power
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 2);
    display.print("PWR MON");
    display.setCursor(50, 2);
    display.print(ip_str);

    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    char pbuf[8];
    dtostrf(total_power, 5, 1, pbuf);
    display.setCursor(0, 16);
    display.print(pbuf);
    display.setTextSize(1);
    display.setCursor(60, 16);
    display.print("W");

    // Divider
    display.drawLine(0, 37, SCREEN_WIDTH - 1, 37, SSD1306_WHITE);

    // YELLOW area: log count
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 42);
    display.print("LOG:");
    display.print(log_entries_count());
    display.print(" entries");
    if (log_has_overflow_file()) display.print(" [OVF]");
}

// ─── Channel page ───────────────────────────────────────────────

static void draw_channel_page(uint8_t ch, const SensorData& data) {
    float v, i, p;
    char name[24] = "";
    if (ch < 3) {
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

    // ── BLUE AREA (y=0..47): full-width single line ──────────────
    // "BATT  V: 48.23  I: 3.156  P: 152W"
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 2);
    display.print(name);

    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    // V label+value
    display.setCursor(0, 14);
    display.print("V:");
    display.setTextSize(2);
    display.print(v, 2);

    // I label+value
    display.setTextSize(1);
    display.setCursor(48, 14);
    display.print("I:");
    display.setTextSize(2);
    display.print(i, 3);

    // P label+value
    display.setTextSize(1);
    display.setCursor(96, 14);
    display.print("P:");
    display.setTextSize(2);
    char pbuf[8];
    dtostrf(p, 5, 1, pbuf);
    display.print(pbuf);

    // ── YELLOW BAR (y=50..63): SoC or mAh ─────────────────────────
    display.drawLine(0, 49, SCREEN_WIDTH - 1, 49, SSD1306_WHITE);
    float mAh = get_coulomb_mAh(ch);
    BatteryConfig bat;
    float soc = -1;
    if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
        soc = bat.initial_soc_pct + (mAh / bat.capacity_mAh) * 100.0f;
        if (soc < 0) soc = 0;
        if (soc > 100) soc = 100;
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 52);
        display.print("SoC");
        display.print(soc, 0);
        display.print("% ");
        draw_soc_bar(90, 57, 36, soc);
    } else {
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 52);
        display.print("mAh:");
        display.print(mAh, 0);
    }

    // Page number top-right
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(SCREEN_WIDTH - 20, 0);
    display.print("P.");
    display.print(ch + 2);
}

// ─── Main display loop ───────────────────────────────────────────

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

void init_display() {
    Wire.begin(I2C_SDA, I2C_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        Serial.println("OLED init failed");
        return;
    }
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 20);
    display.println("Power");
    display.setCursor(20, 40);
    display.println("Monitor");
    display.display();
    delay(2000);
    wire_started = true;
}

#else
void init_display() {}
void update_display(const SensorData&, const char*, float) {}
#endif