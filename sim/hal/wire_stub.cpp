#include "Wire.h"
#include <string.h>

// In-memory buffer for I2C transaction simulation.
// The PCF8574AT driver writes a single byte and reads a single byte.
// We store the last written value and return it on read.
static uint8_t g_i2c_reg = 0x00;  // simulated output latch
static uint8_t g_i2c_addr = 0;
static bool    g_i2c_writing = false;
static int     g_i2c_available = 0;
static uint8_t g_i2c_read_buf = 0;

bool TwoWire::begin(int sda, int scl) {
    (void)sda;
    (void)scl;
    return true;
}

void TwoWire::setClock(uint32_t freq) {
    (void)freq;
}

void TwoWire::beginTransmission(uint8_t address) {
    g_i2c_addr = address;
    g_i2c_writing = true;
}

uint8_t TwoWire::write(uint8_t data) {
    if (g_i2c_writing) {
        g_i2c_reg = data;  // store the last written value
    }
    return 1;  // return 1 byte written
}

uint8_t TwoWire::endTransmission(bool stopBit) {
    (void)stopBit;
    g_i2c_writing = false;
    return 0;  // 0 = success
}

uint8_t TwoWire::requestFrom(uint8_t address, uint8_t quantity) {
    (void)address;
    // Return the last written value as the simulated pin state
    g_i2c_read_buf = g_i2c_reg;
    g_i2c_available = (quantity > 0) ? 1 : 0;
    return g_i2c_available;
}

int TwoWire::available() {
    return g_i2c_available;
}

int TwoWire::read() {
    if (g_i2c_available > 0) {
        g_i2c_available = 0;
        return g_i2c_read_buf;
    }
    return -1;
}

TwoWire Wire;
