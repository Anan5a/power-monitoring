#ifndef BL0939_POD_H
#define BL0939_POD_H

#include <stdint.h>
#include "sensor_pod.h"

// Wire-format constants. BL0939_FRAME_LEN is the on-wire frame size
// (1 addr + 8 × 3 bytes + 1 frequency + 1 checksum = 24 bytes); surfaced
// here so the test target can build a synthetic frame and round-trip
// the CRC algorithm without re-declaring the layout in two places.
#ifndef BL0939_FRAME_LEN
#define BL0939_FRAME_LEN 24
#endif

// Initialize the configured BL0939 UART meter(s).
void bl0939_pod_init();

// Read into a PodState. The PodState must already describe the device/channel layout.
void bl0939_pod_read(PodState* pod);

#ifdef BL0939_EXPOSE_FOR_TESTING
// Host-side test hooks — declared only when the production TU is compiled
// with BL0939_EXPOSE_FOR_TESTING=1 (set by the sim test target). These
// let the unit test exercise the frame parser / address-lookup without
// having to attach a real BL0939 to a UART. They are NOT part of the
// production ABI; production firmware never sees these symbols.
bool bl0939_parse_frame_for_test(const uint8_t* buf, PodState* pod);
int8_t bl0939_slot_for_address_for_test(uint8_t addr);
#endif

#endif
