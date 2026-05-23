#ifndef RELAY_CONTROLLER_H
#define RELAY_CONTROLLER_H

#include "sensor_manager.h"

void init_relays();
void evaluate_relays(const SensorData& data);

#endif
