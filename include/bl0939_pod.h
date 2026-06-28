#ifndef BL0939_POD_H
#define BL0939_POD_H

#include <stdint.h>
#include "sensor_pod.h"

// Initialize the configured BL0939 UART meter(s).
void bl0939_pod_init();

// Read into a PodState. The PodState must already describe the device/channel layout.
void bl0939_pod_read(PodState* pod);

#endif
