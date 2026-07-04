// shims/connectivity_publish_shim.cpp
// =============================================================================
// Slim replacement for connectivity_manager.cpp + switch_controller.cpp that
// exposes only the bits telemetry.cpp and the test harness actually call.
// The full connectivity_manager.cpp pulls in PubSubClient + Blynk + mbedTLS,
// and switch_controller.cpp uses vTaskDelay + publish_switch_state (which
// itself pulls connectivity_manager), none of which compile on the host. We
// provide just enough surface for the test to drive telemetry_build() and
// inspect the resulting snapshot, plus a few switch helpers for the JSON
// payload.
//
// This is a SHIM in the sim/ tree, not a firmware source change.
// =============================================================================

#include "connectivity_manager.h"   // real header — declares get_local_ip_str, get_epoch_time, ntp_is_synced
#include "switch_controller.h"
#include "Arduino.h"
// On the real firmware ESP is brought in via the Arduino core's Esp.h. We
// provide a small shim in hal/ESP.h + hal/esp_stub.cpp to satisfy the
// `ESP.getFreeHeap()` call in telemetry.cpp.
#include <ESP.h>
#include <time.h>
#include <string.h>

// Test-controllable IP/epoch knobs. The test sets these before calling
// telemetry_build() to assert time_source / wifi.ip / ts fields.
static const char* g_ip_str = "192.168.1.42";
static time_t     g_epoch  = 0;
static bool       g_ntp_synced = true;

void sim_set_local_ip(const char* ip) { g_ip_str = ip ? ip : "0.0.0.0"; }
void sim_set_epoch(time_t epoch) { g_epoch = epoch; }
void sim_set_ntp_synced(bool synced) { g_ntp_synced = synced; }
bool sim_ntp_synced() { return g_ntp_synced; }

const char* get_local_ip_str() { return g_ip_str; }
time_t get_epoch_time() { return g_epoch; }
bool   ntp_is_synced() { return g_ntp_synced; }

// Switch state: per-switch bool, controlled by the test for telemetry output.
static bool g_switch_state[8] = {false};
static bool g_switch_auto_enabled = false;
static bool g_switch_present[8] = {false};

void   init_switches() {}
void   evaluate_switches(const SensorSnapshot&) {}
void   switch_set(uint8_t idx, bool is_energized) {
    if (idx < 8) { g_switch_state[idx] = is_energized; g_switch_present[idx] = true; }
}
void   switch_pulse(uint8_t /*idx*/, uint32_t /*duration_ms*/) {}
bool   get_switch_state(uint8_t idx) { return idx < 8 && g_switch_state[idx]; }
void   switch_set_auto(bool enabled) { g_switch_auto_enabled = enabled; }
bool   switch_get_auto_enabled() { return g_switch_auto_enabled; }
bool   switch_gpio_allowed(int8_t /*pin*/) { return true; }
void   switch_rule_default_init(SwitchRule* rule, uint8_t /*switch_idx*/, uint8_t /*channel*/) {
    if (rule) memset(rule, 0, sizeof(*rule));
}
const char* switch_condition_kind_name(uint8_t) { return "?"; }
const char* switch_condition_op_name(uint8_t)   { return "?"; }
const char* switch_logic_name(uint8_t)           { return "?"; }
