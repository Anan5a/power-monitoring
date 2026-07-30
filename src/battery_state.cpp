// Per-channel binding + state persisted to NVS namespaces:
//   "pm-battery-state"  (via battery_nvs.cpp):
//     "bat_ch_bind_v1"  : 16-byte array (one profile id per logical channel)
//     "bat_state_v2"    : per-channel blob keyed "ch0".."ch15", prefixed
//                         with a 1-byte version (=kStateBlobVersion)
//
// All read/write paths go through battery_nvs_* helpers. The shared
// g_battery_mux critical section (battery_lock.h) serialises concurrent
// access from sensorTask (writes at 1Hz) and networkTask (reads at 5s).
// In-process access to g_channel_profile[] is also guarded by the same
// lock so a network-task read of `battery_channel_profile(ch)` cannot
// observe a half-updated byte.

#include "battery_state.h"
#include "battery_lock.h"
#include "battery_nvs.h"
#include "log_serial.h"
#include <Arduino.h>
#include <string.h>

namespace {
constexpr const char* kBindKey  = "bat_ch_bind_v1";
constexpr const char* kBindVer  = "bat_ch_bind_ver";
constexpr uint8_t     kBindVersion = 1;

// BatteryState blob version. v1 had current_session_dod_Ah; v2 dropped it;
// v3 removes CapacityTestState and adds continuous SoH fields
// (soh_pct, soh_samples, last_full_discharge_Ah). On load we reject blobs
// whose version is > current (shouldn't happen). v2 blobs are migrated:
// surviving fields are copied and SoH is seeded at 100%.
constexpr uint8_t kStateBlobVersion = 3;

uint8_t g_channel_profile[MAX_LOGICAL_CHANNELS];

bool persist_bindings_locked() {
    return battery_nvs_state_put_locked(kBindKey, g_channel_profile, sizeof(g_channel_profile));
}

bool persist_bindings() {
    bool ok = persist_bindings_locked();
    if (ok) battery_nvs_state_put_u8(kBindVer, kBindVersion);
    return ok;
}

void make_state_key(uint8_t channel, char* out, size_t out_len) {
    snprintf(out, out_len, "ch%u", (unsigned)channel);
}
}  // namespace

void init_battery_bindings() {
    for (uint8_t i = 0; i < MAX_LOGICAL_CHANNELS; i++) g_channel_profile[i] = BATTERY_CHANNEL_NO_BINDING;

    uint8_t ver = 0;
    if (battery_nvs_state_get_u8(kBindVer, &ver) && ver == kBindVersion) {
        battery_nvs_state_get(kBindKey, g_channel_profile, sizeof(g_channel_profile), nullptr);
    }
}

uint8_t battery_channel_profile(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return BATTERY_CHANNEL_NO_BINDING;
    BATTERY_LOCK();
    uint8_t v = g_channel_profile[channel];
    BATTERY_UNLOCK();
    return v;
}

// Reject 0xFF — callers wanting to clear a binding must use
// battery_channel_clear(). Allowing 0xFF through set_profile() was a
// symmetry-bug: callers could bind a channel to "no profile" without going
// through the explicit clear path, hiding the intent at the call site.
bool battery_channel_set_profile(uint8_t channel, uint8_t profile_id) {
    if (channel >= MAX_LOGICAL_CHANNELS) return false;
    if (profile_id == BATTERY_CHANNEL_NO_BINDING) return false;
    if (profile_id >= BATTERY_MAX_PROFILES) return false;
    BATTERY_LOCK();
    g_channel_profile[channel] = profile_id;
    bool ok = persist_bindings_locked();
    BATTERY_UNLOCK();
    if (ok) battery_nvs_state_put_u8(kBindVer, kBindVersion);
    return ok;
}

void battery_channel_clear(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return;
    BATTERY_LOCK();
    g_channel_profile[channel] = BATTERY_CHANNEL_NO_BINDING;
    bool ok = persist_bindings_locked();
    BATTERY_UNLOCK();
    if (ok) battery_nvs_state_put_u8(kBindVer, kBindVersion);
}

void init_battery_states() {
    // No-op: state is loaded on-demand by battery_state_load().
}

bool battery_state_load(uint8_t channel, BatteryState* out) {
    if (!out || channel >= MAX_LOGICAL_CHANNELS) return false;
    char key[8];
    make_state_key(channel, key, sizeof(key));
    // Layout: [version][BatteryState body]
    uint8_t buf[1 + sizeof(BatteryState)];
    size_t len = 0;
    if (!battery_nvs_state_get(key, buf, sizeof(buf), &len)) return false;
    if (len < 1) return false;
    uint8_t ver = buf[0];
    if (ver > kStateBlobVersion) {
        LOG_PRINT("[battery_state] v%u > current %u — rejected\n", (unsigned)ver, (unsigned)kStateBlobVersion);
        return false;
    }
    if (ver == kStateBlobVersion) {
        // Current version: direct copy.
        if (len != sizeof(buf)) return false;
        memcpy(out, &buf[1], sizeof(BatteryState));
        return true;
    }
    // v2 → v3 migration: copy surviving fields, seed SoH at 100%.
    // v2 BatteryState layout (68 bytes):
    //   float cumulative_Ah_in (4)
    //   float cumulative_Ah_out (4)
    //   float equivalent_full_cycles (4)
    //   float last_SoC_pct (4)
    //   float last_V (4)
    //   float last_I (4)
    //   uint32_t last_update_ms (4)
    //   float last_session_start_pct (4)
    //   CapacityTestState test (32 bytes, to be dropped)
    // v3 BatteryState layout (48 bytes):
    //   same first 8 fields (32 bytes)
    //   float soh_pct (4)
    //   uint32_t soh_samples (4)
    //   float last_full_discharge_Ah (4)
    //   padding (4 bytes)
    if (ver == 2) {
        if (len < 1 + 32) return false;  // at least the first 8 fields
        // Read the v2 fields at known offsets
        struct V2Layout {
            float cumulative_Ah_in;
            float cumulative_Ah_out;
            float equivalent_full_cycles;
            float last_SoC_pct;
            float last_V;
            float last_I;
            uint32_t last_update_ms;
            float last_session_start_pct;
            // CapacityTestState follows (32 bytes), ignored
        } v2;
        memcpy(&v2, &buf[1], sizeof(v2));
        BatteryState s = {};
        s.cumulative_Ah_in = v2.cumulative_Ah_in;
        s.cumulative_Ah_out = v2.cumulative_Ah_out;
        s.equivalent_full_cycles = v2.equivalent_full_cycles;
        s.last_SoC_pct = v2.last_SoC_pct;
        s.last_V = v2.last_V;
        s.last_I = v2.last_I;
        s.last_update_ms = v2.last_update_ms;
        s.last_session_start_pct = v2.last_session_start_pct;
        s.soh_pct = 100.0f;  // seed at 100% on migration
        s.soh_samples = 0;
        s.last_full_discharge_Ah = 0.0f;
        *out = s;
        LOG_PRINT("[battery_state] ch%u migrated v2→v3\n", (unsigned)channel);
        return true;
    }
    // v1 or unknown: reject (caller re-inits to {})
    LOG_PRINT("[battery_state] v%u unknown — rejected\n", (unsigned)ver);
    return false;
}

bool battery_state_save(uint8_t channel, const BatteryState* in) {
    if (!in || channel >= MAX_LOGICAL_CHANNELS) return false;
    char key[8];
    make_state_key(channel, key, sizeof(key));
    uint8_t buf[1 + sizeof(BatteryState)];
    buf[0] = kStateBlobVersion;
    memcpy(&buf[1], in, sizeof(BatteryState));
    return battery_nvs_state_put(key, buf, sizeof(buf));
}

void battery_state_reset(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return;
    BatteryState empty = {};
    battery_state_save(channel, &empty);
    char key[8];
    make_state_key(channel, key, sizeof(key));
    battery_nvs_state_remove(key);
}
