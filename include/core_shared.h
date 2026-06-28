#ifndef CORE_SHARED_H
#define CORE_SHARED_H

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "sensor_manager.h"

extern QueueHandle_t g_sensor_queue;   // Core 1 → Core 0: latest SensorSnapshot
extern QueueHandle_t g_cmd_queue;      // Core 0 → Core 1: settings command JSON
extern SemaphoreHandle_t g_relay_mutex;

void init_core_shared();
void push_sensor_data(const SensorSnapshot& data);
bool pop_settings_cmd(char* cmd_type_buf, char* payload_json_buf, size_t buf_len);

#endif