#include "relay_controller.h"
#include "settings_manager.h"
#include "coulomb_counter.h"
#include "config.h"
#include "connectivity_manager.h"
#include <Arduino.h>

static const uint8_t default_pins[4] = { RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO, RELAY_4_GPIO };

static bool is_safe_relay_pin(int pin) {
    if (pin < 0 || pin >= GPIO_NUM_MAX) return false;
    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)pin)) return false;
    // Reject common strapping, UART, and SPI/flash pins across ESP32 variants.
    switch (pin) {
        case 0:   // strapping / BOOT
        case 1:   // UART0 TX
        case 3:   // UART0 RX
        case 6:   // SPI flash CLK (classic ESP32)
        case 7:   // SPI flash SD0
        case 8:   // SPI flash SD1
        case 9:   // SPI flash SD2
        case 10:  // SPI flash SD3
        case 11:  // SPI flash CMD
        case 12:  // strapping / flash
        case 15:  // strapping / flash
        case 16:  // flash (ESP32-C3)
        case 17:  // flash (ESP32-C3)
        case 20:  // flash CS / UART0 TX (ESP32-C3)
        case 21:  // flash CLK / UART0 RX (ESP32-C3)
            return false;
        default:
            return true;
    }
}

struct RelayState {
    bool energized;
    bool was_energized;  // track prior state for change detection
    unsigned long condition_start_ms;
    bool condition_active;
};
static RelayState relay_states[4];
static unsigned long relay_boot_time = 0;
static bool relay_auto_enabled = false;  // off by default — user enables via serial

void init_relays() {
    uint8_t count = settings_load_relay_count();
    relay_boot_time = millis();
    relay_auto_enabled = false;  // OFF by default — must enable manually
    if (count == 0) {
        for (uint8_t ch = 0; ch < 4; ch++) {
            RelayRule rt = { ch, 5.0f, 0.0f, 0.0f, 0.0f, 1000, 5000, default_pins[ch], true, true };
            settings_save_relay(ch, &rt);
            if (!is_safe_relay_pin(default_pins[ch])) {
                Serial.printf("[RELAY] ERROR: default pin %d for relay %d is invalid/unsafe — skipping\n",
                    default_pins[ch], ch);
                relay_states[ch] = { false, false, 0, false };
                continue;
            }
            pinMode(default_pins[ch], OUTPUT);
            digitalWrite(default_pins[ch], LOW); // active_high → LOW = relay OFF at boot
            relay_states[ch] = { false, false, 0, false };
        }
    } else {
        for (uint8_t i = 0; i < count; i++) {
            RelayRule rt;
            if (settings_load_relay(i, &rt)) {
                // Defensive: if gpio_pin doesn't match board default, correct it
                if (i < 4 && rt.gpio_pin != default_pins[i]) {
                    Serial.printf("[RELAY] gpio_pin mismatch idx=%d: expected %d, got %d — correcting\n",
                        i, default_pins[i], rt.gpio_pin);
                    rt.gpio_pin = default_pins[i];
                    settings_save_relay(i, &rt);
                }
                if (!is_safe_relay_pin(rt.gpio_pin)) {
                    Serial.printf("[RELAY] ERROR: loaded pin %d for relay %d is invalid/unsafe — skipping\n",
                        rt.gpio_pin, i);
                    relay_states[i] = { false, false, 0, false };
                    continue;
                }
                pinMode(rt.gpio_pin, OUTPUT);
                digitalWrite(rt.gpio_pin, LOW); // active_high=true: LOW=OFF, HIGH=ON
                relay_states[i] = { false, false, 0, false };
            }
        }
    }
    // Publish initial relay states to relay_states table
    for (uint8_t i = 0; i < 4; i++) {
        publish_relay_state(i, relay_states[i].energized);
    }
}

void evaluate_relays(const SensorSnapshot& data) {
    (void)data;
    // Auto-trip disabled until user enables via 'relay auto on'
    if (!relay_auto_enabled) return;
    float voltages[4] = {
        get_channel_voltage(0), get_channel_voltage(1),
        get_channel_voltage(2), get_channel_voltage(3)
    };
    float currents[4] = {
        get_channel_current(0), get_channel_current(1),
        get_channel_current(2), get_channel_current(3)
    };

    uint8_t count = settings_load_relay_count();
    unsigned long now = millis();

    for (uint8_t i = 0; i < count; i++) {
        RelayRule rt;
        if (!settings_load_relay(i, &rt) || !rt.enabled) continue;
        if (rt.channel > 3) continue;

        uint8_t ch = rt.channel;

        // Compute SoC if battery config exists
        float soc_pct = -1.0f; // -1 = no battery configured
        BatteryConfig bat;
        if (settings_load_battery(ch, &bat) && bat.capacity_mAh > 0.001f) {
            float net_mAh = get_coulomb_mAh(ch);
            soc_pct = bat.initial_soc_pct + (net_mAh / bat.capacity_mAh) * 100.0f;
            if (soc_pct < 0) soc_pct = 0;
            if (soc_pct > 100) soc_pct = 100;
        }

        bool condition_met = false;
        if (rt.overcurrent_A > 0.001f && currents[ch] > rt.overcurrent_A) condition_met = true;
        if (rt.undervoltage_V > 0.001f && voltages[ch] < rt.undervoltage_V) condition_met = true;
        if (soc_pct >= 0 && rt.soc_low_pct > 0.001f && soc_pct < rt.soc_low_pct) condition_met = true;
        if (soc_pct >= 0 && rt.soc_high_pct > 0.001f && soc_pct > rt.soc_high_pct) condition_met = true;

        RelayState& st = relay_states[i];

        if (condition_met) {
            if (!st.condition_active) {
                st.condition_active = true;
                st.condition_start_ms = now;
            } else if (!st.energized && (now - st.condition_start_ms >= rt.trip_delay_ms)) {
                st.energized = true;
                if (!st.was_energized) { st.was_energized = true; publish_relay_state(i, true); }
                if (is_safe_relay_pin(rt.gpio_pin)) {
                    digitalWrite(rt.gpio_pin, rt.active_high ? HIGH : LOW);
                } else {
                    Serial.printf("[RELAY] ERROR: trip skipped, invalid pin %d (relay %d)\n", rt.gpio_pin, i);
                }
                Serial.printf("Relay %d TRIPPED (ch=%d, pin=%d)\n", i, ch, rt.gpio_pin);
            }
        } else {
            if (st.condition_active) {
                st.condition_active = false;
                st.condition_start_ms = now;
            } else if (st.energized && (now - st.condition_start_ms >= rt.reset_delay_ms)) {
                st.energized = false;
                if (st.was_energized) { st.was_energized = false; publish_relay_state(i, false); }
                if (is_safe_relay_pin(rt.gpio_pin)) {
                    digitalWrite(rt.gpio_pin, rt.active_high ? LOW : HIGH);
                } else {
                    Serial.printf("[RELAY] ERROR: reset skipped, invalid pin %d (relay %d)\n", rt.gpio_pin, i);
                }
                Serial.printf("Relay %d RESET (ch=%d, pin=%d)\n", i, ch, rt.gpio_pin);
            }
        }
    }
}

void relay_set_auto(bool enabled) {
    relay_auto_enabled = enabled;
}

void relay_set(uint8_t idx, bool is_energized) {
    RelayRule rt;
    if (!settings_load_relay(idx, &rt)) return;
    if (!is_safe_relay_pin(rt.gpio_pin)) {
        Serial.printf("[RELAY] ERROR: manual set skipped, invalid pin %d (relay %d)\n", rt.gpio_pin, idx);
        return;
    }
    digitalWrite(rt.gpio_pin, rt.active_high ? (is_energized ? HIGH : LOW) : (is_energized ? LOW : HIGH));
    relay_states[idx].energized = is_energized;
    relay_states[idx].was_energized = is_energized;
    rt.is_energized = is_energized;
    settings_save_relay(idx, &rt);
    publish_relay_state(idx, is_energized);
}

bool get_relay_state(uint8_t idx) {
    RelayRule rt;
    if (!settings_load_relay(idx, &rt) || !rt.enabled) return false;
    if (!is_safe_relay_pin(rt.gpio_pin)) return false;
    bool pin_high = digitalRead(rt.gpio_pin) == HIGH;
    return rt.active_high ? pin_high : !pin_high;
}
