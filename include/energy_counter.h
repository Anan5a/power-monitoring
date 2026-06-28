#ifndef ENERGY_COUNTER_H
#define ENERGY_COUNTER_H

#include "sensor_manager.h"

void init_energy_counter();
void update_energy_counter(const SensorSnapshot& data, float dt_seconds);
float get_energy_Wh(uint8_t channel); // 0-3
void reset_energy_counter(uint8_t channel);

#endif
