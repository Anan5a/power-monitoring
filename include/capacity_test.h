#ifndef CAPACITY_TEST_H
#define CAPACITY_TEST_H

#include <stdint.h>
#include <stdbool.h>
#include "sensor_manager.h"
#include "battery_state.h"

enum CapacityTestMode {
    CAP_TEST_MANUAL = 0,
    CAP_TEST_AUTOMATED = 1,
};

// CapacityTestResult is the payload returned at stop or on demand.
struct CapacityTestResult {
    bool     valid;
    uint8_t  channel;
    uint8_t  mode;          // CapacityTestMode
    float    measured_Ah;
    float    rated_Ah;
    float    soh_pct;       // 100 * measured / rated
    uint32_t duration_s;
    uint32_t samples;
    float    start_SoC_pct;
    float    end_SoC_pct;
    bool     invalid_profile;  // true when the test ran on a channel that
                               // was unbound or pointed at a missing profile
                               // at finalisation time
};

// Initialize: nothing to load (per-channel state is in BatteryState).
void init_capacity_test();

// Returns true if a test is active on this channel.
bool capacity_test_is_active(uint8_t channel);

// Start a test. AUTOMATED requires load_switch_idx >= 0 and that switch to
// be enabled. MANUAL integrates current until capacity_test_stop().
// Returns false on refusal (no battery profile, automated load invalid, etc.)
bool capacity_test_start(uint8_t channel, uint8_t mode, int8_t load_switch_idx, float cutoff_v);

// Stop the test and return a result. The result is also stored in
// g_last_result[channel] and queued for the next BLE publish / BLE notify.
// Calling stop on an idle channel is a no-op and returns valid=false.
CapacityTestResult capacity_test_stop(uint8_t channel);

// Get the most recent result for a channel (cleared on read by BLE).
bool capacity_test_get_result(uint8_t channel, CapacityTestResult* out);

// Returns the SoH% from the most recent result for a channel, or -1 if no
// result is pending. Used by telemetry_build() to populate
// TelemetryBattery.capacity_test_soh_pct / capacity_test_soh_valid. Negative
// sentinel avoids confusion with a legitimate 0% SoH.
float capacity_test_last_soh_pct(uint8_t logical_channel);

// Per-second tick: integrate current, run the AUTOMATED cut-off logic.
void update_capacity_test_monitor(const SensorSnapshot& snap, float dt_seconds);

#endif // CAPACITY_TEST_H
