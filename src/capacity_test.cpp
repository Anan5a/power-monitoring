// Capacity test subsystem
//
// Two modes:
//   MANUAL     — user starts/stops; the firmware integrates |discharge| current
//                into measured_Ah between the two calls.
//   AUTOMATED  — user supplies a load_switch_idx and cutoff_v. The firmware
//                enables the switch at start, then disables it on the first
//                sample where V < cutoff_v, then reports.
//
// Both modes record start_SoC_pct, sample_count, and duration. The result is
// stored in g_last_result[channel] with a pending flag for the next BLE notify.
//
// Struct size summary:
//   CapacityTestResult  : 32 bytes
//     valid(1) + channel(1) + mode(1) + pad(1) +
//     measured_Ah(4) + rated_Ah(4) + soh_pct(4) +
//     duration_s(4) + samples(4) + start_SoC_pct(4) + end_SoC_pct(4) = 32
//   CapacityTestState   : 32 bytes (active+mode+started_ms+start_SoC+measured+
//                                 samples+load_switch+cutoff+last_report+pending)
//
// Behavior:
//   1. init_capacity_test() zeros g_last_result[ch] for all 16 channels
//   2. capacity_test_start() refuses if no profile, no load (automated), or
//      already running; persists BatteryState
//   3. update_capacity_test_monitor() each 1s tick integrates |I|*dt/3600
//      into measured_Ah, increments sample_count, recomputes SoC indirectly
//      via cycle_counter's last_SoC_pct
//   4. In AUTOMATED, when V < cutoff_v the module calls
//      switch_set(load_idx,false) and finalize_result()
//   5. finalize_result() builds the CapacityTestResult, queues it, surfaces
//      SoH to the next telemetry publish, and persists
//   6. capacity_test_get_result() returns the most recent result (no auto-clear;
//      callers re-read on demand)
//   7. BLE layer can call capacity_test_is_active() to report live status

#include "capacity_test.h"
#include "battery_profile.h"
#include "battery_state.h"
#include "switch_controller.h"
#include "settings_manager.h"  // settings_load_switch
#include "cycle_counter.h"
#include "telemetry.h"          // telemetry_publish_capacity_test_soh
#include <Arduino.h>
#include <string.h>

namespace {
constexpr uint32_t kReportIntervalMs = 60000;  // 60s progress notify

CapacityTestResult g_last_result[MAX_LOGICAL_CHANNELS] = {};

const char* mode_name(uint8_t mode) {
    return mode == CAP_TEST_AUTOMATED ? "automated" : "manual";
}
}  // namespace

void init_capacity_test() {
    for (uint8_t i = 0; i < MAX_LOGICAL_CHANNELS; i++) {
        g_last_result[i] = CapacityTestResult{};
    }
}

bool capacity_test_is_active(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return false;
    BatteryState st;
    cycle_counter_get(channel, &st);
    return st.test.active;
}

bool capacity_test_start(uint8_t channel, uint8_t mode, int8_t load_switch_idx, float cutoff_v) {
    if (channel >= MAX_LOGICAL_CHANNELS) return false;
    uint8_t pid = battery_channel_profile(channel);
    if (pid == BATTERY_CHANNEL_NO_BINDING) return false;
    const BatteryChemistryProfile* bp = battery_profile_get(pid);
    if (!bp || bp->rated_capacity_Ah <= 0.001f) return false;

    BatteryState st;
    cycle_counter_get(channel, &st);
    if (st.test.active) return false;  // already running

    st.test = CapacityTestState{};
    st.test.active = true;
    st.test.mode = mode;
    st.test.started_ms = millis();
    st.test.measured_Ah = 0.0f;
    st.test.sample_count = 0;
    st.test.load_switch_idx = (mode == CAP_TEST_AUTOMATED) ? load_switch_idx : (int8_t)-1;
    st.test.cutoff_v = cutoff_v;
    st.test.last_report_ms = millis();
    st.test.result_pending = false;
    st.test.start_SoC_pct = st.last_SoC_pct;
    cycle_counter_put(channel, &st);

    if (mode == CAP_TEST_AUTOMATED) {
        if (load_switch_idx < 0) {
            // Refused: must supply a switch
            st.test.active = false;
            cycle_counter_put(channel, &st);
            return false;
        }
        SwitchChannel ch;
        if (!settings_load_switch(load_switch_idx, &ch) || !ch.enabled) {
            st.test.active = false;
            cycle_counter_put(channel, &st);
            return false;
        }
        switch_set((uint8_t)load_switch_idx, true);
    }
    return true;
}

static void finalize_result(uint8_t channel, BatteryState& st) {
    CapacityTestResult r = {};
    r.valid = true;
    r.channel = channel;
    r.mode = st.test.mode;
    r.measured_Ah = st.test.measured_Ah;
    uint8_t pid = battery_channel_profile(channel);
    const BatteryChemistryProfile* bp = battery_profile_get(pid);
    r.rated_Ah = bp ? bp->rated_capacity_Ah : 0.0f;
    r.soh_pct = (r.rated_Ah > 0.001f) ? (r.measured_Ah / r.rated_Ah) * 100.0f : 0.0f;
    r.duration_s = (millis() - st.test.started_ms) / 1000U;
    r.samples = st.test.sample_count;
    r.start_SoC_pct = st.test.start_SoC_pct;
    r.end_SoC_pct = st.last_SoC_pct;
    g_last_result[channel] = r;
    st.test.active = false;
    st.test.result_pending = true;
    cycle_counter_put(channel, &st);
    // Surface SoH to the next telemetry publish (one-shot, auto-cleared)
    telemetry_publish_capacity_test_soh(r.soh_pct);
}

CapacityTestResult capacity_test_stop(uint8_t channel) {
    CapacityTestResult empty = {};
    if (channel >= MAX_LOGICAL_CHANNELS) return empty;
    BatteryState st;
    cycle_counter_get(channel, &st);
    if (!st.test.active) return empty;

    if (st.test.mode == CAP_TEST_AUTOMATED && st.test.load_switch_idx >= 0) {
        switch_set((uint8_t)st.test.load_switch_idx, false);
    }
    finalize_result(channel, st);
    return g_last_result[channel];
}

bool capacity_test_get_result(uint8_t channel, CapacityTestResult* out) {
    if (!out || channel >= MAX_LOGICAL_CHANNELS) return false;
    if (!g_last_result[channel].valid) return false;
    *out = g_last_result[channel];
    return true;
}

float capacity_test_last_soh_pct(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return -1.0f;
    if (!g_last_result[channel].valid) return -1.0f;
    return g_last_result[channel].soh_pct;
}

void update_capacity_test_monitor(const SensorSnapshot& snap, float dt_seconds) {
    uint32_t now = millis();
    for (uint8_t ch = 0; ch < MAX_LOGICAL_CHANNELS; ch++) {
        BatteryState st;
        cycle_counter_get(ch, &st);
        if (!st.test.active) continue;

        float i = get_channel_current(snap, ch);
        float v = get_channel_voltage(snap, ch);
        if (i < -CYCLE_CURRENT_DEADZONE_A) {
            float ah = -i * dt_seconds / 3600.0f;
            st.test.measured_Ah += ah;
        }
        st.test.sample_count++;

        // AUTOMATED cutoff
        if (st.test.mode == CAP_TEST_AUTOMATED &&
            st.test.load_switch_idx >= 0 &&
            v < st.test.cutoff_v) {
            switch_set((uint8_t)st.test.load_switch_idx, false);
            finalize_result(ch, st);
            continue;
        }

        // Persist every tick (1s) — measured_Ah/sample_count are too important
        // to risk a crash loss. The cost is small: a single 50-60 byte NVS write.
        st.test.last_report_ms = now;
        cycle_counter_put(ch, &st);
    }
}
