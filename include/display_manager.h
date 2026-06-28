#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "sensor_manager.h"
#include <Arduino.h>

void init_display();
void update_display(const SensorSnapshot& data, const char* ip_str, float total_power);

#endif
