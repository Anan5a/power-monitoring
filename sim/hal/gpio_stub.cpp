#include "gpio_stub.h"
#include <stdio.h>

void pinMode(uint8_t pin, uint8_t mode) {
    const char* mode_str = "unknown";
    switch (mode) {
        case 0: mode_str = "INPUT"; break;
        case 1: mode_str = "OUTPUT"; break;
        case 2: mode_str = "INPUT_PULLUP"; break;
    }
    printf("[GPIO] pinMode(%u, %s)\n", pin, mode_str);
}

void digitalWrite(uint8_t pin, uint8_t value) {
    printf("[GPIO] digitalWrite(%u, %s)\n", pin, value ? "HIGH" : "LOW");
}
