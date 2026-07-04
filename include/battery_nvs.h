#ifndef BATTERY_NVS_H
#define BATTERY_NVS_H

// battery_nvs.h
// =============================================================================
// Public API for the shared battery NVS module. All callers (battery_state.cpp,
// battery_profile.cpp, capacity_test.cpp) route their NVS operations through
// these helpers so we own one Preferences instance per namespace and one
// critical section that protects both.
// =============================================================================

#include <stdint.h>
#include <stddef.h>

constexpr const char* kBatteryStateNs   = "pm-battery-state";
constexpr const char* kBatteryProfileNs = "pm-battery-profile";

// State-namespace API (used by battery_state.cpp)
bool battery_nvs_state_open();
void battery_nvs_state_close();
bool battery_nvs_state_get(const char* key, void* buf, size_t max_len, size_t* out_len);
bool battery_nvs_state_put(const char* key, const void* buf, size_t len);
bool battery_nvs_state_get_u8(const char* key, uint8_t* out);
bool battery_nvs_state_put_u8(const char* key, uint8_t v);
void battery_nvs_state_remove(const char* key);

// Profile-namespace API (used by battery_profile.cpp)
bool battery_nvs_profile_open();
void battery_nvs_profile_close();
bool battery_nvs_profile_get(const char* key, void* buf, size_t max_len, size_t* out_len);
bool battery_nvs_profile_put(const char* key, const void* buf, size_t len);
bool battery_nvs_profile_get_u8(const char* key, uint8_t* out);
bool battery_nvs_profile_put_u8(const char* key, uint8_t v);

// ── Internal _locked variants ──────────────────────────────────────────────
// These DO NOT take the battery lock. Callers MUST already hold it. The
// public helpers above are the safe entry points; the _locked variants are
// for use inside an existing BATTERY_LOCK() window where taking it again
// would deadlock (FreeRTOS taskENTER_CRITICAL is not recursive).
//
// Use _locked only when the I/O must be atomic with respect to other code
// that reads/writes the same g_battery_mux-protected state. Otherwise prefer
// the unlocked public helpers.
bool battery_nvs_state_get_locked(const char* key, void* buf, size_t max_len, size_t* out_len);
bool battery_nvs_state_put_locked(const char* key, const void* buf, size_t len);
bool battery_nvs_state_get_u8_locked(const char* key, uint8_t* out);
bool battery_nvs_state_put_u8_locked(const char* key, uint8_t v);
void battery_nvs_state_remove_locked(const char* key);
bool battery_nvs_profile_get_locked(const char* key, void* buf, size_t max_len, size_t* out_len);
bool battery_nvs_profile_put_locked(const char* key, const void* buf, size_t len);
bool battery_nvs_profile_get_u8_locked(const char* key, uint8_t* out);
bool battery_nvs_profile_put_u8_locked(const char* key, uint8_t v);

#endif // BATTERY_NVS_H
