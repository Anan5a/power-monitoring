#include "pcf8574at.h"
#include <Arduino.h>
#include <Wire.h>

// ── Static state ────────────────────────────────────────────────────────────

static uint8_t  g_addr = PCF8574AT_ADDR_DEFAULT;
static bool     g_inited = false;
static uint8_t  g_shadow = 0x00;  // last confirmed-good output byte

// ── Public API ──────────────────────────────────────────────────────────────

bool pcf8574at_init(uint8_t addr) {
    g_addr = addr;
    g_shadow = 0x00;

    // Set all outputs LOW (relays off on boot). The PCF8574AT powers on with
    // all pins HIGH (weak pull-up), so we must actively drive them LOW.
    Wire.beginTransmission(g_addr);
    Wire.write(g_shadow);
    uint8_t err = Wire.endTransmission();
    g_inited = (err == 0);
    return g_inited;
}

bool pcf8574at_write_byte(uint8_t value) {
    if (!g_inited) return false;
    Wire.beginTransmission(g_addr);
    Wire.write(value);
    uint8_t err = Wire.endTransmission();
    if (err != 0) return false;
    g_shadow = value;
    return true;
}

bool pcf8574at_set_pin(uint8_t pin, bool state) {
    if (!g_inited || pin > 7) return false;

    // Read-modify-write against the shadow byte — NOT a live bus read. A
    // transient I2C read failure must never seed a write: it would clobber
    // every other output pin, not just this one.
    uint8_t next = g_shadow;
    if (state) {
        next |= (1 << pin);
    } else {
        next &= ~(1 << pin);
    }
    return pcf8574at_write_byte(next);
}

uint8_t pcf8574at_read() {
    if (!g_inited) return 0xFF;

    Wire.requestFrom(g_addr, (uint8_t)1);
    if (Wire.available()) {
        return Wire.read();
    }
    return 0xFF;  // bus error — return all-high (safe: relays off if active-LOW)
}
