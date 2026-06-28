#ifndef WIRE_H
#define WIRE_H

#include <stdint.h>

class TwoWire {
public:
    bool begin(int sda, int scl);
    void setClock(uint32_t freq);
};

extern TwoWire Wire;

#endif
