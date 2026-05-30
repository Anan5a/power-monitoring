#ifndef CONNECTIVITY_MANAGER_H
#define CONNECTIVITY_MANAGER_H

#include <stddef.h>
#include <time.h>
#include "sensor_manager.h"
#include "settings_manager.h"

void init_connectivity();
void loop_connectivity();
void publish_data(const SensorData& data);
const char* get_local_ip_str();
time_t get_epoch_time();
bool try_sync_epoch_time();
void publish_data_http(const SensorData& data, const char* json_buffer, size_t json_len);
void publish_data_supabase(const SensorData& data);
void publish_log_batch();
void publish_log_batch_supabase();
void sync_calibration_to_supabase();
void sync_ble_pin_to_supabase();
bool get_ble_pin_from_supabase(char* pin_str, size_t len);

// Virtual channel helpers: get sensor values by source type
// src: 0=none, 1=ina3221_volt(0x42), 2=ina3221_curr(0x40), 3=ina226, 4=ads1115
float get_sensor_voltage(uint8_t src, uint8_t idx, const SensorData& data);
float get_sensor_current(uint8_t src, uint8_t idx, const SensorData& data);
float get_sensor_power(uint8_t src, uint8_t idx, const SensorData& data);

// Settings commands: ESP32 polls Supabase for pending config changes
void check_settings_commands();
void publish_calibration_status();  // writes sensor_calibration_status table during active calibration
void apply_settings_posthook(const char* cmd_type);  // reconnect WiFi/MQTT, reset Supabase client after settings change

// Relay state publishing
void publish_relay_state(uint8_t idx, bool is_energized);

// Supabase WebSocket
bool isSupabaseWsConnected();

#endif
