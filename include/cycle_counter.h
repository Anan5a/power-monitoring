#ifndef CYCLE_COUNTER_H
#define CYCLE_COUNTER_H

#include <stdint.h>
#include "sensor_manager.h"
#include "battery_state.h"

// Direction-flip hysteresis: close the partial session only if the absolute
// SoC delta since the last flip exceeds 5% (in capacity_Ah terms).
#define CYCLE_DOD_HYSTERESIS_PCT 5.0f
// Hysteresis in current magnitude (A) to avoid jitter around zero crossing
#define CYCLE_CURRENT_DEADZONE_A 0.02f

// Initialize: load per-channel accumulators from NVS.
void init_cycle_counter();

// Drive the cycle-counter 1s tick. `dt_seconds` is typically 1.0.
// `v` and `i` are the latest snapshot readings for the channel; the cycle
// counter uses them to update last_V/last_I and to update SoC estimates
// when a profile is bound. Bounded heap; no allocation.
void update_cycle_counter(const SensorSnapshot& snap, float dt_seconds);

// Snapshots the current cycle state for a channel (used by BLE/serial).
void cycle_counter_get(uint8_t channel, BatteryState* out);

// Atomic point-in-time copy of a channel's BatteryState under a single
// critical-section lock. Preferred over cycle_counter_get() for telemetry
// and any caller that reads multiple BatteryState fields in a row, because
// the fields are guaranteed to come from the same sensorTask frame. See
// battery_lock.h for the lock discipline.
void cycle_counter_snapshot(uint8_t channel, BatteryState* out);

// Writes updated BatteryState back (used by capacity_test monitor to update
// the in-memory cache after integrating measured_Ah).
void cycle_counter_put(uint8_t channel, const BatteryState* in);

// Test helper: reset accumulator for a channel.
void cycle_counter_reset(uint8_t channel);

// ── Per-field accessors (preferred API for telemetry) ────────────────────────
// All of these are thin forwarders over the cached g_state[] entry; calling
// them does not allocate and is safe from the network task. They load the
// channel state from NVS on first access (same lazy-load as
// cycle_counter_get). Returns 0 / false / -1 for invalid channel indices.

// DoD-weighted cycle count for the channel. Source of truth for
// TelemetryBattery.equivalent_full_cycles.
float    cycle_counter_get_equivalent_full_cycles(uint8_t logical_channel);
float    cycle_counter_get_cumulative_Ah_in(uint8_t logical_channel);
float    cycle_counter_get_cumulative_Ah_out(uint8_t logical_channel);
float    cycle_counter_get_last_soc_pct(uint8_t logical_channel);
float    cycle_counter_get_last_voltage(uint8_t logical_channel);
float    cycle_counter_get_last_current(uint8_t logical_channel);
uint32_t cycle_counter_get_last_update_ms(uint8_t logical_channel);

// True when the channel has a profile binding. telemetry_build() uses this
// to decide whether to emit a row in the unified battery[] array. Unbound
// channels are skipped.
bool     cycle_counter_is_active(uint8_t logical_channel);

#endif // CYCLE_COUNTER_H
