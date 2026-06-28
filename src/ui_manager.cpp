#include "ui_manager.h"
#include "config.h"
#include "switch_controller.h"
#include "sensor_manager.h"
#include "settings_manager.h"
#include "display_manager.h"
#include "connectivity_manager.h"
#include <Arduino.h>

#if UI_BUTTON_COUNT > 0

struct ButtonState {
    uint8_t pin;
    bool last_raw;
    bool stable;
    unsigned long last_change_ms;
    unsigned long press_start_ms;
    unsigned long last_release_ms;
    bool pending_short;
    bool pending_long;
    bool pending_double;
};

static ButtonState buttons[UI_BUTTON_COUNT];
static int8_t next_display_page = -1;

struct LedState {
    uint8_t pin;
    LedPattern pattern;
    bool current;
    unsigned long last_toggle_ms;
};

static LedState leds[3]; // network, error, ok

static bool network_wifi = false;
static bool network_cloud = false;
static bool system_fault = false;
static bool system_healthy = false;

static const int ui_button_pins[UI_BUTTON_COUNT] = UI_BUTTON_PINS;

void init_ui() {
    for (uint8_t i = 0; i < UI_BUTTON_COUNT; i++) {
        buttons[i].pin = (ui_button_pins[i] >= 0) ? (uint8_t)ui_button_pins[i] : 255;
        if (buttons[i].pin != 255) {
            pinMode(buttons[i].pin, INPUT_PULLUP);
            buttons[i].last_raw = digitalRead(buttons[i].pin) == LOW; // active low
            buttons[i].stable = buttons[i].last_raw;
        }
    }

    leds[0].pin = (UI_LED_NETWORK_GPIO >= 0) ? (uint8_t)UI_LED_NETWORK_GPIO : 255;
    leds[1].pin = (UI_LED_ERROR_GPIO >= 0) ? (uint8_t)UI_LED_ERROR_GPIO : 255;
    leds[2].pin = (UI_LED_OK_GPIO >= 0) ? (uint8_t)UI_LED_OK_GPIO : 255;

    for (uint8_t i = 0; i < 3; i++) {
        if (leds[i].pin != 255) {
            pinMode(leds[i].pin, OUTPUT);
            digitalWrite(leds[i].pin, LOW);
        }
    }
}

static bool is_pressed(const ButtonState& b) {
    return b.stable; // active low after inversion in read
}

void ui_set_led(uint8_t led, LedPattern pattern) {
    if (led >= 3) return;
    leds[led].pattern = pattern;
}

void ui_set_network_status(bool wifi_connected, bool cloud_connected) {
    network_wifi = wifi_connected;
    network_cloud = cloud_connected;
    if (!network_wifi) ui_set_led(0, LED_SLOW_BLINK);      // off: slow blink
    else if (!network_cloud) ui_set_led(0, LED_FAST_BLINK); // wifi but no cloud
    else ui_set_led(0, LED_ON);                              // connected
}

void ui_set_fault(bool fault) {
    system_fault = fault;
    ui_set_led(1, fault ? LED_ON : LED_OFF);
}

void ui_set_heartbeat(bool healthy) {
    system_healthy = healthy;
    ui_set_led(2, healthy ? LED_PULSE : LED_OFF);
}

static void update_leds(unsigned long now) {
    for (uint8_t i = 0; i < 3; i++) {
        if (leds[i].pin == 255) continue;
        bool on = false;
        switch (leds[i].pattern) {
            case LED_OFF: on = false; break;
            case LED_ON:  on = true;  break;
            case LED_SLOW_BLINK:
                if (now - leds[i].last_toggle_ms >= 500) {
                    leds[i].last_toggle_ms = now;
                    leds[i].current = !leds[i].current;
                }
                on = leds[i].current;
                break;
            case LED_FAST_BLINK:
                if (now - leds[i].last_toggle_ms >= 125) {
                    leds[i].last_toggle_ms = now;
                    leds[i].current = !leds[i].current;
                }
                on = leds[i].current;
                break;
            case LED_PULSE:
                on = ((now / 250) % 2) == 0;
                break;
        }
        digitalWrite(leds[i].pin, on ? HIGH : LOW);
    }
}

static void handle_button_event(uint8_t idx, bool short_press, bool long_press, bool double_press) {
    (void)double_press;
    if (short_press) {
        switch (idx) {
            case 0: next_display_page = 1; break; // cycle page on next display update
            case 1: switch_set(0, !get_switch_state(0)); break;
            case 2: sensor_calibrate_baseline(); break;
            default: break;
        }
    }
    if (long_press && idx == 3) {
        Serial.println("[UI] factory reset requested via button");
        settings_factory_reset();
        ESP.restart();
    }
}

void loop_ui() {
    unsigned long now = millis();

    for (uint8_t i = 0; i < UI_BUTTON_COUNT; i++) {
        if (buttons[i].pin == 255) continue;
        bool raw = digitalRead(buttons[i].pin) == LOW; // active low
        if (raw != buttons[i].last_raw) {
            buttons[i].last_change_ms = now;
            buttons[i].last_raw = raw;
        }
        if ((now - buttons[i].last_change_ms) >= 20) {
            if (raw != buttons[i].stable) {
                buttons[i].stable = raw;
                if (raw) {
                    // pressed
                    buttons[i].press_start_ms = now;
                } else {
                    // released
                    unsigned long dur = now - buttons[i].press_start_ms;
                    if (dur >= 5000) {
                        buttons[i].pending_long = true;
                    } else if (dur >= 30) {
                        if ((now - buttons[i].last_release_ms) < 300) {
                            buttons[i].pending_double = true;
                        } else {
                            buttons[i].pending_short = true;
                        }
                    }
                    buttons[i].last_release_ms = now;
                }
            }
        }

        // Long press detection while still held
        if (buttons[i].stable && (now - buttons[i].press_start_ms) >= 5000 && !buttons[i].pending_long) {
            buttons[i].pending_long = true;
        }
    }

    for (uint8_t i = 0; i < UI_BUTTON_COUNT; i++) {
        bool s = buttons[i].pending_short; buttons[i].pending_short = false;
        bool l = buttons[i].pending_long;  buttons[i].pending_long = false;
        bool d = buttons[i].pending_double; buttons[i].pending_double = false;
        if (s || l || d) handle_button_event(i, s, l, d);
    }

    update_leds(now);
}

bool ui_next_display_page() {
    if (next_display_page > 0) {
        next_display_page = 0;
        return true;
    }
    return false;
}

#else // UI_BUTTON_COUNT == 0

void init_ui() {}
void loop_ui() {}
void ui_set_led(uint8_t, LedPattern) {}
void ui_set_network_status(bool, bool) {}
void ui_set_fault(bool) {}
void ui_set_heartbeat(bool) {}

#endif
