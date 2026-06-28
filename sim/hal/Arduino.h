#ifndef ARDUINO_H
#define ARDUINO_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef bool boolean;
typedef uint8_t byte;

#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

unsigned long millis();
void delay(unsigned long ms);

inline long random(long max) {
    if (max <= 0) return 0;
    return rand() % max;
}

inline long random(long min, long max) {
    if (min >= max) return min;
    return min + rand() % (max - min);
}

class SerialClass {
public:
    int printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    void println(const char* s);
    void println();
};

extern SerialClass Serial;

#endif
