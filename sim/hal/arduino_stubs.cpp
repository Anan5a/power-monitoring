#include "Arduino.h"
#include <stdarg.h>

static unsigned long g_millis = 0;

unsigned long millis() {
    return g_millis;
}

void delay(unsigned long ms) {
    g_millis += ms;
}

// Test-only hook: set millis() to an absolute value. Used by tests that
// need to simulate long uptime windows (e.g. the 7-day capacity-test
// stale-recovery bound). Host-only; production firmware never calls this.
void set_millis_for_test(unsigned long ms) {
    g_millis = ms;
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
