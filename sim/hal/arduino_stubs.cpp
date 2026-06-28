#include "Arduino.h"
#include <stdarg.h>

static unsigned long g_millis = 0;

unsigned long millis() {
    return g_millis;
}

void delay(unsigned long ms) {
    g_millis += ms;
}

int SerialClass::printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int r = vprintf(fmt, args);
    va_end(args);
    return r;
}

void SerialClass::println(const char* s) {
    printf("%s\n", s);
}

void SerialClass::println() {
    printf("\n");
}

SerialClass Serial;
