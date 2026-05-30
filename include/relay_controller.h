#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include "sensor_manager.h"

void init_relays();
void evaluate_relays(const SensorData& data);
void relay_set_auto(bool enabled);
bool get_relay_state(uint8_t idx);

#endif
