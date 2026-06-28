#include "Wire.h"

bool TwoWire::begin(int sda, int scl) {
    (void)sda;
    (void)scl;
    return true;
}

void TwoWire::setClock(uint32_t freq) {
    (void)freq;
}

TwoWire Wire;
