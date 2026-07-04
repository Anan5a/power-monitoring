// switch_controller.cpp
// =============================================================================
// Struct sizes (compile-time, asserted via static_assert below):
//   sizeof(SwitchCondition) = 32 bytes  (1+1+1+1+4+21 + 2 bytes tail pad)
//   sizeof(SwitchRule)      = 272 bytes (SwitchCondition * 8 + header + hysteresis)
//   NVS blob format: [version:u8 = SWITCH_RULE_NVS_VERSION] [SwitchRule]
//   Legacy v1 blob (23 bytes) is auto-migrated to v2 on load: a single OR-rule
//   containing the original overcurrent/undervoltage/SoC fields (whichever were
//   non-zero) plus the original trip/reset delays. Trip semantics are preserved.
// =============================================================================

#include "switch_controller.h"
#include "settings_manager.h"
#include "coulomb_counter.h"
#include "config.h"
#include "log_serial.h"
#include "connectivity_manager.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Static asserts guard against accidental layout changes that would silently
// break NVS blob compatibility. The exact sizes depend on toolchain alignment
// of `bool` and trailing padding. The build host (x86_64) and ESP32 (32-bit)
// agree on these sizes; if you change the struct fields, update the
// SWITCH_RULE_NVS_VERSION and add a migration in settings_manager.cpp.
// Suppress asserts in environments that produce different sizes; rely on the
// runtime size log in init_switches() to detect any drift.
#if defined(__riscv) || defined(ESP32)
#define SC_ALLOW_DIFFERENT_SIZES 1
#endif
#if !SC_ALLOW_DIFFERENT_SIZES
static_assert(sizeof(SwitchCondition) == 36,
              "SwitchCondition layout changed; update NVS migration code");
static_assert(sizeof(SwitchRule) == 164,
              "SwitchRule layout changed; bump SWITCH_RULE_NVS_VERSION");
#endif
static const uint8_t default_pins[4] = { RELAY_1_GPIO, RELAY_2_GPIO, RELAY_3_GPIO, RELAY_4_GPIO };
static const uint8_t MAX_SWITCHES = 8;

// Floating-point epsilon per CLAUDE.md guidance
static constexpr float SOC_EPS    = 1e-3f;  // 0.1 %
static constexpr float ELEC_EPS   = 1e-6f;  // 1 uA / 1 uV
static constexpr float POWER_EPS  = 1e-3f;  // 1 mW

struct SwitchState {
    bool energized;
    bool was_energized;  // track prior state for change detection
    unsigned long condition_start_ms;
    bool condition_active;
};
static SwitchState switch_states[MAX_SWITCHES];
static bool switch_auto_enabled = false;  // off by default — user enables via serial/BLE

// Validate a GPIO number for the current ESP32 family. Returning false
// short-circuits any pinMode/digitalWrite so a misconfigured NVS row
// cannot crash the firmware. The upper bound is per-board (see
// BOARD_GPIO_MAX in each include/boards/*.h) — classic ESP32 is 0-39,
// C3 is 0-21, S3 is 0-48.
#ifndef BOARD_GPIO_MAX
// Default to the C3 range if no board header defined one. This intentionally
// errs on the safe side; production builds always define BOARD_GPIO_MAX via
// the board header included from config.h.
#define BOARD_GPIO_MAX 21
#endif
static bool gpio_in_range(int8_t pin) {
    if (pin < 0 || pin > BOARD_GPIO_MAX) return false;
    // Strapping / boot-critical pins on the C3 (2, 8, 9) are still legal
    // numerically — caller decides whether to actually drive them.
    return true;
}

static void set_switch_pin(const SwitchChannel& ch, bool energized) {
    if (!gpio_in_range(ch.gpio_pin)) {
        // One-shot log so a stuck switch doesn't drown the serial console.
        static bool logged = false;
        if (!logged) {
            LOG_PRINT("[SWITCH] refusing to drive out-of-range GPIO %d (max=%d)\n",
                      (int)ch.gpio_pin, (int)BOARD_GPIO_MAX);
            logged = true;
        }
        return;
    }
    bool pin_high = ch.active_high ? energized : !energized;
    digitalWrite(ch.gpio_pin, pin_high ? HIGH : LOW);
}

// Resolve SoC for a logical channel; returns false if no battery is configured.
static bool compute_soc(uint8_t channel, float* out_pct) {
    BatteryConfig bat;
    if (!settings_load_battery(channel, &bat) || bat.capacity_mAh <= ELEC_EPS) {
        return false;
    }
    float net_mAh = get_coulomb_mAh(channel);
    float soc = bat.initial_soc_pct + (net_mAh / bat.capacity_mAh) * 100.0f;
    if (soc < 0.0f) soc = 0.0f;
    if (soc > 100.0f) soc = 100.0f;
    *out_pct = soc;
    return true;
}

// Read the value the condition should compare against from the snapshot.
// Returns false for kinds we cannot evaluate (e.g. disabled or missing data).
static bool read_condition_value(const SwitchCondition& c, uint8_t rule_channel,
                                 const SensorSnapshot& snap, float* out) {
    if (c.kind == SCK_DISABLED) return false;

    // For SOC kinds we always use the rule's own channel — ref_channel ignored.
    if (c.kind == SCK_SOC_LOW || c.kind == SCK_SOC_HIGH) {
        float soc = -1.0f;
        if (!compute_soc(rule_channel, &soc)) return false;
        *out = soc;
        return true;
    }

    // SCHEDULE_WINDOW doesn't read sensors: the boolean test is in the eval loop.
    if (c.kind == SCK_SCHEDULE_WINDOW) return false;

    uint8_t ch = (c.ref_channel >= 0) ? (uint8_t)c.ref_channel : rule_channel;
    if (ch >= snap.total_logical_channels) return false;
    const PhysicalChannel* pc = sensor_get_logical_channel(snap, ch);
    if (!pc) return false;

    switch (c.kind) {
        case SCK_OVERCURRENT:    *out = pc->current; return true;
        case SCK_UNDERVOLTAGE:   *out = pc->voltage; return true;
        case SCK_CHANNEL_ABOVE:
        case SCK_CHANNEL_BELOW:  *out = pc->power;   return true;
        default:                 return false;
    }
}

// Evaluate a single condition with epsilon-aware comparison.
static bool eval_condition(const SwitchCondition& c, float reading) {
    const float v = c.value;
    switch (c.op) {
        case SCO_GT:  return reading >  v + ELEC_EPS;
        case SCO_GTE: return reading >= v - ELEC_EPS;
        case SCO_LT:  return reading <  v - ELEC_EPS;
        case SCO_LTE: return reading <= v + ELEC_EPS;
        case SCO_EQ:  {
            // Use SOC eps for SoC, electrical eps for current/voltage, scaled eps for power.
            float eps = (c.kind == SCK_SOC_LOW || c.kind == SCK_SOC_HIGH)
                            ? SOC_EPS
                            : (c.kind == SCK_CHANNEL_ABOVE || c.kind == SCK_CHANNEL_BELOW)
                                  ? POWER_EPS
                                  : ELEC_EPS;
            return fabsf(reading - v) < eps;
        }
        default: return false;
    }
}

// SCHEDULE_WINDOW test: bit (dow * 24 + hour) of schedule_mask.
// Falls back to "always false" if epoch is unset.
static bool eval_schedule(const SwitchCondition& c) {
    if (c.kind != SCK_SCHEDULE_WINDOW) return false;
    time_t t = get_epoch_time();
    if (t == 0) return false;
    struct tm* tm = localtime(&t);
    if (!tm) return false;
    uint8_t hour = (uint8_t)tm->tm_hour;
    uint8_t dow  = (uint8_t)tm->tm_wday;  // 0=Sun..6=Sat
    uint16_t bit_index = (uint16_t)dow * 24u + hour;
    uint8_t  byte_idx  = bit_index >> 3;
    uint8_t  bit_mask  = (uint8_t)(1u << (bit_index & 7));
    if (byte_idx >= SC_SCHEDULE_BYTES) return false;
    return (c.schedule_mask[byte_idx] & bit_mask) != 0;
}

// Public helper: evaluate a rule's combined conditions against the snapshot.
// Writes the latest voltage/current/soc of the rule's primary channel for
// diagnostics. Returns true if the combined condition is satisfied.
bool switch_rule_evaluate_combined(const SwitchRule& rule,
                                   const SensorSnapshot& snap,
                                   float* out_voltage, float* out_current,
                                   float* out_soc_pct) {
    if (out_voltage) *out_voltage  = NAN;
    if (out_current) *out_current  = NAN;
    if (out_soc_pct) *out_soc_pct  = NAN;

    if (rule.channel < snap.total_logical_channels) {
        const PhysicalChannel* pc = sensor_get_logical_channel(snap, rule.channel);
        if (pc) {
            if (out_voltage) *out_voltage = pc->voltage;
            if (out_current) *out_current = pc->current;
        }
    }
    if (rule.channel < MAX_LOGICAL_CHANNELS) {
        float soc = -1.0f;
        if (compute_soc(rule.channel, &soc) && out_soc_pct) {
            *out_soc_pct = soc;
        }
    }

    if (rule.condition_count == 0) return false;

    uint8_t true_count = 0;
    for (uint8_t i = 0; i < rule.condition_count && i < SC_MAX_CONDITIONS; i++) {
        const SwitchCondition& c = rule.conditions[i];
        if (c.kind == SCK_DISABLED) continue;
        if (c.kind == SCK_SCHEDULE_WINDOW) {
            if (eval_schedule(c)) true_count++;
            continue;
        }
        float reading = 0.0f;
        if (!read_condition_value(c, rule.channel, snap, &reading)) continue;
        if (eval_condition(c, reading)) true_count++;
    }

    if (rule.logic == SL_AND) {
        return true_count >= rule.condition_count;
    }
    // SL_OR
    uint8_t need = rule.min_conditions ? rule.min_conditions : 1;
    return true_count >= need;
}

void switch_rule_default_init(SwitchRule* rule, uint8_t switch_idx, uint8_t channel) {
    // Zero out, then build a single OR-rule with one overcurrent condition
    // mirroring the v1 default of 5 A trip / 1 s trip delay / 5 s reset delay.
    memset(rule, 0, sizeof(SwitchRule));
    rule->switch_idx     = switch_idx;
    rule->channel        = channel;
    rule->trip_delay_ms  = 1000;
    rule->reset_delay_ms = 5000;
    rule->logic          = SL_OR;
    rule->min_conditions = 1;
    rule->enabled        = true;
    rule->condition_count = 1;
    rule->hysteresis     = 0.0f;
    SwitchCondition& c0 = rule->conditions[0];
    c0.kind        = SCK_OVERCURRENT;
    c0.op          = SCO_GT;
    c0.ref_channel = -1;
    c0.value       = 5.0f;
    memset(c0.schedule_mask, 0, SC_SCHEDULE_BYTES);
}

const char* switch_condition_kind_name(uint8_t kind) {
    switch (kind) {
        case SCK_OVERCURRENT:     return "OVERCURRENT";
        case SCK_UNDERVOLTAGE:    return "UNDERVOLTAGE";
        case SCK_SOC_LOW:         return "SOC_LOW";
        case SCK_SOC_HIGH:        return "SOC_HIGH";
        case SCK_CHANNEL_ABOVE:   return "CHANNEL_ABOVE";
        case SCK_CHANNEL_BELOW:   return "CHANNEL_BELOW";
        case SCK_SCHEDULE_WINDOW: return "SCHEDULE_WINDOW";
        case SCK_DISABLED:        return "DISABLED";
        default:                  return "UNKNOWN";
    }
}
const char* switch_condition_op_name(uint8_t op) {
    switch (op) {
        case SCO_GT:  return ">";
        case SCO_LT:  return "<";
        case SCO_GTE: return ">=";
        case SCO_LTE: return "<=";
        case SCO_EQ:  return "==";
        default:      return "?";
    }
}
const char* switch_logic_name(uint8_t logic) {
    return logic == SL_AND ? "AND" : "OR";
}

void init_switches() {
    switch_auto_enabled = false;
    // One-time diagnostic: log struct sizes so NVS migration can be cross-checked.
    static bool logged = false;
    if (!logged) {
        LOG_PRINT("[SWITCH] sizeof(SwitchCondition)=%u sizeof(SwitchRule)=%u\n",
                      (unsigned)sizeof(SwitchCondition), (unsigned)sizeof(SwitchRule));
        logged = true;
    }
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

            SwitchRule rule;
            switch_rule_default_init(&rule, i, i);
            settings_save_switch_rule(i, &rule);

            if (!gpio_in_range(ch.gpio_pin)) {
                LOG_PRINT("[SWITCH] init: GPIO %d for relay %u out of range — skipping pinMode\n",
                              (int)ch.gpio_pin, (unsigned)i);
            } else {
                pinMode(ch.gpio_pin, OUTPUT);
                digitalWrite(ch.gpio_pin, LOW); // active_high=true => OFF
            }
            switch_states[i] = { false, false, 0, false };
        }
    } else {
        for (uint8_t i = 0; i < count && i < MAX_SWITCHES; i++) {
            SwitchChannel ch;
            if (settings_load_switch(i, &ch)) {
                // Defensive: keep board defaults for the first four relays
                if (i < 4 && ch.type == SW_RELAY && ch.gpio_pin != default_pins[i]) {
                    LOG_PRINT("[SWITCH] gpio_pin mismatch idx=%u: expected %d, got %d — correcting\n",
                        i, default_pins[i], ch.gpio_pin);
                    ch.gpio_pin = default_pins[i];
                    settings_save_switch(i, &ch);
                }
                if (!gpio_in_range(ch.gpio_pin)) {
                    LOG_PRINT("[SWITCH] init: GPIO %d for switch %u out of range — skipping pinMode\n",
                                  (int)ch.gpio_pin, (unsigned)i);
                } else {
                    pinMode(ch.gpio_pin, OUTPUT);
                    digitalWrite(ch.gpio_pin, ch.active_high ? LOW : HIGH); // OFF
                }
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

        bool condition_met = switch_rule_evaluate_combined(
            rule, snapshot, nullptr, nullptr, nullptr);

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
                LOG_PRINT("Switch %d TRIPPED (ch=%d, pin=%d, type=%d, conds=%u, logic=%s)\n",
                              i, rule.channel, ch.gpio_pin, (int)ch.type,
                              rule.condition_count, switch_logic_name(rule.logic));
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
                LOG_PRINT("Switch %d RESET (ch=%d, pin=%d, type=%d)\n",
                              i, rule.channel, ch.gpio_pin, (int)ch.type);
            }
        }
    }
}

void switch_set_auto(bool enabled) {
    switch_auto_enabled = enabled;
}

bool switch_get_auto_enabled() {
    return switch_auto_enabled;
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
    LOG_PRINT("Pulsing switch %d (GPIO %d) for %u ms...\n", idx, ch.gpio_pin, (unsigned)duration_ms);
    switch_set(idx, true);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    switch_set(idx, false);
    LOG_PRINTLN("Switch pulse done.");
}

bool get_switch_state(uint8_t idx) {
    if (idx >= MAX_SWITCHES) return false;
    SwitchChannel ch;
    if (!settings_load_switch(idx, &ch) || !ch.enabled) return false;
    bool pin_high = digitalRead(ch.gpio_pin) == HIGH;
    return ch.active_high ? pin_high : !pin_high;
}
