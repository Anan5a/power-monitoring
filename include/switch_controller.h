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
#define SC_SCHEDULE_BYTES    21   // 168 bits = 21 bytes, 7×24h bitmask

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
    bool     condition_latched[SC_MAX_CONDITIONS]; // per-condition trip state
    SwitchCondition conditions[SC_MAX_CONDITIONS];
};
// Lock persisted blob sizes so a field reorder/packing change is caught at
// compile time instead of silently mis-decoding NVS. The rule blob is written
// on hysteresis-state changes (see switch_controller.cpp) and loaded on every
// eval tick, so a layout drift would corrupt relay behavior.
static_assert(sizeof(SwitchChannel) == 30, "SwitchChannel size drift");
static_assert(sizeof(SwitchCondition) == 32, "SwitchCondition size drift");
static_assert(sizeof(SwitchRule) == 148, "SwitchRule size drift");

void init_switches();
void evaluate_switches(const SensorSnapshot& snapshot);
void switch_set(uint8_t idx, bool is_energized);
void switch_pulse(uint8_t idx, uint32_t duration_ms);  // non-blocking; ends via evaluate_switches()
bool switch_pulse_active(uint8_t idx);                 // true while a pulse is in flight
bool get_switch_state(uint8_t idx);
void switch_set_auto(bool enabled);
bool switch_get_auto_enabled();  // telemetry snapshot accessor

// THREADING MODEL:
//   switch_set(), switch_pulse(), switch_set_auto() and get_switch_state()
//   may be called from any context (loop, BLE task, serial CLI). They are
//   short and write through NVS / GPIO directly.
//
//   evaluate_switches() is the single writer of switch_states[i] in the
//   automatic (rule-driven) path and runs on the sensor task (Core 1) at
//   1 Hz. The auto path never races with itself, but the manual path
//   (switch_set) and the auto path can collide: a manual set can be
//   overwritten by the next sensor tick if `switch auto on` is in effect.
//   Callers that want durable manual control must `switch auto off` first.
//
//   The legacy relay-controller mutex is NOT used here; switches are
//   accessed from a single auto-writer + concurrent manual callers, and
//   each NVS row update is independent.

// Human-readable names for SwitchConditionKind / SwitchConditionOp / SwitchLogic
const char* switch_condition_kind_name(uint8_t kind);
const char* switch_condition_op_name(uint8_t op);
const char* switch_logic_name(uint8_t logic);

// Validate a GPIO for use as a switch pin. Returns false if the pin is
// outside the board's legal range OR is on the per-board denylist
// (strapping pins, USB, flash). Used by the BLE / serial set_switch path
// to refuse unsafe pins before they reach NVS.
bool switch_gpio_allowed(int8_t pin);

// Initialize a SwitchRule with a single OR overcurrent condition.
void switch_rule_default_init(SwitchRule* rule, uint8_t switch_idx, uint8_t channel);

#endif
