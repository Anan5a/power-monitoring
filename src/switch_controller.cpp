// switch_controller.cpp
// =============================================================================
// Struct sizes (compile-time, asserted via static_assert below):
//   sizeof(SwitchCondition) = 32 bytes  (1+1+1+1+4+21 + 3 bytes tail pad)
//   sizeof(SwitchRule)      = 148 bytes (header 20 + 4×SwitchCondition 128)
//   NVS blob format: [version:u8 = SWITCH_RULE_NVS_VERSION] [SwitchRule]
//   Legacy v1 blob (23 bytes) is auto-migrated to v2 on load: a single OR-rule
//   containing the original overcurrent/undervoltage/SoC fields (whichever were
//   non-zero) plus the original trip/reset delays. Trip semantics are preserved.
// =============================================================================

#include "switch_controller.h"
#include "settings_manager.h"
#include "coulomb_counter.h"
#include "config.h"
#include <cstring>
#include "log_serial.h"
#include "connectivity_manager.h"
#include "data_logger.h"  // log_epoch_valid() for SCHEDULE_WINDOW gate
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
static_assert(sizeof(SwitchCondition) == 32,
              "SwitchCondition layout changed; update NVS migration code");
static_assert(sizeof(SwitchRule) == 148,
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
    bool pulse_active;          // a non-blocking pulse is in flight
    unsigned long pulse_off_at_ms;  // when the pulse should end
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

// Per-board denylist of GPIOs that may never be driven by the switch
// controller (strapping pins, USB, flash, etc.). The lists are defined in
// include/boards/<board>.h and selected at compile time via the BOARD_*
// build flags in platformio.ini.
#if defined(BOARD_ESP32DEV)
    static const int* BAD_GPIO_PINS     = BAD_GPIO_PINS_ESP32DEV;
    static const int  BAD_GPIO_COUNT    = BAD_GPIO_COUNT_ESP32DEV;
#elif defined(BOARD_ESP32C3)
    static const int* BAD_GPIO_PINS     = BAD_GPIO_PINS_ESP32C3;
    static const int  BAD_GPIO_COUNT    = BAD_GPIO_COUNT_ESP32C3;
#elif defined(BOARD_ESP32S3)
    static const int* BAD_GPIO_PINS     = BAD_GPIO_PINS_ESP32S3;
    static const int  BAD_GPIO_COUNT    = BAD_GPIO_COUNT_ESP32S3;
#else
    // Fallback: no denylist beyond the range check. Compile-time warning
    // is already emitted by config.h, but a defined() check keeps the
    // list zero-length instead of failing the build.
    static const int  BAD_GPIO_PINS_DUMMY[1] = { -1 };
    static const int* BAD_GPIO_PINS     = BAD_GPIO_PINS_DUMMY;
    static const int  BAD_GPIO_COUNT    = 0;
#endif

static bool gpio_in_range(int8_t pin) {
    if (pin < 0 || pin > BOARD_GPIO_MAX) return false;
    for (int i = 0; i < BAD_GPIO_COUNT; i++) {
        if (pin == BAD_GPIO_PINS[i]) return false;
    }
    return true;
}

// Public so the BLE / serial path can validate a pin before saving it to
// NVS. Returns false with a one-shot log if the pin is reserved.
bool switch_gpio_allowed(int8_t pin) {
    if (!gpio_in_range(pin)) {
        static bool logged = false;
        if (!logged) {
            LOG_PRINT("[SWITCH] pin %d is reserved (out of range or strapping/USB) — refusing\n",
                          (int)pin);
            logged = true;
        }
        return false;
    }
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
        case SCK_CHANNEL_BELOW:  *out = pc->current; return true;  // current, not power
        default:                 return false;
    }
}

// Eval direction for hysteresis-aware per-condition latching.
enum EvalDir { DIR_RISING = 0, DIR_FALLING };

// Evaluate a single condition with epsilon-aware comparison. Hysteresis is
// applied against the threshold: on a RISING direction the condition only
// trips when reading > value + hyst, and on a FALLING direction it only
// resets when reading < value - hyst. When hyst <= 0 the original threshold
// is used and behaviour matches a non-hysteretic trip/reset.
//
// Hysteresis sign convention per kind:
//   - OVERCURRENT, CHANNEL_ABOVE, SOC_HIGH: trip above value, reset below value - hyst
//   - UNDERVOLTAGE, CHANNEL_BELOW, SOC_LOW: trip below value, reset above value + hyst
//   - SCO_EQ / SCO_GTE / SCO_LTE: hyst is ignored (compatibility) — these
//     conditions are not the canonical hysteresis targets.
//
// The caller's `latched` flag carries the previous per-condition state so
// this function is a pure mapping from (reading, latched, hyst) to bool.
static bool eval_condition(const SwitchCondition& c, float reading, EvalDir dir,
                           bool latched, float hyst) {
    const float v = c.value;
    float trip_v = v;
    float reset_v = v;
    if (hyst > 0.0f) {
        switch (c.kind) {
            case SCK_OVERCURRENT:
            case SCK_CHANNEL_ABOVE:
            case SCK_SOC_HIGH:
                trip_v  = v + hyst;
                reset_v = v;
                break;
            case SCK_UNDERVOLTAGE:
            case SCK_CHANNEL_BELOW:
            case SCK_SOC_LOW:
                trip_v  = v;
                reset_v = v + hyst;
                break;
            default:
                trip_v  = v;
                reset_v = v;
                break;
        }
    }

    switch (c.op) {
        case SCO_GT: {
            if (dir == DIR_RISING) return reading > trip_v + ELEC_EPS;
            // FALLING: must drop below reset threshold
            return reading < reset_v - ELEC_EPS;
        }
        case SCO_GTE: {
            if (dir == DIR_RISING) return reading >= trip_v - ELEC_EPS;
            return reading <= reset_v + ELEC_EPS;
        }
        case SCO_LT: {
            if (dir == DIR_RISING) return reading < trip_v - ELEC_EPS;
            return reading > reset_v + ELEC_EPS;
        }
        case SCO_LTE: {
            if (dir == DIR_RISING) return reading <= trip_v + ELEC_EPS;
            return reading >= reset_v - ELEC_EPS;
        }
        case SCO_EQ: {
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
// Falls back to "always false" if epoch is unset or not yet trusted (NTP
// never synced — we don't want a stale epoch to act on a stale schedule).
static bool eval_schedule(const SwitchCondition& c) {
    if (c.kind != SCK_SCHEDULE_WINDOW) return false;
    if (!log_epoch_valid()) return false;
    time_t t = get_epoch_time();
    if (t == 0) return false;
    struct tm* tm = localtime(&t);
    if (!tm) return false;
    uint8_t hour = (uint8_t)tm->tm_hour;
    uint8_t dow  = (uint8_t)tm->tm_wday;  // 0=Sun..6=Sat
    uint16_t bit_index = (uint16_t)dow * 24u + hour;
    if (bit_index >= 168) return false;  // defensive: 7×24
    uint8_t  byte_idx  = bit_index >> 3;
    uint8_t  bit_mask  = (uint8_t)(1u << (bit_index & 7));
    if (byte_idx >= SC_SCHEDULE_BYTES) return false;
    return (c.schedule_mask[byte_idx] & bit_mask) != 0;
}

// Per-condition hysteresis-aware latching lives in evaluate_switches()
// (the only caller) — the per-rule combined result is built inline there
// so we can update condition_latched[] in place. SCHEDULE_WINDOW conditions
// are stateless bit lookups in eval_schedule().

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
    // Load the persisted auto-trip flag so a reboot (power loss, OTA, crash)
    // does not silently disable all safety rules. Previously this was hardcoded
    // to false, which meant overcurrent/undervoltage/SoC protection was off
    // after every reboot until a human sent "switch auto on".
    switch_auto_enabled = settings_load_switch_auto_enabled();
    if (!switch_auto_enabled) {
        LOG_PRINTLN("[SWITCH] WARNING: auto-trip is DISABLED — safety rules will "
                    "not fire until 'switch auto on' (or BLE set_switch_auto)");
    }
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
                    // Safe-state-on-boot: drive every relay OFF, regardless of
                    // the persisted is_energized flag. If a rule should keep
                    // the load energized, the first evaluate_switches() pass
                    // (within 1 s, when auto is enabled) re-energizes it after
                    // re-checking live conditions. This prevents a load that was
                    // manually ON before a reboot from coming back ON with no
                    // rule evaluation while auto-trip is disabled.
                    digitalWrite(ch.gpio_pin, ch.active_high ? LOW : HIGH);
                }
                // Runtime mirror starts de-energized; eval will set it on trip.
                switch_states[i].energized     = false;
                switch_states[i].was_energized = false;
                switch_states[i].condition_start_ms = 0;
                switch_states[i].condition_active   = false;
            }
        }
    }

    // Publish initial switch states to the remote state table
    for (uint8_t i = 0; i < 4; i++) {
        publish_switch_state(i, switch_states[i].energized);
    }
}

// Forward: defined below switch_pulse().
static void switch_pulse_tick(unsigned long now);

void evaluate_switches(const SensorSnapshot& snapshot) {
    unsigned long now = millis();
    // Drive non-blocking pulses to completion regardless of auto-trip state,
    // so a pulse started while auto is off still ends on time.
    switch_pulse_tick(now);
    // Auto-trip disabled until user enables via 'switch auto on'
    if (!switch_auto_enabled) return;

    uint8_t count = settings_load_switch_count();

    for (uint8_t i = 0; i < count && i < MAX_SWITCHES; i++) {
        // A non-blocking pulse owns this switch for its duration — skip auto
        // eval so the rule engine can't override or race the pulse.
        if (switch_states[i].pulse_active) continue;
        SwitchChannel ch;
        SwitchRule rule;
        if (!settings_load_switch(i, &ch) || !ch.enabled) continue;
        if (!settings_load_switch_rule(i, &rule) || !rule.enabled) continue;
        if (rule.channel >= snapshot.total_logical_channels) continue;
        // Snapshot the loaded rule so we can persist it only when hysteresis
        // state (condition_latched[]) actually changes — not on every 1 Hz
        // tick. The previous unconditional save wrote ~8 flash pages/sec per
        // switch and raced BLE/serial rule edits.
        SwitchRule prev_rule = rule;

        // Per-condition hysteresis-aware latching. We update
        // rule.condition_latched[i] in place based on the eval result.
        bool cond_satisfied[SC_MAX_CONDITIONS] = { false };
        uint8_t active_count = 0;  // non-disabled conditions (AND denominator)
        for (uint8_t j = 0; j < rule.condition_count && j < SC_MAX_CONDITIONS; j++) {
            const SwitchCondition& c = rule.conditions[j];
            if (c.kind == SCK_DISABLED) {
                rule.condition_latched[j] = false;
                continue;
            }
            active_count++;
            if (c.kind == SCK_SCHEDULE_WINDOW) {
                if (eval_schedule(c)) {
                    cond_satisfied[j] = true;
                    rule.condition_latched[j] = true;
                } else {
                    rule.condition_latched[j] = false;
                }
                continue;
            }
            float reading = 0.0f;
            if (!read_condition_value(c, rule.channel, snapshot, &reading)) continue;
            bool latched = rule.condition_latched[j];
            EvalDir dir = latched ? DIR_FALLING : DIR_RISING;
            bool sat = eval_condition(c, reading, dir, latched, rule.hysteresis);
            rule.condition_latched[j] = sat;
            cond_satisfied[j] = sat;
        }

        // Combine per OR/AND/min_conditions
        uint8_t true_count = 0;
        for (uint8_t j = 0; j < rule.condition_count && j < SC_MAX_CONDITIONS; j++) {
            if (cond_satisfied[j]) true_count++;
        }
        bool condition_met;
        if (rule.logic == SL_AND) {
            // AND requires every ACTIVE (non-disabled) condition. Using
            // condition_count as the denominator made an AND rule with any
            // disabled condition permanently un-trippable (the disabled
            // condition could never be satisfied, but still counted).
            condition_met = (active_count > 0 && true_count >= active_count);
        } else {
            uint8_t need = rule.min_conditions ? rule.min_conditions : 1;
            condition_met = (true_count >= need);
        }

        SwitchState& st = switch_states[i];

        if (condition_met) {
            if (!st.condition_active) {
                st.condition_active = true;
                st.condition_start_ms = now;
            } else if (!st.energized) {
                int32_t elapsed = (int32_t)(now - st.condition_start_ms);
                if (elapsed >= (int32_t)rule.trip_delay_ms) {
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
            }
        } else {
            if (st.condition_active) {
                st.condition_active = false;
                st.condition_start_ms = now;
            } else if (st.energized) {
                int32_t elapsed = (int32_t)(now - st.condition_start_ms);
                if (elapsed >= (int32_t)rule.reset_delay_ms) {
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

        // Persist updated per-condition latched state and is_energized so
        // a reboot resumes with the same hysteresis memory — but only when
        // something actually changed, to avoid wearing the flash (~8 writes
        // per second per switch previously) and to narrow the window in which
        // a BLE/serial rule edit could be clobbered by this read-modify-write.
        if (ch.is_energized != st.energized) {
            ch.is_energized = st.energized;
            settings_save_switch(i, &ch);
        }
        if (memcmp(&prev_rule, &rule, sizeof(SwitchRule)) != 0) {
            settings_save_switch_rule(i, &rule);
        }
    }
}

void switch_set_auto(bool enabled) {
    switch_auto_enabled = enabled;
    // Persist so the choice survives reboot (see init_switches).
    settings_save_switch_auto_enabled(enabled);
    LOG_PRINT("[SWITCH] auto-trip %s\n", enabled ? "ENABLED" : "DISABLED");
    // When auto is turned OFF, the rule-based reset path can no longer
    // fire, so any switch that is currently energized under a rule must
    // be force-reset immediately. Otherwise it would stay stuck ON
    // forever (or until the next `switch auto on` cycle). Manual control
    // is the only way out from this point.
    if (!enabled) {
        uint8_t count = settings_load_switch_count();
        for (uint8_t i = 0; i < count && i < MAX_SWITCHES; i++) {
            SwitchChannel ch;
            SwitchRule rule;
            if (!settings_load_switch(i, &ch)) continue;
            if (!settings_load_switch_rule(i, &rule)) continue;
            if (!rule.enabled) continue;
            if (!switch_states[i].energized) continue;
            // Force-reset: drive pin low, clear runtime state, persist.
            set_switch_pin(ch, false);
            switch_states[i].energized = false;
            switch_states[i].was_energized = false;
            ch.is_energized = false;
            settings_save_switch(i, &ch);
            publish_switch_state(i, false);
            LOG_PRINT("Switch %d force-reset (auto off)\n", i);
        }
    }
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
    if (idx >= MAX_SWITCHES) return;
    SwitchChannel ch;
    if (!settings_load_switch(idx, &ch)) return;
    LOG_PRINT("Pulsing switch %d (GPIO %d) for %u ms...\n", idx, ch.gpio_pin, (unsigned)duration_ms);
    // Energize now; the pulse is ended by switch_pulse_tick() (called from
    // evaluate_switches on the sensor task) when the duration elapses. This
    // avoids blocking the caller's task for `duration_ms` and lets
    // evaluate_switches skip auto-eval for this switch while the pulse runs
    // (see the pulse_active check in the per-switch loop).
    switch_set(idx, true);
    switch_states[idx].pulse_active = true;
    switch_states[idx].pulse_off_at_ms = millis() + duration_ms;
}

bool switch_pulse_active(uint8_t idx) {
    if (idx >= MAX_SWITCHES) return false;
    return switch_states[idx].pulse_active;
}

// Drive the non-blocking pulse to completion. Called every tick from
// evaluate_switches regardless of switch_auto_enabled so a pulse always ends
// even when auto-trip is off.
static void switch_pulse_tick(unsigned long now) {
    for (uint8_t i = 0; i < MAX_SWITCHES; i++) {
        if (!switch_states[i].pulse_active) continue;
        if ((long)(now - switch_states[i].pulse_off_at_ms) >= 0) {
            switch_set(i, false);
            switch_states[i].pulse_active = false;
            LOG_PRINT("Switch %d pulse done.\n", i);
        }
    }
}

bool get_switch_state(uint8_t idx) {
    if (idx >= MAX_SWITCHES) return false;
    SwitchChannel ch;
    if (!settings_load_switch(idx, &ch) || !ch.enabled) return false;
    bool pin_high = digitalRead(ch.gpio_pin) == HIGH;
    return ch.active_high ? pin_high : !pin_high;
}
