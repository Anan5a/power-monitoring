#include "serial1_manager.h"
#include "config.h"
#include <HardwareSerial.h>

#if ENABLE_SERIAL1
static char rx_buffer[SERIAL1_BUFFER_SIZE];
static uint16_t rx_head = 0;  // write position
static uint16_t rx_tail = 0;  // read position

void init_serial1() {
    Serial1.begin(SERIAL1_BAUD, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
    rx_head = rx_tail = 0;
    Serial.printf("Serial1 enabled on RX=%d TX=%d at %d baud\n",
        SERIAL1_RX_PIN, SERIAL1_TX_PIN, SERIAL1_BAUD);
}

void loop_serial1() {
    while (Serial1.available() && ((rx_head + 1) % SERIAL1_BUFFER_SIZE) != rx_tail) {
        rx_buffer[rx_head] = Serial1.read();
        rx_head = (rx_head + 1) % SERIAL1_BUFFER_SIZE;
    }
}

bool serial1_read_line(char* buf, size_t len) {
    if (rx_tail == rx_head) return false;

    size_t i = 0;
    while (i < len - 1 && rx_tail != rx_head) {
        char c = rx_buffer[rx_tail];
        rx_tail = (rx_tail + 1) % SERIAL1_BUFFER_SIZE;

        if (c == '\n' || c == '\r') {
            if (i > 0) break;  // got at least one char before EOL
        } else {
            buf[i++] = c;
        }
    }
    buf[i] = '\0';
    return i > 0;
}

uint16_t serial1_available() {
    if (rx_head >= rx_tail) return rx_head - rx_tail;
    return SERIAL1_BUFFER_SIZE - rx_tail + rx_head;
}

#else  // ENABLE_SERIAL1 == 0
void init_serial1() {}
void loop_serial1() {}
bool serial1_read_line(char* buf, size_t len) { return false; }
uint16_t serial1_available() { return 0; }
#endif