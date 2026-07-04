// log_serial.h
// =============================================================================
// Thin wrapper around the Arduino Serial console so the firmware can build in
// environments where `Serial` is not available (e.g. ESP32-C3 with USB-CDC off,
// where Serial is declared but unusable).
//
// Define HAS_SERIAL=0 in build_flags to compile out all debug log output.
// When HAS_SERIAL=1 (the default) every LOG_PRINT/LOG_PRINTLN call routes to
// the normal Serial.printf/println. When HAS_SERIAL=0 they expand to no-ops.
// =============================================================================

#ifndef LOG_SERIAL_H
#define LOG_SERIAL_H

#include "config.h"

#if HAS_SERIAL
    #define LOG_PRINT(...)    Serial.printf(__VA_ARGS__)
    #define LOG_PRINTLN(x)    Serial.println(x)
#else
    #define LOG_PRINT(...)    do {} while (0)
    #define LOG_PRINTLN(x)    do {} while (0)
#endif

#endif // LOG_SERIAL_H
