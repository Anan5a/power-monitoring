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

// BatteryState blob version. v1 blobs had current_session_dod_Ah; v2 drops
// that field. On load we reject any blob whose leading version byte doesn't
// match kStateBlobVersion. The caller (cycle_counter) treats
// `battery_state_load` returning false as "fresh state" and re-initialises
// g_state[ch] = {} — no migration of v1 data is performed.
constexpr uint8_t kStateBlobVersion = 2;

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
    // Layout: [version=1][BatteryState body]
    uint8_t buf[1 + sizeof(BatteryState)];
    size_t len = 0;
    if (!battery_nvs_state_get(key, buf, sizeof(buf), &len)) return false;
    if (len != sizeof(buf)) return false;
    if (buf[0] != kStateBlobVersion) {
        LOG_PRINTLN("[battery_state] v? rejected");
        return false;
    }
    memcpy(out, &buf[1], sizeof(BatteryState));
    return true;
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
