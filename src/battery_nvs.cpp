// battery_nvs.cpp
// =============================================================================
// Single TU-owned Preferences handle for the battery subsystem. Consolidates
// the two anonymous-namespace `Preferences prefs;` instances that previously
// lived in battery_state.cpp and battery_profile.cpp. Two distinct NVS
// namespaces are used:
//   - "pm-battery-state"   : BatteryState per-channel blobs + binding table
//   - "pm-battery-profile" : BatteryChemistryProfile registry
//
// Two layers of API are exported:
//   1. The public battery_nvs_* helpers take the g_battery_mux critical
//      section for the entire NVS round-trip. Callers that are already
//      holding the lock MUST use the internal `*_locked` variants below to
//      avoid a self-deadlock (taskENTER_CRITICAL is not recursive).
//   2. The internal `*_locked` helpers assume the caller already holds the
//      critical section; they perform only the Preferences I/O.
// =============================================================================

#include "battery_nvs.h"
#include "battery_lock.h"
#include <Preferences.h>

static Preferences g_state_prefs;
static Preferences g_profile_prefs;
static bool        g_state_opened   = false;
static bool        g_profile_opened = false;

// ── Internal: prefer the *already open* Preferences handle. We intentionally
// do NOT lazily `begin()` inside _locked paths; callers that hold the lock
// should have already opened the namespace. This keeps the _locked variants
// cheap (a single I/O call, no begin/end).
static bool ensure_state_prefs_open() {
    if (g_state_opened) return true;
    g_state_opened = g_state_prefs.begin(kBatteryStateNs, false);
    return g_state_opened;
}

static bool ensure_profile_prefs_open() {
    if (g_profile_opened) return true;
    g_profile_opened = g_profile_prefs.begin(kBatteryProfileNs, false);
    return g_profile_opened;
}

// ── Internal _locked variants (caller holds BATTERY_LOCK) ───────────────────

static bool state_get_locked(const char* key, void* buf, size_t max_len, size_t* out_len) {
    if (!key || !buf || max_len == 0) return false;
    if (!ensure_state_prefs_open()) return false;
    size_t len = g_state_prefs.getBytesLength(key);
    if (out_len) *out_len = len;
    if (len != max_len) return false;
    g_state_prefs.getBytes(key, buf, max_len);
    return true;
}

static bool state_put_locked(const char* key, const void* buf, size_t len) {
    if (!key || !buf) return false;
    if (!ensure_state_prefs_open()) return false;
    return g_state_prefs.putBytes(key, buf, len) == len;
}

static bool state_get_u8_locked(const char* key, uint8_t* out) {
    if (!key || !out) return false;
    if (!ensure_state_prefs_open()) return false;
    if (!g_state_prefs.isKey(key)) return false;
    *out = g_state_prefs.getUChar(key, 0);
    return true;
}

static bool state_put_u8_locked(const char* key, uint8_t v) {
    if (!key) return false;
    if (!ensure_state_prefs_open()) return false;
    return g_state_prefs.putUChar(key, v) == sizeof(uint8_t);
}

static void state_remove_locked(const char* key) {
    if (!key) return;
    if (!ensure_state_prefs_open()) return;
    g_state_prefs.remove(key);
}

static bool profile_get_locked(const char* key, void* buf, size_t max_len, size_t* out_len) {
    if (!key || !buf || max_len == 0) return false;
    if (!ensure_profile_prefs_open()) return false;
    size_t len = g_profile_prefs.getBytesLength(key);
    if (out_len) *out_len = len;
    if (len != max_len) return false;
    g_profile_prefs.getBytes(key, buf, max_len);
    return true;
}

static bool profile_put_locked(const char* key, const void* buf, size_t len) {
    if (!key || !buf) return false;
    if (!ensure_profile_prefs_open()) return false;
    return g_profile_prefs.putBytes(key, buf, len) == len;
}

static bool profile_get_u8_locked(const char* key, uint8_t* out) {
    if (!key || !out) return false;
    if (!ensure_profile_prefs_open()) return false;
    if (!g_profile_prefs.isKey(key)) return false;
    *out = g_profile_prefs.getUChar(key, 0);
    return true;
}

static bool profile_put_u8_locked(const char* key, uint8_t v) {
    if (!key) return false;
    if (!ensure_profile_prefs_open()) return false;
    return g_profile_prefs.putUChar(key, v) == sizeof(uint8_t);
}

// ── Public _locked variants (caller holds BATTERY_LOCK) ────────────────────

bool battery_nvs_state_get_locked(const char* key, void* buf, size_t max_len, size_t* out_len) {
    return state_get_locked(key, buf, max_len, out_len);
}

bool battery_nvs_state_put_locked(const char* key, const void* buf, size_t len) {
    return state_put_locked(key, buf, len);
}

bool battery_nvs_state_get_u8_locked(const char* key, uint8_t* out) {
    return state_get_u8_locked(key, out);
}

bool battery_nvs_state_put_u8_locked(const char* key, uint8_t v) {
    return state_put_u8_locked(key, v);
}

void battery_nvs_state_remove_locked(const char* key) {
    state_remove_locked(key);
}

bool battery_nvs_profile_get_locked(const char* key, void* buf, size_t max_len, size_t* out_len) {
    return profile_get_locked(key, buf, max_len, out_len);
}

bool battery_nvs_profile_put_locked(const char* key, const void* buf, size_t len) {
    return profile_put_locked(key, buf, len);
}

bool battery_nvs_profile_get_u8_locked(const char* key, uint8_t* out) {
    return profile_get_u8_locked(key, out);
}

bool battery_nvs_profile_put_u8_locked(const char* key, uint8_t v) {
    return profile_put_u8_locked(key, v);
}

// ── Public API (takes the lock) ─────────────────────────────────────────────

bool battery_nvs_state_open() {
    BATTERY_LOCK();
    bool ok = ensure_state_prefs_open();
    BATTERY_UNLOCK();
    return ok;
}

void battery_nvs_state_close() {
    if (!g_state_opened) return;
    BATTERY_LOCK();
    g_state_prefs.end();
    g_state_opened = false;
    BATTERY_UNLOCK();
}

bool battery_nvs_state_get(const char* key, void* buf, size_t max_len, size_t* out_len) {
    BATTERY_LOCK();
    bool ok = state_get_locked(key, buf, max_len, out_len);
    BATTERY_UNLOCK();
    return ok;
}

bool battery_nvs_state_put(const char* key, const void* buf, size_t len) {
    BATTERY_LOCK();
    bool ok = state_put_locked(key, buf, len);
    BATTERY_UNLOCK();
    return ok;
}

bool battery_nvs_state_get_u8(const char* key, uint8_t* out) {
    BATTERY_LOCK();
    bool found = state_get_u8_locked(key, out);
    BATTERY_UNLOCK();
    return found;
}

bool battery_nvs_state_put_u8(const char* key, uint8_t v) {
    BATTERY_LOCK();
    bool ok = state_put_u8_locked(key, v);
    BATTERY_UNLOCK();
    return ok;
}

void battery_nvs_state_remove(const char* key) {
    if (!key) return;
    BATTERY_LOCK();
    state_remove_locked(key);
    BATTERY_UNLOCK();
}

bool battery_nvs_profile_open() {
    BATTERY_LOCK();
    bool ok = ensure_profile_prefs_open();
    BATTERY_UNLOCK();
    return ok;
}

void battery_nvs_profile_close() {
    if (!g_profile_opened) return;
    BATTERY_LOCK();
    g_profile_prefs.end();
    g_profile_opened = false;
    BATTERY_UNLOCK();
}

bool battery_nvs_profile_get(const char* key, void* buf, size_t max_len, size_t* out_len) {
    BATTERY_LOCK();
    bool ok = profile_get_locked(key, buf, max_len, out_len);
    BATTERY_UNLOCK();
    return ok;
}

bool battery_nvs_profile_put(const char* key, const void* buf, size_t len) {
    BATTERY_LOCK();
    bool ok = profile_put_locked(key, buf, len);
    BATTERY_UNLOCK();
    return ok;
}

bool battery_nvs_profile_get_u8(const char* key, uint8_t* out) {
    BATTERY_LOCK();
    bool found = profile_get_u8_locked(key, out);
    BATTERY_UNLOCK();
    return found;
}

bool battery_nvs_profile_put_u8(const char* key, uint8_t v) {
    BATTERY_LOCK();
    bool ok = profile_put_u8_locked(key, v);
    BATTERY_UNLOCK();
    return ok;
}

