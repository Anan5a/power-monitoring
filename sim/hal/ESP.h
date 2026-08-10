#ifndef ESP_H
#define ESP_H

#include <stdint.h>

// Minimal stand-in for the Arduino ESP class. The real API has dozens of
// methods; the firmware only calls getFreeHeap() in the publish paths
// we exercise from the host-side test.
class ESPClass {
public:
    uint32_t getFreeHeap();
    uint32_t getMinFreeHeap();
};

extern ESPClass ESP;

#endif
