#ifndef SPI_H
#define SPI_H

#include <stdint.h>

// Minimal stub for the Arduino SPI library. The sim's data_logger.cpp
// includes <SPI.h> for SD card communication. On the host we provide
// no-op stubs so the module compiles.

class SPIClass {
public:
    void begin(int sck = -1, int miso = -1, int mosi = -1, int ss = -1) {
        (void)sck; (void)miso; (void)mosi; (void)ss;
    }
    void end() {}
    void beginTransaction(uint32_t settings) { (void)settings; }
    void endTransaction() {}
    uint8_t transfer(uint8_t data) { return data; }
    void transfer(uint8_t* buf, size_t count) { (void)buf; (void)count; }
    void setFrequency(uint32_t freq) { (void)freq; }
    void setDataMode(uint8_t mode) { (void)mode; }
    void setBitOrder(uint8_t order) { (void)order; }
};

extern SPIClass SPI;

// SPI mode constants
#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3

// Bit order
#define LSBFIRST 0
#define MSBFIRST 1

#endif // SPI_H
