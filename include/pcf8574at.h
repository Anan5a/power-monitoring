#ifndef PCF8574AT_H
#define PCF8574AT_H

#include <stdint.h>
#include <stdbool.h>

// ── PCF8574AT I2C I/O Expander Driver ──────────────────────────────────────
//
// The PCF8574AT is an 8-bit I2C I/O expander. It drives 8 outputs (or reads
// 8 inputs) over a single I2C address. This driver implements output mode for
// relay control.
//
// I2C address: 0x38-0x3F depending on A0-A2 pin strapping.
//   Default (all high = VCC): 0x3F
//   All low (GND):             0x38
//
// Write: send one byte — each bit corresponds to an I/O pin (bit 0 = P0).
// Read:  read one byte — returns the pin states.
//
// The PCF8574AT has a quasi-bidirectional architecture: when a pin is set HIGH
// it can source only ~100 µA, so outputs driving relays should be active-LOW
// (relay ON = pin LOW) with external pull-up resistors, OR the expander output
// should drive a transistor/ MOSFET that switches the relay coil.
//
// This driver assumes the shared `Wire` bus is already initialized by
// sensor_manager.cpp before any pcf8574at_* function is called.
//
// Fault tolerance: the driver tracks a shadow copy of the last
// successfully-written output byte and uses that (never a live bus read) as
// the base for single-pin read-modify-write updates. A transient I2C read
// failure must never be allowed to seed a write — an all-1s/0s fallback value
// written back over the bus would clobber every OTHER output pin's state,
// not just the one being changed.

// Default I2C address when A0=A1=A2=VCC
#define PCF8574AT_ADDR_DEFAULT  0x3F

// Initialize the PCF8574AT. Sets all outputs LOW (relays off).
// `addr` is the 7-bit I2C address (e.g. 0x3F).
// Returns false if the initial bus write failed (wrong address, bus
// contention, missing pull-ups) — the driver stays disabled until the next
// pcf8574at_init() call.
bool pcf8574at_init(uint8_t addr);

// Write all 8 output states at once. `value` bit 0 = P0, bit 7 = P7.
// Returns false if the I2C write failed; the shadow byte is only updated on
// success, so a failed write is never silently treated as applied.
bool pcf8574at_write_byte(uint8_t value);

// Set a single output pin (0-7) to the given state.
// Read-modify-write against the in-driver shadow byte (last confirmed-good
// write), never a live bus read — see the fault-tolerance note above.
// Returns false if the write failed (or pin/init state was invalid), in
// which case the shadow byte is left unchanged.
bool pcf8574at_set_pin(uint8_t pin, bool state);

// Read the current state of all 8 pins directly off the bus. Returns a byte
// where bit N = pin PN. Diagnostic/input-mode use only — never used as the
// basis for a write (see pcf8574at_set_pin).
uint8_t pcf8574at_read();

#endif // PCF8574AT_H
