#ifndef BLE_PROVISIONER_H
#define BLE_PROVISIONER_H

#include <stdint.h>
#include <stddef.h>

void init_ble_provisioner();
void loop_ble_provisioner();
void ble_notify_sensor_data(const char* data, size_t len);

#endif
