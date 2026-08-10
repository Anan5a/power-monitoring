// shims/telemetry_deps_stubs.cpp
// =============================================================================
// Host-side stubs for modules that telemetry.cpp depends on but that don't
// compile on the host (ble_provisioner, ota_client, device_identity, and
// parts of connectivity_manager).
//
// These provide just enough surface for telemetry_build() to link and return
// deterministic values. The test harness can override knobs via the sim_set_*
// helpers in connectivity_publish_shim.cpp.
// =============================================================================

#include "device_identity.h"
#include "ble_provisioner.h"
#include "ota_client.h"
#include "connectivity_manager.h"
#include "data_logger.h"
#include <string.h>
#include <stdint.h>

// ── device_identity stubs ───────────────────────────────────────────────────

static const char* kTestSerial = "AABBCCDDEEFF";
static const char* kTestHwRev  = "rev1.0";
static uint32_t    g_crash_count = 0;

void init_device_identity() {}
const char* get_device_serial() { return kTestSerial; }
const char* get_device_hw_rev() { return kTestHwRev; }
uint32_t get_crash_count() { return g_crash_count; }
void reset_crash_count() { g_crash_count = 0; }
void mark_clean_shutdown() {}

// Test-only knob to set crash count for safe_mode assertions.
void sim_set_crash_count(uint32_t n) { g_crash_count = n; }

// ── ble_provisioner stubs ───────────────────────────────────────────────────

static bool g_ble_active = false;
static bool g_ble_connected = false;

void init_ble_provisioner() {}
void start_ble_advertising() {}
void stop_ble_advertising() {}
void deinit_ble_provisioner() {}
void loop_ble_provisioner() {}
void ble_notify_sensor_data(const char*, size_t) {}
void sync_ble_pin_to_supabase() {}
bool get_ble_pin_from_supabase(char*, size_t) { return false; }
bool apply_settings_command(const char*, const char*) { return false; }
bool ble_is_active() { return g_ble_active; }
bool ble_is_connected() { return g_ble_connected; }

void sim_set_ble_active(bool active) { g_ble_active = active; }
void sim_set_ble_connected(bool connected) { g_ble_connected = connected; }

// ── ota_client stubs ────────────────────────────────────────────────────────

static OtaState g_ota_state = OTA_IDLE;
static const char* g_ota_version = "";
static uint8_t g_ota_progress = 0;
static const char* g_ota_error = "";

void init_ota_client() {}
void loop_ota_client() {}
void ota_trigger_check() {}
OtaState ota_get_state() { return g_ota_state; }
const char* ota_get_version() { return g_ota_version; }
uint8_t ota_get_progress_pct() { return g_ota_progress; }
const char* ota_get_last_error() { return g_ota_error; }
void ota_set_poll_interval(uint32_t) {}
uint32_t ota_get_poll_interval() { return 300; }
void ota_confirm_valid() {}

void sim_set_ota_state(OtaState st) { g_ota_state = st; }
void sim_set_ota_version(const char* v) { g_ota_version = v ? v : ""; }
void sim_set_ota_progress(uint8_t pct) { g_ota_progress = pct; }
void sim_set_ota_error(const char* e) { g_ota_error = e ? e : ""; }

// ── connectivity_manager stubs (not in connectivity_publish_shim.cpp) ────────

static bool g_mqtt_connected = false;
static bool g_network_skipped = false;

bool mqtt_is_connected() { return g_mqtt_connected; }
bool network_is_skipped() { return g_network_skipped; }

void sim_set_mqtt_connected(bool connected) { g_mqtt_connected = connected; }
void sim_set_network_skipped(bool skipped) { g_network_skipped = skipped; }

// ── event_log stubs ──────────────────────────────────────────────────────────
// data_logger.cpp calls log_event() for SD card I/O errors. The sim doesn't
// exercise the SD path, so a no-op stub is sufficient.

#include "event_log.h"
void log_event(uint8_t /*severity*/, const char* /*module*/, const char* /*fmt*/, ...) {}
