#ifndef CONNECTIVITY_MANAGER_H
#define CONNECTIVITY_MANAGER_H

#include <stddef.h>
#include <time.h>
#include "sensor_manager.h"

float get_sensor_voltage(uint8_t src, uint8_t idx, const SensorSnapshot& data);
float get_sensor_current(uint8_t src, uint8_t idx, const SensorSnapshot& data);
float get_sensor_power(uint8_t src, uint8_t idx, const SensorSnapshot& data);

#endif
