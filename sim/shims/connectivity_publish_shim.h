// shims/connectivity_publish_shim.h
// =============================================================================
// Header for the slim shim that replaces connectivity_manager.cpp +
// switch_controller.cpp. The test includes the real connectivity_manager.h
// (so all the production-side declarations are visible) and pulls in the
// sim-only knobs declared here to drive telemetry_build() with controlled
// time_source / IP / epoch state.
//
// Also pulls in <ESP.h> so telemetry.cpp's `ESP.getFreeHeap()` resolves
// without modifying any firmware source.
// =============================================================================

#include <ESP.h>
#include <time.h>

// Test-controllable knobs (defined in shims/connectivity_publish_shim.cpp).
void sim_set_local_ip(const char* ip);
void sim_set_epoch(time_t epoch);
void sim_set_ntp_synced(bool synced);
bool sim_ntp_synced();

// Test-controllable knobs for telemetry deps (defined in shims/telemetry_deps_stubs.cpp).
void sim_set_crash_count(uint32_t n);
void sim_set_ble_active(bool active);
void sim_set_ble_connected(bool connected);
void sim_set_mqtt_connected(bool connected);
void sim_set_network_skipped(bool skipped);

// OTA state knobs (defined in shims/telemetry_deps_stubs.cpp).
#include "ota_client.h"
void sim_set_ota_state(OtaState st);
void sim_set_ota_version(const char* v);
void sim_set_ota_progress(uint8_t pct);
void sim_set_ota_error(const char* e);
