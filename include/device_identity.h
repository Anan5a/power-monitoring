#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Maximum length of the device serial string (including null terminator).
// Format: "PM-" + 6 hex bytes of MAC = PM-AABBCCDDEEFF (14 chars + null).
#define DEVICE_SERIAL_MAX_LEN 24

// Maximum length of the hardware revision string.
#define DEVICE_HW_REV_MAX_LEN 16

// Initialize device identity. On first boot, generates a unique serial
// number from the MAC address and persists it to NVS. On subsequent
// boots, loads from NVS. Safe to call multiple times.
void init_device_identity();

// Get the persistent device serial number. Returns a pointer to a
// static buffer, valid for the lifetime of the device.
const char* get_device_serial();

// Get the hardware revision string (from compile-time define or NVS).
const char* get_device_hw_rev();

// Get the number of crashes recorded since the last clean boot.
// Incremented on boot if the previous shutdown was not clean.
uint32_t get_crash_count();

// Reset the crash counter (call after a successful healthy boot cycle).
void reset_crash_count();

// Record a clean shutdown marker in NVS so the next boot knows the
// previous shutdown was intentional. Call from the reboot/factory-reset
// path before ESP.restart().
void mark_clean_shutdown();

#endif // DEVICE_IDENTITY_H
