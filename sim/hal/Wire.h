#ifndef WIRE_H
#define WIRE_H

#include <stdint.h>

class TwoWire {
public:
    bool begin(int sda, int scl);
    void setClock(uint32_t freq);

    // I2C transaction API (used by PCF8574AT driver)
    void beginTransmission(uint8_t address);
    uint8_t write(uint8_t data);
    uint8_t endTransmission(bool stopBit = true);
    uint8_t requestFrom(uint8_t address, uint8_t quantity);
    int available();
    int read();
};

extern TwoWire Wire;

#endif
