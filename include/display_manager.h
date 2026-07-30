#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include "telemetry.h"
#include <Arduino.h>

void init_display();
void update_display(const TelemetrySnapshot& snap);

#endif
