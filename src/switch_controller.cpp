#include "switch_controller.h"
#include "settings_manager.h"
#include "coulomb_counter.h"
#include "config.h"
#include "connectivity_manager.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const uint8_t default_pins[4] = { RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO, RELAY_4_GPIO };
static const uint8_t MAX_SWITCHES = 8;

struct SwitchState {
    bool energized;
    bool was_energized;  // track prior state for change detection
    unsigned long condition_start_ms;
    bool condition_active;
};
static SwitchState switch_states[MAX_SWITCHES];
static bool switch_auto_enabled = false;  // off by default — user enables via serial/BLE

static void set_switch_pin(const SwitchChannel& ch, bool energized) {
    bool pin_high = ch.active_high ? energized : !energized;
    digitalWrite(ch.gpio_pin, pin_high ? HIGH : LOW);
}

void init_switches() {
    switch_auto_enabled = false;
    uint8_t count = settings_load_switch_count();

    if (count == 0) {
        for (uint8_t i = 0; i < 4; i++) {
            SwitchChannel ch = {};
            ch.idx = i;
            ch.type = SW_RELAY;
            ch.gpio_pin = default_pins[i];
            ch.active_high = true;
            ch.enabled = true;
            ch.is_energized = false;
            snprintf(ch.name, sizeof(ch.name), "Relay %u", (unsigned)i);
            settings_save_switch(i, &ch);

            SwitchRule rule = {};
            rule.switch_idx = i;
            rule.channel = i;
            rule.overcurrent_A = 5.0f;
            rule.undervoltage_V = 0.0f;
            rule.soc_low_pct = 0.0f;
            rule.soc_high_pct = 0.0f;
            rule.trip_delay_ms = 1000;
            rule.reset_delay_ms = 5000;
            rule.enabled = true;
            settings_save_switch_rule(i, &rule);

            pinMode(ch.gpio_pin, OUTPUT);
            digitalWrite(ch.gpio_pin, LOW); // active_high=true => OFF
            switch_states[i] = { false, false, 0, false };
        }
    } else {
        for (uint8_t i = 0; i < count && i < MAX_SWITCHES; i++) {
            SwitchChannel ch;
            if (settings_load_switch(i, &ch)) {
                // Defensive: keep board defaults for the first four relays
                if (i < 4 && ch.type == SW_RELAY && ch.gpio_pin != default_pins[i]) {
                    Serial.printf("[SWITCH] gpio_pin mismatch idx=%u: expected %d, got %d — correcting\n",
                        i, default_pins[i], ch.gpio_pin);
                    ch.gpio_pin = default_pins[i];
                    settings_save_switch(i, &ch);
                }
                pinMode(ch.gpio_pin, OUTPUT);
                digitalWrite(ch.gpio_pin, ch.active_high ? LOW : HIGH); // OFF
                switch_states[i] = { false, false, 0, false };
            }
        }
    }

    // Publish initial switch states to the remote state table
    for (uint8_t i = 0; i < 4; i++) {
        publish_switch_state(i, switch_states[i].energized);
    }
}

void evaluate_switches(const SensorSnapshot& snapshot) {
    // Auto-trip disabled until user enables via 'switch auto on'
    if (!switch_auto_enabled) return;

    uint8_t count = settings_load_switch_count();
    unsigned long now = millis();

    for (uint8_t i = 0; i < count && i < MAX_SWITCHES; i++) {
        SwitchChannel ch;
        SwitchRule rule;
        if (!settings_load_switch(i, &ch) || !ch.enabled) continue;
        if (!settings_load_switch_rule(i, &rule) || !rule.enabled) continue;
        if (rule.channel >= snapshot.total_logical_channels) continue;

        uint8_t c = rule.channel;
        const PhysicalChannel* pc = sensor_get_logical_channel(snapshot, c);
        if (!pc) continue;
        float voltage = pc->voltage;
        float current = pc->current;

        float soc_pct = -1.0f;
        BatteryConfig bat;
        if (settings_load_battery(c, &bat) && bat.capacity_mAh > 0.001f) {
            float net_mAh = get_coulomb_mAh(c);
            soc_pct = bat.initial_soc_pct + (net_mAh / bat.capacity_mAh) * 100.0f;
            if (soc_pct < 0.0f) soc_pct = 0.0f;
            if (soc_pct > 100.0f) soc_pct = 100.0f;
        }

        bool condition_met = false;
        if (rule.overcurrent_A > 0.001f && current > rule.overcurrent_A) condition_met = true;
        if (rule.undervoltage_V > 0.001f && voltage < rule.undervoltage_V) condition_met = true;
        if (soc_pct >= 0.0f && rule.soc_low_pct > 0.001f && soc_pct < rule.soc_low_pct) condition_met = true;
        if (soc_pct >= 0.0f && rule.soc_high_pct > 0.001f && soc_pct > rule.soc_high_pct) condition_met = true;

        SwitchState& st = switch_states[i];

        if (condition_met) {
            if (!st.condition_active) {
                st.condition_active = true;
                st.condition_start_ms = now;
            } else if (!st.energized && (now - st.condition_start_ms >= rule.trip_delay_ms)) {
                st.energized = true;
                if (!st.was_energized) {
                    st.was_energized = true;
                    publish_switch_state(i, true);
                }
                set_switch_pin(ch, true);
                Serial.printf("Switch %d TRIPPED (ch=%d, pin=%d, type=%d)\n", i, c, ch.gpio_pin, (int)ch.type);
            }
        } else {
            if (st.condition_active) {
                st.condition_active = false;
                st.condition_start_ms = now;
            } else if (st.energized && (now - st.condition_start_ms >= rule.reset_delay_ms)) {
                st.energized = false;
                if (st.was_energized) {
                    st.was_energized = false;
                    publish_switch_state(i, false);
                }
                set_switch_pin(ch, false);
                Serial.printf("Switch %d RESET (ch=%d, pin=%d, type=%d)\n", i, c, ch.gpio_pin, (int)ch.type);
            }
        }
    }
}

void switch_set_auto(bool enabled) {
    switch_auto_enabled = enabled;
}

void switch_set(uint8_t idx, bool is_energized) {
    if (idx >= MAX_SWITCHES) return;
    SwitchChannel ch;
    if (!settings_load_switch(idx, &ch)) return;
    set_switch_pin(ch, is_energized);
    switch_states[idx].energized = is_energized;
    switch_states[idx].was_energized = is_energized;
    ch.is_energized = is_energized;
    settings_save_switch(idx, &ch);
    publish_switch_state(idx, is_energized);
}

void switch_pulse(uint8_t idx, uint32_t duration_ms) {
    SwitchChannel ch;
    if (!settings_load_switch(idx, &ch)) return;
    Serial.printf("Pulsing switch %d (GPIO %d) for %u ms...\n", idx, ch.gpio_pin, (unsigned)duration_ms);
    switch_set(idx, true);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    switch_set(idx, false);
    Serial.println("Switch pulse done.");
}

bool get_switch_state(uint8_t idx) {
    if (idx >= MAX_SWITCHES) return false;
    SwitchChannel ch;
    if (!settings_load_switch(idx, &ch) || !ch.enabled) return false;
    bool pin_high = digitalRead(ch.gpio_pin) == HIGH;
    return ch.active_high ? pin_high : !pin_high;
}
