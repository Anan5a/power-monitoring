#ifndef COULOMB_COUNTER_H
#define COULOMB_COUNTER_H

#include "sensor_manager.h"

void init_coulomb_counter();
void update_coulomb_counter(const SensorData& data, float dt_seconds);
float get_coulomb_mAh(uint8_t channel); // 0-3
void reset_coulomb_counter(uint8_t channel);

#endif
