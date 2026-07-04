#ifndef BATTERY_STATE_H
#define BATTERY_STATE_H

#include <stdint.h>
#include <stdbool.h>
#include "sensor_pod.h"      // MAX_LOGICAL_CHANNELS
#include "battery_profile.h"

// ── Size summary ──────────────────────────────────────────────────────────────
// ChannelBinding   : 16 bytes (one uint8 per logical channel)
// BatteryState     : ~52 bytes
//   cumulative_Ah_in (4) + cumulative_Ah_out (4) + equivalent_full_cycles (4) +
//   last_SoC_pct (4) + last_V (4) + last_I (4) + last_update_ms (4) +
//   current_session_dod_Ah (4) + last_session_start_pct (4) = 36
//   + CapacityTestState (16) = 52
// CapacityTestState:
//   active (1) + mode (1) + pad (2) + started_ms (4) + start_SoC_pct (4) +
//   measured_Ah (4) + sample_count (4) + load_switch_idx (1) + cutoff_v (4) +
//   last_report_ms (4) = ~28
// ──────────────────────────────────────────────────────────────────────────────

#define BATTERY_CHANNEL_NO_BINDING 0xFF

// Mapping from logical channel → profile id (0..15) or 0xFF for "no battery"
void init_battery_bindings();
uint8_t battery_channel_profile(uint8_t channel);
bool   battery_channel_set_profile(uint8_t channel, uint8_t profile_id);
void   battery_channel_clear(uint8_t channel);

// Per-channel battery state (cycle counter + capacity test state)
struct CapacityTestState {
    bool     active;            // true while a test is in progress
    uint8_t  mode;              // 0=MANUAL, 1=AUTOMATED
    uint32_t started_ms;
    float    start_SoC_pct;
    float    measured_Ah;
    uint32_t sample_count;
    int8_t   load_switch_idx;   // -1 if not set
    float    cutoff_v;
    uint32_t last_report_ms;
    bool     result_pending;    // true when a result is queued for BLE notify
};

struct BatteryState {
    // Cycle counter (DoD-weighted)
    float    cumulative_Ah_in;
    float    cumulative_Ah_out;
    float    equivalent_full_cycles;
    float    last_SoC_pct;
    float    last_V;
    float    last_I;
    uint32_t last_update_ms;
    float    current_session_dod_Ah;
    float    last_session_start_pct;
    // Capacity test
    CapacityTestState test;
};

void init_battery_states();
bool battery_state_load(uint8_t channel, BatteryState* out);
bool battery_state_save(uint8_t channel, const BatteryState* in);
void battery_state_reset(uint8_t channel);  // zero out the cycle accumulator

#endif // BATTERY_STATE_H
