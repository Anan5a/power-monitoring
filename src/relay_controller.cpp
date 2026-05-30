#include "relay_controller.h"
#include "settings_manager.h"
#include "coulomb_counter.h"
#include "config.h"
#include "connectivity_manager.h"
#include <Arduino.h>

static const uint8_t default_pins[4] = { RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO, RELAY_4_GPIO };

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
            pinMode(default_pins[ch], OUTPUT);
            digitalWrite(default_pins[ch], LOW); // active_high → LOW = relay OFF at boot
            relay_states[ch] = { false, false, 0, false };
        }
    } else {
        for (uint8_t i = 0; i < count; i++) {
            RelayRule rt;
            if (settings_load_relay(i, &rt)) {
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

void evaluate_relays(const SensorData& data) {
    // Auto-trip disabled until user enables via 'relay auto on'
    if (!relay_auto_enabled) return;
    float voltages[4] = {
        data.ads1115_volts[0], data.ads1115_volts[1], data.ads1115_volts[2], data.ina226_busV
    };
    float currents[4] = {
        data.ina3221_current[0], data.ina3221_current[1], data.ina3221_current[2], data.ina226_current
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
                digitalWrite(rt.gpio_pin, rt.active_high ? HIGH : LOW);
                Serial.printf("Relay %d TRIPPED (ch=%d, pin=%d)\n", i, ch, rt.gpio_pin);
            }
        } else {
            if (st.condition_active) {
                st.condition_active = false;
                st.condition_start_ms = now;
            } else if (st.energized && (now - st.condition_start_ms >= rt.reset_delay_ms)) {
                st.energized = false;
                if (st.was_energized) { st.was_energized = false; publish_relay_state(i, false); }
                digitalWrite(rt.gpio_pin, rt.active_high ? LOW : HIGH);
                Serial.printf("Relay %d RESET (ch=%d, pin=%d)\n", i, ch, rt.gpio_pin);
            }
        }
    }
}

void relay_set_auto(bool enabled) {
    relay_auto_enabled = enabled;
}

bool get_relay_state(uint8_t idx) {
    RelayRule rt;
    if (!settings_load_relay(idx, &rt) || !rt.enabled) return false;
    bool pin_high = digitalRead(rt.gpio_pin) == HIGH;
    return rt.active_high ? pin_high : !pin_high;
}
