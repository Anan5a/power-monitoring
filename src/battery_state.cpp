// Per-channel binding + state persisted to NVS namespace "pm-battery":
//   "bat_ch_bind_v1"  : 16-byte array (one profile id per logical channel)
//   "bat_state_v1"    : per-channel blob keyed "ch0".."ch15" (sizeof(BatteryState))

#include "battery_state.h"
#include <Preferences.h>
#include <string.h>

namespace {
constexpr const char* kPrefsNs = "pm-battery";
constexpr const char* kBindKey  = "bat_ch_bind_v1";
constexpr const char* kBindVer  = "bat_ch_bind_ver";
constexpr uint8_t kBindVersion = 1;

Preferences prefs;

uint8_t g_channel_profile[MAX_LOGICAL_CHANNELS];
}  // namespace

static bool persist_bindings() {
    if (!prefs.begin((char*)kPrefsNs, false)) return false;
    bool ok = prefs.putBytes(kBindKey, g_channel_profile, sizeof(g_channel_profile)) == sizeof(g_channel_profile);
    if (ok) prefs.putUChar(kBindVer, kBindVersion);
    prefs.end();
    return ok;
}

void init_battery_bindings() {
    // Default: no binding on any channel
    for (uint8_t i = 0; i < MAX_LOGICAL_CHANNELS; i++) g_channel_profile[i] = BATTERY_CHANNEL_NO_BINDING;

    if (prefs.begin((char*)kPrefsNs, false)) {
        uint8_t version = prefs.getUChar(kBindVer, 0);
        if (version == kBindVersion) {
            size_t len = prefs.getBytesLength(kBindKey);
            if (len == sizeof(g_channel_profile)) {
                prefs.getBytes(kBindKey, g_channel_profile, sizeof(g_channel_profile));
            }
        }
        prefs.end();
    }
}

uint8_t battery_channel_profile(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return BATTERY_CHANNEL_NO_BINDING;
    return g_channel_profile[channel];
}

bool battery_channel_set_profile(uint8_t channel, uint8_t profile_id) {
    if (channel >= MAX_LOGICAL_CHANNELS) return false;
    if (profile_id != BATTERY_CHANNEL_NO_BINDING && profile_id >= BATTERY_MAX_PROFILES) return false;
    g_channel_profile[channel] = profile_id;
    return persist_bindings();
}

void battery_channel_clear(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return;
    g_channel_profile[channel] = BATTERY_CHANNEL_NO_BINDING;
    persist_bindings();
}

void init_battery_states() {
    // No-op: state is loaded on-demand by battery_state_load().
    // This function exists so callers can hook into a single init step.
}

static void make_state_key(uint8_t channel, char* out, size_t out_len) {
    snprintf(out, out_len, "ch%u", (unsigned)channel);
}

bool battery_state_load(uint8_t channel, BatteryState* out) {
    if (!out || channel >= MAX_LOGICAL_CHANNELS) return false;
    if (!prefs.begin((char*)kPrefsNs, false)) return false;
    char key[8];
    make_state_key(channel, key, sizeof(key));
    bool ok = false;
    if (prefs.isKey(key) && prefs.getBytesLength(key) == sizeof(BatteryState)) {
        prefs.getBytes(key, out, sizeof(BatteryState));
        ok = true;
    }
    prefs.end();
    return ok;
}

bool battery_state_save(uint8_t channel, const BatteryState* in) {
    if (!in || channel >= MAX_LOGICAL_CHANNELS) return false;
    if (!prefs.begin((char*)kPrefsNs, false)) return false;
    char key[8];
    make_state_key(channel, key, sizeof(key));
    bool ok = prefs.putBytes(key, in, sizeof(BatteryState)) == sizeof(BatteryState);
    prefs.end();
    return ok;
}

void battery_state_reset(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return;
    BatteryState empty = {};
    battery_state_save(channel, &empty);
    // Also clear NVS key for clarity
    if (prefs.begin((char*)kPrefsNs, false)) {
        char key[8];
        make_state_key(channel, key, sizeof(key));
        prefs.remove(key);
        prefs.end();
    }
}
