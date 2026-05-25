#ifndef CONNECTIVITY_MANAGER_H
#define CONNECTIVITY_MANAGER_H

#include <stddef.h>
#include <time.h>
#include "sensor_manager.h"

void init_connectivity();
void loop_connectivity();
void publish_data(const SensorData& data);
const char* get_local_ip_str();
time_t get_epoch_time();
void publish_data_http(const SensorData& data, const char* json_buffer, size_t json_len);
void publish_data_supabase(const SensorData& data);
void publish_log_batch();
void sync_calibration_to_supabase();
void sync_ble_pin_to_supabase();
bool get_ble_pin_from_supabase(char* pin_str, size_t len);

#endif
