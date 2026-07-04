#ifndef SWITCH_CONTROLLER_H
#define SWITCH_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>
#include "sensor_pod.h"

enum SwitchType {
    SW_RELAY = 0,
    SW_MOSFET_LOW_SIDE,
    SW_MOSFET_HIGH_SIDE,
    SW_SSR,
    SW_EXPANDER
};

struct SwitchChannel {
    uint8_t idx;            // switch index 0..N-1
    uint8_t type;           // SwitchType
    uint8_t gpio_pin;
    bool active_high;       // true: HIGH = energized
    bool enabled;
    bool is_energized;        // persisted physical state
    char name[24];
};

// ── Switch rule "list-shape" ──────────────────────────────────────────────────
// A rule is a list of SwitchConditions combined with a logic (AND/OR) and a
// minimum count. The legacy flat-shape (overcurrent_A, undervoltage_V, …) is
// still accepted on the wire and is translated into a single OR condition per
// non-zero field. The struct below always stores the new shape; the BLE/serial
// path is responsible for translation.
#define SC_MAX_CONDITIONS    4
#define SC_SCHEDULE_BYTES    28   // 4 × 7 days bitmask

enum SwitchConditionKind {
    SCK_DISABLED = 0,
    SCK_OVERCURRENT,
    SCK_UNDERVOLTAGE,
    SCK_SOC_LOW,
    SCK_SOC_HIGH,
    SCK_CHANNEL_ABOVE,    // value compared to another channel's current
    SCK_CHANNEL_BELOW,
    SCK_SCHEDULE_WINDOW,  // 7×24h bitmask in schedule_mask[]
};

enum SwitchConditionOp {
    SCO_GT = 0,
    SCO_LT,
    SCO_GTE,
    SCO_LTE,
    SCO_EQ,
};

struct SwitchCondition {
    uint8_t kind;       // SwitchConditionKind
    uint8_t op;         // SwitchConditionOp
    int8_t  ref_channel; // -1 = current rule channel
    uint8_t pad;        // explicit pad for layout stability
    float   value;
    uint8_t schedule_mask[SC_SCHEDULE_BYTES];
};

enum SwitchLogic {
    SL_OR = 0,
    SL_AND,
};

struct SwitchRule {
    uint8_t  switch_idx;       // index of switch this rule controls
    uint8_t  channel;          // logical sensor channel used for thresholds
    uint16_t trip_delay_ms;    // must exceed this duration to trip
    uint16_t reset_delay_ms;   // must stay below threshold this long to reset
    uint8_t  logic;            // SwitchLogic (OR by default)
    uint8_t  min_conditions;   // minimum # of conditions that must be true
    uint8_t  condition_count;  // number of populated conditions
    uint8_t  enabled;
    float    hysteresis;       // dead-band on/off threshold difference
    SwitchCondition conditions[SC_MAX_CONDITIONS];
};

void init_switches();
void evaluate_switches(const SensorSnapshot& snapshot);
void switch_set(uint8_t idx, bool is_energized);
void switch_pulse(uint8_t idx, uint32_t duration_ms);
bool get_switch_state(uint8_t idx);
void switch_set_auto(bool enabled);
bool switch_get_auto_enabled();  // telemetry snapshot accessor

// Human-readable names for SwitchConditionKind / SwitchConditionOp / SwitchLogic
const char* switch_condition_kind_name(uint8_t kind);
const char* switch_condition_op_name(uint8_t op);
const char* switch_logic_name(uint8_t logic);

// Initialize a SwitchRule with a single OR overcurrent condition.
void switch_rule_default_init(SwitchRule* rule, uint8_t switch_idx, uint8_t channel);

#endif
