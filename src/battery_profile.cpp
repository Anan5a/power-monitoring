// BatteryChemistryProfile registry
//
// Size summary:
//   BatteryChemistryProfile  : 64 bytes (see battery_profile.h)
//   One NVS blob             : 1 (version) + 16 * 64 = 1025 bytes
//
// Behavior:
//   1. init_battery_profiles() loads built-in defaults (slots 0..3) and
//      overlays any persisted overrides from the "pm-battery" NVS namespace
//      (key "bat_profiles_v1", blob = [version][16 profiles])
//   2. battery_profile_get(id) returns a const pointer or nullptr for invalid
//      id or empty slot
//   3. battery_profile_set() writes the profile, grows g_populated if needed,
//      persists the whole blob
//   4. battery_profile_delete() refuses built-ins (id 0..3) and clears a
//      custom slot
//   5. battery_profile_reset_builtin() restores a built-in factory default
//   6. battery_profile_list_ids() returns the populated ids in order

#include "battery_profile.h"
#include "settings_manager.h"
#include <Preferences.h>
#include <string.h>

namespace {
constexpr const char* kPrefsNs = "pm-battery";
constexpr const char* kProfilesKey = "bat_profiles_v1";
constexpr uint8_t kProfileBlobVersion = 1;

BatteryChemistryProfile g_profiles[BATTERY_MAX_PROFILES];
uint8_t g_populated = BATTERY_BUILTIN_PROFILE_COUNT;
bool g_loaded = false;

Preferences prefs;

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
    if (g_loaded) return;
    for (uint8_t i = 0; i < BATTERY_BUILTIN_PROFILE_COUNT; i++) {
        g_profiles[i] = kBuiltins[i];
    }
    for (uint8_t i = BATTERY_BUILTIN_PROFILE_COUNT; i < BATTERY_MAX_PROFILES; i++) {
        memset(&g_profiles[i], 0, sizeof(BatteryChemistryProfile));
    }
    g_populated = BATTERY_BUILTIN_PROFILE_COUNT;

    if (prefs.begin((char*)kPrefsNs, false)) {
        size_t len = prefs.getBytesLength(kProfilesKey);
        if (len == 1 + BATTERY_MAX_PROFILES * sizeof(BatteryChemistryProfile)) {
            uint8_t version = prefs.getUChar("bat_profiles_ver", 0);
            if (version == kProfileBlobVersion) {
                uint8_t raw[1 + BATTERY_MAX_PROFILES * sizeof(BatteryChemistryProfile)];
                prefs.getBytes(kProfilesKey, raw, sizeof(raw));
                const BatteryChemistryProfile* p =
                    reinterpret_cast<const BatteryChemistryProfile*>(&raw[1]);
                uint8_t new_pop = BATTERY_BUILTIN_PROFILE_COUNT;
                for (uint8_t i = 0; i < BATTERY_MAX_PROFILES; i++) {
                    if (p[i].id == i && p[i].name[0] != 0 && p[i].rated_capacity_Ah > 0.001f) {
                        g_profiles[i] = p[i];
                        if (i >= BATTERY_BUILTIN_PROFILE_COUNT && i + 1 > new_pop) {
                            new_pop = i + 1;
                        }
                    }
                }
                g_populated = new_pop;
            }
        }
        prefs.end();
    }
    g_loaded = true;
}

const BatteryChemistryProfile* battery_profile_get(uint8_t id) {
    if (id >= BATTERY_MAX_PROFILES) return nullptr;
    if (id >= g_populated) return nullptr;
    return &g_profiles[id];
}

uint8_t battery_profile_count() {
    return g_populated;
}

uint8_t battery_profile_list_ids(uint8_t* out_ids, uint8_t max) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < g_populated && n < max; i++) {
        out_ids[n++] = i;
    }
    return n;
}

static bool persist_profiles() {
    if (!prefs.begin((char*)kPrefsNs, false)) return false;
    uint8_t raw[1 + BATTERY_MAX_PROFILES * sizeof(BatteryChemistryProfile)];
    raw[0] = kProfileBlobVersion;
    memcpy(&raw[1], g_profiles, sizeof(g_profiles));
    bool ok = prefs.putBytes(kProfilesKey, raw, sizeof(raw)) == sizeof(raw);
    if (ok) prefs.putUChar("bat_profiles_ver", kProfileBlobVersion);
    prefs.end();
    return ok;
}

bool battery_profile_set(const BatteryChemistryProfile* profile) {
    if (!profile) return false;
    if (profile->id >= BATTERY_MAX_PROFILES) return false;
    g_profiles[profile->id] = *profile;
    if (profile->id + 1 > g_populated) g_populated = profile->id + 1;
    return persist_profiles();
}

bool battery_profile_delete(uint8_t id) {
    if (id < BATTERY_BUILTIN_PROFILE_COUNT) return false;
    if (id >= BATTERY_MAX_PROFILES) return false;
    if (id >= g_populated) return false;
    memset(&g_profiles[id], 0, sizeof(BatteryChemistryProfile));
    if (id + 1 == g_populated) {
        while (g_populated > BATTERY_BUILTIN_PROFILE_COUNT) {
            const BatteryChemistryProfile* p = &g_profiles[g_populated - 1];
            if (p->id == g_populated - 1 && p->name[0] != 0 && p->rated_capacity_Ah > 0.001f) break;
            g_populated--;
        }
    }
    return persist_profiles();
}

void battery_profile_reset_builtin(uint8_t id) {
    if (id >= BATTERY_BUILTIN_PROFILE_COUNT) return;
    g_profiles[id] = kBuiltins[id];
    persist_profiles();
}
