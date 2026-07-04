// BatteryChemistryProfile registry
//
// Size summary:
//   BatteryChemistryProfile  : 56 bytes (static_asserted in battery_profile.h)
//   One NVS blob             : 1 (version) + 16 * 56 = 897 bytes
//
// Behavior:
//   1. init_battery_profiles() loads built-in defaults (slots 0..3) and
//      overlays any persisted overrides from the "pm-battery-profile" NVS
//      namespace (key "bat_profiles_v1", blob = [version][16 profiles])
//   2. battery_profile_get(id) returns a const pointer or nullptr for invalid
//      id, sparse slot (empty name or near-zero capacity), or out-of-range
//   3. battery_profile_set() validates the profile (non-empty name, capacity
//      > 0.001 Ah, id < BATTERY_MAX_PROFILES) before writing; rejects bad
//      input with a debug log
//   4. battery_profile_delete() refuses built-ins (id 0..3) and clears a
//      custom slot
//   5. battery_profile_reset_builtin() restores a built-in factory default
//   6. battery_profile_list_ids() returns the populated ids in order
//
// All NVS and in-memory access is guarded by the g_battery_mux critical
// section (see battery_lock.h). Read paths (telemetry, BLE) take the lock
// once per call; the lock window covers both NVS and the g_profiles[] read
// but stays short.

#include "battery_profile.h"
#include "battery_lock.h"
#include "battery_nvs.h"
#include "log_serial.h"
#include <Arduino.h>
#include <string.h>

namespace {
constexpr const char* kProfilesKey    = "bat_profiles_v1";
constexpr const char* kProfilesVerKey = "bat_profiles_ver";
constexpr uint8_t     kProfileBlobVersion = 1;

BatteryChemistryProfile g_profiles[BATTERY_MAX_PROFILES];
uint8_t g_populated = BATTERY_BUILTIN_PROFILE_COUNT;

static const BatteryChemistryProfile kBuiltins[BATTERY_BUILTIN_PROFILE_COUNT] = {
    // 0: 12V Lead-Acid (flooded)
    { 0, "12V Lead-Acid", BAT_CHEM_LEAD_ACID, 12.0f, 100.0f, 0.2f, 10.5f, 13.8f, 0.85f, 500, 20.0f, 100.0f },
    // 1: Li-ion (NMC)
    { 1, "Li-ion 3.7V",   BAT_CHEM_LIION,      3.7f,   2.5f, 1.0f,  3.0f,  4.2f, 0.95f, 500, 10.0f,  95.0f },
    // 2: LiFePO4 (LFP)
    { 2, "LiFePO4 3.2V",  BAT_CHEM_LFP,        3.2f, 100.0f, 1.0f,  2.5f,  3.65f, 0.95f, 2000, 10.0f, 100.0f },
    // 3: LiPo
    { 3, "LiPo 3.7V",     BAT_CHEM_LIPO,       3.7f,   2.2f, 1.0f,  3.0f,  4.2f, 0.94f, 300, 20.0f,  95.0f },
};

// Test whether a profile slot is "populated" (has a valid name and
// non-trivial capacity). Used for both the persisted-blob validation
// pass and the runtime battery_profile_get() sparse-slot guard.
static inline bool profile_slot_valid(const BatteryChemistryProfile& p) {
    return p.name[0] != 0 && p.rated_capacity_Ah > 0.001f;
}

static bool persist_profiles_locked() {
    uint8_t raw[1 + BATTERY_MAX_PROFILES * sizeof(BatteryChemistryProfile)];
    raw[0] = kProfileBlobVersion;
    memcpy(&raw[1], g_profiles, sizeof(g_profiles));
    bool ok = battery_nvs_profile_put_locked(kProfilesKey, raw, sizeof(raw));
    if (ok) battery_nvs_profile_put_u8_locked(kProfilesVerKey, kProfileBlobVersion);
    return ok;
}
}  // namespace

const char* battery_chemistry_name(uint8_t chemistry) {
    switch (chemistry) {
        case BAT_CHEM_LEAD_ACID: return "lead_acid";
        case BAT_CHEM_LIION:     return "liion";
        case BAT_CHEM_LFP:       return "lfp";
        case BAT_CHEM_LIPO:      return "lipo";
        case BAT_CHEM_NICD:      return "nicd";
        case BAT_CHEM_NIMH:      return "nimh";
        case BAT_CHEM_CUSTOM:    return "custom";
        default:                 return "lead_acid";
    }
}

const BatteryChemistryProfile* battery_profile_builtin_defaults() {
    return kBuiltins;
}

void init_battery_profiles() {
    BATTERY_LOCK();
    for (uint8_t i = 0; i < BATTERY_BUILTIN_PROFILE_COUNT; i++) {
        g_profiles[i] = kBuiltins[i];
    }
    for (uint8_t i = BATTERY_BUILTIN_PROFILE_COUNT; i < BATTERY_MAX_PROFILES; i++) {
        memset(&g_profiles[i], 0, sizeof(BatteryChemistryProfile));
    }
    g_populated = BATTERY_BUILTIN_PROFILE_COUNT;

    uint8_t version = 0;
    if (battery_nvs_profile_get_u8_locked(kProfilesVerKey, &version) && version == kProfileBlobVersion) {
        uint8_t raw[1 + BATTERY_MAX_PROFILES * sizeof(BatteryChemistryProfile)];
        size_t len = 0;
        if (battery_nvs_profile_get_locked(kProfilesKey, raw, sizeof(raw), &len) && len == sizeof(raw)) {
            const BatteryChemistryProfile* p =
                reinterpret_cast<const BatteryChemistryProfile*>(&raw[1]);
            uint8_t new_pop = BATTERY_BUILTIN_PROFILE_COUNT;
            for (uint8_t i = 0; i < BATTERY_MAX_PROFILES; i++) {
                // A persisted slot is only honoured if it carries its own id
                // and a non-sparse body (name + capacity).
                if (p[i].id == i && profile_slot_valid(p[i])) {
                    g_profiles[i] = p[i];
                    if (i >= BATTERY_BUILTIN_PROFILE_COUNT && i + 1 > new_pop) {
                        new_pop = i + 1;
                    }
                }
            }
            g_populated = new_pop;
        }
    }
    BATTERY_UNLOCK();
}

const BatteryChemistryProfile* battery_profile_get(uint8_t id) {
    if (id >= BATTERY_MAX_PROFILES) return nullptr;
    BATTERY_LOCK();
    const BatteryChemistryProfile* out = nullptr;
    if (id < g_populated) {
        // Sparse-slot guard: the array might contain a zero-initialised
        // entry that survived a delete. Treat it as "not present" so
        // callers don't accidentally read uninitialised fields.
        if (profile_slot_valid(g_profiles[id])) {
            out = &g_profiles[id];
        }
    }
    BATTERY_UNLOCK();
    return out;
}

uint8_t battery_profile_count() {
    BATTERY_LOCK();
    uint8_t n = g_populated;
    BATTERY_UNLOCK();
    return n;
}

uint8_t battery_profile_list_ids(uint8_t* out_ids, uint8_t max) {
    if (!out_ids) return 0;
    uint8_t n = 0;
    BATTERY_LOCK();
    for (uint8_t i = 0; i < g_populated && n < max; i++) {
        out_ids[n++] = i;
    }
    BATTERY_UNLOCK();
    return n;
}

bool battery_profile_set(const BatteryChemistryProfile* profile) {
    if (!profile) {
        LOG_PRINTLN("[battery_profile_set] rejected: null profile");
        return false;
    }
    if (profile->id >= BATTERY_MAX_PROFILES) {
        LOG_PRINT("[battery_profile_set] rejected: id=%u out of range\n",
                  (unsigned)profile->id);
        return false;
    }
    // Reject obviously bad profiles so we never persist empty or
    // zero-capacity rows that would later show up as "0 Ah" SoH results.
    if (profile->name[0] == 0) {
        LOG_PRINTLN("[battery_profile_set] rejected: empty name");
        return false;
    }
    if (profile->rated_capacity_Ah <= 0.001f) {
        LOG_PRINT("[battery_profile_set] rejected: capacity=%.4f Ah\n",
                  (double)profile->rated_capacity_Ah);
        return false;
    }
    BATTERY_LOCK();
    g_profiles[profile->id] = *profile;
    if (profile->id + 1 > g_populated) g_populated = profile->id + 1;
    bool ok = persist_profiles_locked();
    BATTERY_UNLOCK();
    return ok;
}

bool battery_profile_delete(uint8_t id) {
    if (id < BATTERY_BUILTIN_PROFILE_COUNT) return false;
    if (id >= BATTERY_MAX_PROFILES) return false;
    BATTERY_LOCK();
    if (id >= g_populated) { BATTERY_UNLOCK(); return false; }
    memset(&g_profiles[id], 0, sizeof(BatteryChemistryProfile));
    if (id + 1 == g_populated) {
        while (g_populated > BATTERY_BUILTIN_PROFILE_COUNT) {
            const BatteryChemistryProfile* p = &g_profiles[g_populated - 1];
            if (profile_slot_valid(*p)) break;
            g_populated--;
        }
    }
    bool ok = persist_profiles_locked();
    BATTERY_UNLOCK();
    return ok;
}

void battery_profile_reset_builtin(uint8_t id) {
    if (id >= BATTERY_BUILTIN_PROFILE_COUNT) return;
    BATTERY_LOCK();
    g_profiles[id] = kBuiltins[id];
    bool ok = persist_profiles_locked();
    BATTERY_UNLOCK();
    (void)ok;
}
