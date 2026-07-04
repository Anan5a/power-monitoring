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
//   CapacityTestResult  : ~36 bytes (incl. invalid_profile flag + pad)
//   CapacityTestState   : ~32 bytes (in battery_state.h)
//
// Behavior:
//   1. init_capacity_test() zeros g_last_result[ch] for all 16 channels
//   2. capacity_test_start() refuses if no profile, no load (automated), or
//      already running; persists BatteryState
//   3. update_capacity_test_monitor() each 1s tick integrates |I|*dt/3600
//      into measured_Ah, increments sample_count, recomputes SoC indirectly
//      via cycle_counter's last_SoC_pct
//   4. If the channel becomes unbound mid-test (or its profile is missing),
//      the test is auto-cancelled and finalised with invalid_profile=true
//   5. In AUTOMATED, when V < cutoff_v the module calls
//      switch_set(load_idx,false) and finalize_result()
//   6. finalize_result() builds the CapacityTestResult, queues it, surfaces
//      SoH to the next telemetry publish, and persists
//   7. Per-channel state is persisted on (a) state change (start/stop/
//      finalise), (b) the 60 s test-boundary, and (c) the 5-min
//      cycle_counter persist boundary. This drops the per-tick 72 B
//      NVS write that used to fire every 1 s
//   8. A capacity test whose started_ms is more than 7 days in the past at
//      NVS load time is treated as a crash recovery case and cleared
//      (see cycle_counter.cpp::recover_stale_test_if_needed)

#include "capacity_test.h"
#include "battery_lock.h"
#include "battery_nvs.h"
#include "battery_profile.h"
#include "battery_state.h"
#include "switch_controller.h"
#include "settings_manager.h"  // settings_load_switch
#include "cycle_counter.h"
#include "telemetry.h"          // telemetry_publish_capacity_test_soh
#include "log_serial.h"
#include <Arduino.h>
#include <string.h>

namespace {
constexpr uint32_t kReportIntervalMs      = 60000;  // 60s progress notify
constexpr uint32_t kTestPersistIntervalMs = 60000;  // 60s persist cadence
constexpr uint32_t kCyclePersistIntervalMs = 300000; // 5 min (matches cycle_counter)

CapacityTestResult g_last_result[MAX_LOGICAL_CHANNELS] = {};
uint32_t g_last_test_persist_ms = 0;

const char* mode_name(uint8_t mode) {
    return mode == CAP_TEST_AUTOMATED ? "automated" : "manual";
}

// True when the channel has a bound profile and the profile registry has a
// matching non-sparse entry. False means we cannot run a capacity test on
// this channel — auto-cancel and mark the result invalid.
static bool channel_profile_resolved(uint8_t channel, const BatteryChemistryProfile** out) {
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return false;
    const BatteryChemistryProfile* bp = battery_profile_get(battery_channel_profile(channel));
    if (!bp) return false;
    if (out) *out = bp;
    return true;
}

// Forward decls: defined later in this TU at file scope. The auto_cancel
// helper below is moved out of the anonymous namespace and below the
// finalize_result defs to avoid a name-collision with the file-scope
// overload set.
}  // namespace

void finalize_result(uint8_t channel, BatteryState& st);
void finalize_result_unbound(uint8_t channel, BatteryState& st);

void init_capacity_test() {
    for (uint8_t i = 0; i < MAX_LOGICAL_CHANNELS; i++) {
        g_last_result[i] = CapacityTestResult{};
    }
    g_last_test_persist_ms = millis();
}

bool capacity_test_is_active(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return false;
    BatteryState st;
    cycle_counter_get(channel, &st);
    return st.test.active;
}

bool capacity_test_start(uint8_t channel, uint8_t mode, int8_t load_switch_idx, float cutoff_v) {
    if (channel >= MAX_LOGICAL_CHANNELS) return false;
    const BatteryChemistryProfile* bp = nullptr;
    if (!channel_profile_resolved(channel, &bp)) return false;
    if (bp->rated_capacity_Ah <= 0.001f) return false;

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

// Finalise the running test on `ch` with a result. Marks the test as
// inactive, queues the result for BLE / telemetry, and persists. Defined
// here (vs anonymous namespace) so auto_cancel_unbound can call it.
//
// When the profile is missing, the result is queued with invalid_profile=true
// and soh_pct=0 so the dashboard can show "test abandoned" rather than a
// misleading 0% SoH reading.
void finalize_result(uint8_t channel, BatteryState& st) {
    CapacityTestResult r = {};
    r.valid = true;
    r.channel = channel;
    r.mode = st.test.mode;
    r.measured_Ah = st.test.measured_Ah;
    uint8_t pid = battery_channel_profile(channel);
    const BatteryChemistryProfile* bp = (pid != BATTERY_CHANNEL_NO_BINDING)
                                       ? battery_profile_get(pid)
                                       : nullptr;
    if (!bp) {
        // Profile went missing mid-test. Surface a clear signal instead of
        // a 0% SoH that would look like a real measurement.
        r.invalid_profile = true;
        r.rated_Ah = 0.0f;
        r.soh_pct = 0.0f;
    } else {
        r.rated_Ah = bp->rated_capacity_Ah;
        r.soh_pct = (r.rated_Ah > 0.001f) ? (r.measured_Ah / r.rated_Ah) * 100.0f : 0.0f;
    }
    r.duration_s = (millis() - st.test.started_ms) / 1000U;
    r.samples = st.test.sample_count;
    r.start_SoC_pct = st.test.start_SoC_pct;
    r.end_SoC_pct = st.last_SoC_pct;
    g_last_result[channel] = r;
    st.test.active = false;
    st.test.result_pending = true;
    cycle_counter_put(channel, &st);
    // Surface SoH to the next telemetry publish (one-shot, auto-cleared).
    // We skip the side-channel entirely when the result is invalid — a 0%
    // SoH from a missing profile would be misleading.
    if (!r.invalid_profile) {
        telemetry_publish_capacity_test_soh(r.soh_pct);
    }
}

// Force-cancel variant for the channel-unbound case. Keeps a record of the
// partial measurement so the user can inspect it via BLE, but sets
// invalid_profile so the dashboard knows not to treat the SoH as
// authoritative.
void finalize_result_unbound(uint8_t channel, BatteryState& st) {
    CapacityTestResult r = {};
    r.valid = true;
    r.channel = channel;
    r.mode = st.test.mode;
    r.measured_Ah = st.test.measured_Ah;
    r.rated_Ah = 0.0f;
    r.soh_pct = 0.0f;
    r.invalid_profile = true;
    r.duration_s = (millis() - st.test.started_ms) / 1000U;
    r.samples = st.test.sample_count;
    r.start_SoC_pct = st.test.start_SoC_pct;
    r.end_SoC_pct = st.last_SoC_pct;
    g_last_result[channel] = r;
    st.test.active = false;
    st.test.result_pending = true;
    cycle_counter_put(channel, &st);
}

static void auto_cancel_unbound(uint8_t ch) {
    LOG_PRINT("[cap_test] ch%u auto-cancelled (channel unbound)\n", (unsigned)ch);
    BatteryState st;
    cycle_counter_get(ch, &st);
    if (!st.test.active) return;
    finalize_result_unbound(ch, st);
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
    if (g_last_result[channel].invalid_profile) return -1.0f;
    return g_last_result[channel].soh_pct;
}

void update_capacity_test_monitor(const SensorSnapshot& snap, float dt_seconds) {
    uint32_t now = millis();
    for (uint8_t ch = 0; ch < MAX_LOGICAL_CHANNELS; ch++) {
        BatteryState st;
        cycle_counter_get(ch, &st);
        if (!st.test.active) continue;

        // Channel unbound or profile missing: auto-cancel and skip. We
        // do NOT auto-resume — the operator must rebind a profile before
        // restarting the test.
        if (!channel_profile_resolved(ch, nullptr)) {
            auto_cancel_unbound(ch);
            continue;
        }

        float i = get_channel_current(snap, ch);
        float v = get_channel_voltage(snap, ch);
        if (i < -CYCLE_CURRENT_DEADZONE_A) {
            float ah = -i * dt_seconds / 3600.0f;
            st.test.measured_Ah += ah;
        }
        st.test.sample_count++;

        // AUTOMATED cutoff
        bool state_changed = false;
        if (st.test.mode == CAP_TEST_AUTOMATED &&
            st.test.load_switch_idx >= 0 &&
            v < st.test.cutoff_v) {
            switch_set((uint8_t)st.test.load_switch_idx, false);
            finalize_result(ch, st);
            state_changed = true;
            continue;
        }

        // Persist policy:
        //   - Always persist on state changes (handled above by finalise)
        //   - On the 60 s test-boundary, persist all active tests
        //   - On the 5 min cycle-counter boundary, persist (handled there
        //     too, but we double-write here to keep the cadence symmetric)
        // The previous implementation wrote every 1 s; that is overkill for
        // a test that already runs for hours.
        st.test.last_report_ms = now;
        bool persist_now = (now - g_last_test_persist_ms >= kTestPersistIntervalMs);
        if (persist_now || state_changed) {
            cycle_counter_put(ch, &st);
        }
    }
    if (now - g_last_test_persist_ms >= kTestPersistIntervalMs) {
        g_last_test_persist_ms = now;
    }
    // Mirror the cycle_counter 5-min boundary: if we're past the cycle
    // counter's next-persist deadline, also persist active test state. The
    // cycle_counter itself will persist on the same trigger; this just
    // covers tests that finished in the last 5 min and haven't been picked
    // up by the 60 s boundary yet.
    static uint32_t s_last_cycle_persist_ms = 0;
    if (now - s_last_cycle_persist_ms >= kCyclePersistIntervalMs) {
        s_last_cycle_persist_ms = now;
        for (uint8_t ch = 0; ch < MAX_LOGICAL_CHANNELS; ch++) {
            BatteryState st;
            cycle_counter_get(ch, &st);
            if (st.test.active) cycle_counter_put(ch, &st);
        }
    }
}
