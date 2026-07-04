// DoD-weighted cycle counter
//
// State machine (per logical channel that has a battery profile):
//   - Track last_session_start_pct (SoC at start of current partial session).
//   - Each 1s tick:
//       1. Read current. Classify direction (charge/discharge/idle).
//       2. Update cumulative_Ah_in / cumulative_Ah_out based on direction.
//       3. Update last_SoC_pct from net coulombs / rated capacity.
//       4. If direction differs from the previous tick AND the SoC delta
//          |last_SoC_pct - last_session_start_pct| > 5%:
//              equivalent_full_cycles += |delta|/100
//              last_session_start_pct = last_SoC_pct
//              persist immediately
//   - Persist all channels every 5 minutes.
//
//   Example 1: 90% -> 40% in one shot, then flipped to charge:
//       |delta| = 50 > 5, so equivalent_full_cycles += 0.5
//   Example 2: 90% -> 85% -> 40% (two flips):
//       First flip:  |delta| = 5, NOT > 5, no change.
//       Second flip: |delta| = 50, > 5, equivalent_full_cycles += 0.5.
//       Both "85 -> 90" and "85 -> 40" pieces together count as 0.5.
//
// Struct size: sizeof(BatteryState) ≈ 64 B per channel (16 channels * 64 ≈ 1 KB RAM).
//   Per-channel RAM cache: g_state[16] ≈ 1 KB.
//
// Behavior:
//   1. init_cycle_counter() zeroes g_state[*], g_loaded[*], g_last_dir[*]
//   2. update_cycle_counter() runs each 1s tick: integrate signed current,
//      recompute SoC, and on direction-flip check 5% hysteresis
//   3. Persist immediately on every flip and every 5 minutes (batched)
//   4. cycle_counter_get() returns the current g_state (loads from NVS on
//      first access if needed)
//   5. cycle_counter_reset() zeros the accumulator for a channel
//   6. cycle_counter_put() lets external modules (capacity_test) push updated
//      state back so the cache stays in sync with their writes

#include "cycle_counter.h"
#include "battery_profile.h"
#include "battery_state.h"
#include "settings_manager.h"   // settings_load_coulomb_mAh
#include "coulomb_counter.h"
#include <Arduino.h>
#include <string.h>

namespace {
constexpr uint32_t kPersistIntervalMs = 300000;  // 5 min
constexpr int8_t  kDirIdle = 0;
constexpr int8_t  kDirCharge = 1;
constexpr int8_t  kDirDischarge = -1;

BatteryState g_state[MAX_LOGICAL_CHANNELS];
bool g_loaded[MAX_LOGICAL_CHANNELS] = {false};
int8_t g_last_dir[MAX_LOGICAL_CHANNELS] = {0};  // 0 = unknown
uint32_t g_last_persist_ms = 0;
bool g_initialized = false;

float compute_soc_pct(uint8_t channel, const BatteryChemistryProfile* bp) {
    if (!bp || bp->rated_capacity_Ah <= 0.001f) return -1.0f;
    float net_mAh = get_coulomb_mAh(channel);
    float cap_mAh = bp->rated_capacity_Ah * 1000.0f;
    float soc = 100.0f + (net_mAh / cap_mAh) * 100.0f;
    if (soc < 0.0f) soc = 0.0f;
    if (soc > 100.0f) soc = 100.0f;
    return soc;
}

int8_t classify(float current_a) {
    if (current_a > CYCLE_CURRENT_DEADZONE_A) return kDirCharge;
    if (current_a < -CYCLE_CURRENT_DEADZONE_A) return kDirDischarge;
    return kDirIdle;
}
}  // namespace

void init_cycle_counter() {
    for (uint8_t i = 0; i < MAX_LOGICAL_CHANNELS; i++) {
        g_loaded[i] = false;
        g_state[i] = BatteryState{};
        g_last_dir[i] = 0;
    }
    g_last_persist_ms = millis();
    g_initialized = true;
}

static void persist_state(uint8_t channel) {
    battery_state_save(channel, &g_state[channel]);
}

void update_cycle_counter(const SensorSnapshot& snap, float dt_seconds) {
    if (!g_initialized) init_cycle_counter();

    uint32_t now = millis();

    for (uint8_t ch = 0; ch < MAX_LOGICAL_CHANNELS; ch++) {
        uint8_t pid = battery_channel_profile(ch);
        if (pid == BATTERY_CHANNEL_NO_BINDING) continue;
        const BatteryChemistryProfile* bp = battery_profile_get(pid);
        if (!bp) continue;

        if (!g_loaded[ch]) {
            if (!battery_state_load(ch, &g_state[ch])) {
                g_state[ch] = BatteryState{};
            }
            g_loaded[ch] = true;
            g_last_dir[ch] = 0;  // start with no prior direction
        }

        BatteryState& st = g_state[ch];
        float v = get_channel_voltage(snap, ch);
        float i = get_channel_current(snap, ch);
        st.last_V = v;
        st.last_I = i;
        st.last_update_ms = now;

        int8_t new_dir = classify(i);
        // Integrate Ah
        if (new_dir == kDirCharge) {
            float ah = i * dt_seconds / 3600.0f;
            st.cumulative_Ah_in += ah;
            st.current_session_dod_Ah += ah;
        } else if (new_dir == kDirDischarge) {
            float ah = -i * dt_seconds / 3600.0f;
            st.cumulative_Ah_out += ah;
            st.current_session_dod_Ah += ah;
        }

        // Update SoC
        float soc = compute_soc_pct(ch, bp);
        if (soc >= 0.0f) st.last_SoC_pct = soc;

        // Direction flip detection
        if (g_last_dir[ch] != 0 && new_dir != 0 && g_last_dir[ch] != new_dir) {
            float delta = st.last_SoC_pct - st.last_session_start_pct;
            if (delta < 0.0f) delta = -delta;
            if (delta > CYCLE_DOD_HYSTERESIS_PCT) {
                st.equivalent_full_cycles += delta / 100.0f;
                st.last_session_start_pct = st.last_SoC_pct;
                st.current_session_dod_Ah = 0.0f;
                persist_state(ch);
            }
        }
        if (new_dir != 0) g_last_dir[ch] = new_dir;
    }

    if (now - g_last_persist_ms >= kPersistIntervalMs) {
        for (uint8_t ch = 0; ch < MAX_LOGICAL_CHANNELS; ch++) {
            if (g_loaded[ch]) persist_state(ch);
        }
        g_last_persist_ms = now;
    }
}

void cycle_counter_get(uint8_t channel, BatteryState* out) {
    if (!out || channel >= MAX_LOGICAL_CHANNELS) return;
    if (!g_loaded[channel]) {
        if (!battery_state_load(channel, &g_state[channel])) {
            g_state[channel] = BatteryState{};
        }
        g_loaded[channel] = true;
    }
    *out = g_state[channel];
}

void cycle_counter_put(uint8_t channel, const BatteryState* in) {
    if (!in || channel >= MAX_LOGICAL_CHANNELS) return;
    g_state[channel] = *in;
    g_loaded[channel] = true;
    battery_state_save(channel, in);
}

void cycle_counter_reset(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return;
    if (g_loaded[channel]) {
        // Preserve test state? Simpler: zero everything for clarity.
        g_state[channel] = BatteryState{};
        persist_state(channel);
    } else {
        battery_state_reset(channel);
    }
}

// ── Per-field getters ────────────────────────────────────────────────────────
// All forwarders guarantee g_state[ch] is loaded before reading. We refuse
// channels with no profile binding — those rows are skipped in telemetry and
// callers should check cycle_counter_is_active() first.

static void ensure_loaded(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return;
    if (g_loaded[channel]) return;
    if (!battery_state_load(channel, &g_state[channel])) {
        g_state[channel] = BatteryState{};
    }
    g_loaded[channel] = true;
}

float cycle_counter_get_equivalent_full_cycles(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0.0f;
    ensure_loaded(channel);
    return g_state[channel].equivalent_full_cycles;
}

float cycle_counter_get_cumulative_Ah_in(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0.0f;
    ensure_loaded(channel);
    return g_state[channel].cumulative_Ah_in;
}

float cycle_counter_get_cumulative_Ah_out(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0.0f;
    ensure_loaded(channel);
    return g_state[channel].cumulative_Ah_out;
}

float cycle_counter_get_last_soc_pct(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return -1.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return -1.0f;
    ensure_loaded(channel);
    return g_state[channel].last_SoC_pct;
}

float cycle_counter_get_last_voltage(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0.0f;
    ensure_loaded(channel);
    return g_state[channel].last_V;
}

float cycle_counter_get_last_current(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0.0f;
    ensure_loaded(channel);
    return g_state[channel].last_I;
}

uint32_t cycle_counter_get_last_update_ms(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0;
    ensure_loaded(channel);
    return g_state[channel].last_update_ms;
}

bool cycle_counter_is_active(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return false;
    return battery_channel_profile(channel) != BATTERY_CHANNEL_NO_BINDING;
}
