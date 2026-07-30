#ifndef BLE_PROVISIONER_H
#define BLE_PROVISIONER_H

#include <stdint.h>
#include <stddef.h>

void init_ble_provisioner();    // create server/service/chars, no advertising
void start_ble_advertising();   // begin BLE advertising (call after WiFi is up)
void stop_ble_advertising();    // stop BLE advertising
void deinit_ble_provisioner();  // tear down NimBLE stack — frees ~50KB heap
void loop_ble_provisioner();
void ble_notify_sensor_data(const char* data, size_t len);
void sync_ble_pin_to_supabase();
bool get_ble_pin_from_supabase(char* pin_str, size_t len);

// Apply a settings command received from Supabase (no PIN required, trusted channel).
// Returns true on successful apply, false if the command was rejected (unknown
// command, bad payload, or missing required fields). Callers (e.g. the Supabase
// poller) should only run the post-hook (WiFi reconnect, MQTT reconnect, etc.)
// on success — a failed apply must not trigger side effects.
bool apply_settings_command(const char* cmd_type, const char* payload_json);

// Device state accessors
bool ble_is_active();       // NimBLE stack initialized
bool ble_is_connected();    // client currently paired/connected

#endif
