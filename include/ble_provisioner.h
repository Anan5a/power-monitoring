#ifndef BLE_PROVISIONER_H
#define BLE_PROVISIONER_H

#include <stdint.h>
#include <stddef.h>

void init_ble_provisioner();
void loop_ble_provisioner();
void ble_notify_sensor_data(const char* data, size_t len);
void sync_ble_pin_to_supabase();
bool get_ble_pin_from_supabase(char* pin_str, size_t len);

#endif
