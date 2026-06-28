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

struct SwitchRule {
    uint8_t switch_idx;       // index of switch this rule controls
    uint8_t channel;          // logical sensor channel used for thresholds
    float overcurrent_A;      // trip if current > this (0 = disabled)
    float undervoltage_V;     // trip if voltage < this (0 = disabled)
    float soc_low_pct;        // trip if SoC < this (0 = disabled)
    float soc_high_pct;       // trip if SoC > this (0 = disabled)
    uint16_t trip_delay_ms;   // must exceed this duration to trip
    uint16_t reset_delay_ms;  // must stay below threshold this long to reset
    bool enabled;
};

void init_switches();
void evaluate_switches(const SensorSnapshot& snapshot);
void switch_set(uint8_t idx, bool is_energized);
void switch_pulse(uint8_t idx, uint32_t duration_ms);
bool get_switch_state(uint8_t idx);
void switch_set_auto(bool enabled);

#endif
