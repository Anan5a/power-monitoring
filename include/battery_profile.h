#ifndef BATTERY_PROFILE_H
#define BATTERY_PROFILE_H

#include <stdint.h>
#include <stdbool.h>

// ── Size summary (verified — see static_assert below) ────────────────────────
// BatteryChemistryProfile  : 56 bytes
//   Layout (4-byte aligned):
//     +0   uint8_t  id
//     +1   char[16] name
//     +17  uint8_t  chemistry
//     +18  pad[2]   (for float alignment)
//     +20  float    nominal_voltage
//     +24  float    rated_capacity_Ah
//     +28  float    c_rating
//     +32  float    cutoff_voltage
//     +36  float    float_voltage
//     +40  float    charge_efficiency
//     +44  uint16_t cycle_life_rated
//     +46  pad[2]   (for float alignment)
//     +48  float    min_soc_pct
//     +52  float    max_soc_pct
//     = 56 bytes total
//
// All persisted as a single NVS blob "bat_profiles_v1" in namespace
// "pm-battery-profile" (via battery_nvs.cpp):
//   [0]    uint8_t   version (=1)
//   [1..]  16 * sizeof(BatteryChemistryProfile) = 16 * 56 = 896 bytes
//   total = 897 bytes
// ──────────────────────────────────────────────────────────────────────────────

enum BatteryChemistryEnum {
    BAT_CHEM_LEAD_ACID = 0,
    BAT_CHEM_LIION,
    BAT_CHEM_LFP,
    BAT_CHEM_LIPO,
    BAT_CHEM_NICD,
    BAT_CHEM_NIMH,
    BAT_CHEM_CUSTOM,
};

#define BATTERY_PROFILE_NAME_MAX 16
#define BATTERY_BUILTIN_PROFILE_COUNT 4
#define BATTERY_MAX_PROFILES 16
#define BATTERY_PROFILE_BLOB_VERSION 1

struct BatteryChemistryProfile {
    uint8_t  id;
    char     name[BATTERY_PROFILE_NAME_MAX];
    uint8_t  chemistry;     // BatteryChemistryEnum
    float    nominal_voltage;
    float    rated_capacity_Ah;
    float    c_rating;
    float    cutoff_voltage;
    float    float_voltage;
    float    charge_efficiency;   // 0..1
    uint16_t cycle_life_rated;
    float    min_soc_pct;
    float    max_soc_pct;
};

static_assert(sizeof(BatteryChemistryProfile) == 56, "size drift — recompute layout above");

// Init: must be called once after init_settings() to populate built-in profiles
// and load any custom profile overrides from NVS.
void init_battery_profiles();

// Returns a const pointer to the profile by id (0..15) or nullptr if invalid.
const BatteryChemistryProfile* battery_profile_get(uint8_t id);
// Returns the count of populated profiles (always >= BUILTIN_PROFILE_COUNT).
uint8_t battery_profile_count();
// Lists ids of populated profiles. Returns count, fills out_ids[].
uint8_t battery_profile_list_ids(uint8_t* out_ids, uint8_t max);

// Set (create or update) a profile by id. Returns false if id is invalid
// (must be < BATTERY_MAX_PROFILES).
bool battery_profile_set(const BatteryChemistryProfile* profile);
// Delete a custom profile (id >= BUILTIN_PROFILE_COUNT). Built-ins cannot
// be deleted but can be overwritten with battery_profile_set().
bool battery_profile_delete(uint8_t id);

// Restore a built-in profile slot to its factory defaults.
void battery_profile_reset_builtin(uint8_t id);

// Returns the const built-in defaults table (length BUILTIN_PROFILE_COUNT).
const BatteryChemistryProfile* battery_profile_builtin_defaults();

const char* battery_chemistry_name(uint8_t chemistry);

#endif // BATTERY_PROFILE_H
