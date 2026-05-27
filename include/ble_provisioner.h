#ifndef BLE_PROVISIONER_H
#define BLE_PROVISIONER_H

#include <stdint.h>
#include <stddef.h>

void init_ble_provisioner();
void loop_ble_provisioner();
void ble_notify_sensor_data(const char* data, size_t len);
void sync_ble_pin_to_supabase();
bool get_ble_pin_from_supabase(char* pin_str, size_t len);

// Apply a settings command received from Supabase (no PIN required, trusted channel)
void apply_settings_command(const char* cmd_type, const char* payload_json);

#endif
