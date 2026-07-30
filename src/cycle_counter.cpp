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
// Struct size: sizeof(BatteryState) per channel (16 channels ≈ 0.9 KB RAM).
//
// Behavior:
//   1. init_cycle_counter() zeroes g_state[*], g_loaded[*], g_last_dir[*]
//   2. update_cycle_counter() runs each 1s tick: integrate signed current,
//      recompute SoC, and on direction-flip check 5% hysteresis
//   3. Persist immediately on every flip and every 5 minutes (batched)
//   4. cycle_counter_get() / cycle_counter_snapshot() return the current
//      g_state (loads from NVS on first access if needed)
//   5. cycle_counter_reset() zeros the accumulator for a channel
//   6. cycle_counter_put() lets external modules (capacity_test) push updated
//      state back so the cache stays in sync with their writes
//
// Concurrency: g_state[], g_loaded[], and g_last_dir[] are touched by
// sensorTask (update_cycle_counter at 1Hz) and read by networkTask
// (telemetry_build at 5s, BLE get_status). All accesses are serialised by
// the g_battery_mux critical section. The lock window is kept tight: NVS
// load/save happens OUTSIDE the lock to avoid holding the spinlock across
// flash I/O.

#include "cycle_counter.h"
#include "battery_lock.h"
#include "battery_nvs.h"
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

// SoH smoothing factor (EWMA). Higher = slower adaptation.
// soh_pct = SOH_ALPHA * soh_pct + (1 - SOH_ALPHA) * leg_soh
constexpr float kSohAlpha = 0.7f;

BatteryState g_state[MAX_LOGICAL_CHANNELS];
bool g_loaded[MAX_LOGICAL_CHANNELS] = {false};
int8_t g_last_dir[MAX_LOGICAL_CHANNELS] = {0};  // 0 = unknown
uint32_t g_last_persist_ms = 0;
bool g_initialized = false;

float compute_soc_pct(uint8_t channel, const BatteryChemistryProfile* bp) {
    if (!bp || bp->rated_capacity_Ah <= 0.001f) return -1.0f;
    float net_mAh = get_coulomb_mAh(channel);
    float cap_mAh = bp->rated_capacity_Ah * 1000.0f;
    // Base SoC is the operator-set initial_soc_pct (BatteryConfig), defaulting
    // to 100% when unset. Previously this was hardcoded to 100%, so a battery
    // installed at 50% read 100% for its whole life until a full charge reset
    // the reference. net_mAh is accumulated charge since the last reset, so
    // soc = base + (net/cap)*100.
    float base_soc = 100.0f;
    BatteryConfig bat;
    if (settings_load_battery(channel, &bat) && bat.initial_soc_pct >= 0.0f && bat.initial_soc_pct <= 100.0f) {
        base_soc = bat.initial_soc_pct;
    }
    float soc = base_soc + (net_mAh / cap_mAh) * 100.0f;
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

        bool crossed_hysteresis = false;
        BATTERY_LOCK();
        if (!g_loaded[ch]) {
            // Drop the lock across the NVS load so a slow flash read doesn't
            // stall other tasks. After re-acquiring the lock we trust the
            // cache for the rest of this tick.
            BATTERY_UNLOCK();
            BatteryState loaded{};
            if (!battery_state_load(ch, &loaded)) {
                loaded = BatteryState{};
            }
            BATTERY_LOCK();
            g_state[ch] = loaded;
            g_loaded[ch] = true;
            g_last_dir[ch] = 0;
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
        } else if (new_dir == kDirDischarge) {
            float ah = -i * dt_seconds / 3600.0f;
            st.cumulative_Ah_out += ah;
        }

        // Update SoC
        float soc = compute_soc_pct(ch, bp);
        if (soc >= 0.0f) st.last_SoC_pct = soc;

        // First directional tick for this channel: seed last_session_start_pct
        // with the post-tick SoC so the first flip measures from the new
        // session start, not from a stale initial value.
        if (g_last_dir[ch] == 0 && new_dir != 0) {
            st.last_session_start_pct = st.last_SoC_pct;
        }

        // Direction flip detection
        if (g_last_dir[ch] != 0 && new_dir != 0 && g_last_dir[ch] != new_dir) {
            float delta = st.last_SoC_pct - st.last_session_start_pct;
            if (delta < 0.0f) delta = -delta;
            if (delta > CYCLE_DOD_HYSTERESIS_PCT) {
                st.equivalent_full_cycles += delta / 100.0f;
                crossed_hysteresis = true;
                // Continuous SoH: compute the leg's discharge Ah from the
                // SoC span and rated capacity, then update the EWMA estimate.
                // leg_Ah = delta_pct / 100 * rated_capacity_Ah
                if (bp && bp->rated_capacity_Ah > 0.001f) {
                    float leg_Ah = (delta / 100.0f) * bp->rated_capacity_Ah;
                    st.last_full_discharge_Ah = leg_Ah;
                    float leg_soh = (leg_Ah / bp->rated_capacity_Ah) * 100.0f;
                    if (leg_soh > 100.0f) leg_soh = 100.0f;
                    if (st.soh_samples == 0) {
                        st.soh_pct = leg_soh;
                    } else {
                        st.soh_pct = kSohAlpha * st.soh_pct + (1.0f - kSohAlpha) * leg_soh;
                    }
                    if (st.soh_pct < 0.0f) st.soh_pct = 0.0f;
                    if (st.soh_pct > 100.0f) st.soh_pct = 100.0f;
                    st.soh_samples++;
                }
            }
            // Whether or not the delta cleared the hysteresis, the new
            // session starts here. The sub-5% portion is absorbed: it
            // contributes 0 cycles now and is included in the next leg's
            // measurement.
            st.last_session_start_pct = st.last_SoC_pct;
        }
        if (new_dir != 0) g_last_dir[ch] = new_dir;
        BATTERY_UNLOCK();

        if (crossed_hysteresis) persist_state(ch);
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
    BATTERY_LOCK();
    if (!g_loaded[channel]) {
        BATTERY_UNLOCK();
        BatteryState loaded{};
        if (!battery_state_load(channel, &loaded)) {
            loaded = BatteryState{};
        }
        BATTERY_LOCK();
        g_state[channel] = loaded;
        g_loaded[channel] = true;
        g_last_dir[channel] = 0;
    }
    *out = g_state[channel];
    BATTERY_UNLOCK();
}

// Point-in-time snapshot under a single lock. Preferred over
// cycle_counter_get() for telemetry consumers that want a consistent
// view across all the BatteryState fields.
void cycle_counter_snapshot(uint8_t channel, BatteryState* out) {
    cycle_counter_get(channel, out);
}

void cycle_counter_put(uint8_t channel, const BatteryState* in) {
    if (!in || channel >= MAX_LOGICAL_CHANNELS) return;
    BATTERY_LOCK();
    g_state[channel] = *in;
    g_loaded[channel] = true;
    BATTERY_UNLOCK();
    battery_state_save(channel, in);
}

void cycle_counter_reset(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return;
    BATTERY_LOCK();
    bool was_loaded = g_loaded[channel];
    g_state[channel] = BatteryState{};
    g_last_dir[channel] = 0;  // forget prior direction so the next
                              // tick re-seeds last_session_start_pct
    g_loaded[channel] = true;
    BATTERY_UNLOCK();
    if (was_loaded) {
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
    BATTERY_LOCK();
    bool loaded = g_loaded[channel];
    BATTERY_UNLOCK();
    if (loaded) return;

    BatteryState loaded_state{};
    if (!battery_state_load(channel, &loaded_state)) {
        loaded_state = BatteryState{};
    }
    BATTERY_LOCK();
    g_state[channel] = loaded_state;
    g_loaded[channel] = true;
    g_last_dir[channel] = 0;
    BATTERY_UNLOCK();
}

float cycle_counter_get_equivalent_full_cycles(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0.0f;
    ensure_loaded(channel);
    BATTERY_LOCK();
    float v = g_state[channel].equivalent_full_cycles;
    BATTERY_UNLOCK();
    return v;
}

float cycle_counter_get_cumulative_Ah_in(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0.0f;
    ensure_loaded(channel);
    BATTERY_LOCK();
    float v = g_state[channel].cumulative_Ah_in;
    BATTERY_UNLOCK();
    return v;
}

float cycle_counter_get_cumulative_Ah_out(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0.0f;
    ensure_loaded(channel);
    BATTERY_LOCK();
    float v = g_state[channel].cumulative_Ah_out;
    BATTERY_UNLOCK();
    return v;
}

float cycle_counter_get_last_soc_pct(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return -1.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return -1.0f;
    ensure_loaded(channel);
    BATTERY_LOCK();
    float v = g_state[channel].last_SoC_pct;
    BATTERY_UNLOCK();
    return v;
}

float cycle_counter_get_last_voltage(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0.0f;
    ensure_loaded(channel);
    BATTERY_LOCK();
    float v = g_state[channel].last_V;
    BATTERY_UNLOCK();
    return v;
}

float cycle_counter_get_last_current(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0.0f;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0.0f;
    ensure_loaded(channel);
    BATTERY_LOCK();
    float v = g_state[channel].last_I;
    BATTERY_UNLOCK();
    return v;
}

uint32_t cycle_counter_get_last_update_ms(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0;
    if (battery_channel_profile(channel) == BATTERY_CHANNEL_NO_BINDING) return 0;
    ensure_loaded(channel);
    BATTERY_LOCK();
    uint32_t v = g_state[channel].last_update_ms;
    BATTERY_UNLOCK();
    return v;
}

bool cycle_counter_is_active(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return false;
    return battery_channel_profile(channel) != BATTERY_CHANNEL_NO_BINDING;
}
