#ifndef ERROR_REPORTER_H
#define ERROR_REPORTER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Maximum number of pending error reports kept in the ring buffer.
#define ERROR_REPORTER_MAX_PENDING 8

// Maximum length of an error message string.
#define ERROR_REPORTER_MSG_MAX 64

// A single error report.
struct ErrorReport {
    uint32_t uptime_ms;         // millis() when the error occurred
    char     subsystem[16];     // e.g. "sd", "mqtt", "sensor", "nvs"
    uint8_t  code;              // subsystem-specific error code
    char     message[ERROR_REPORTER_MSG_MAX];  // human-readable
};

// Report an error. The error is stored in a ring buffer and will be
// included in the next telemetry publish. Safe to call from any context
// (ISR-safe: no allocations, no locks).
void report_error(const char* subsystem, uint8_t code, const char* message);

// Get the number of pending (unpublished) error reports.
uint8_t pending_error_count();

// Pop the oldest pending error report. Returns false if none pending.
// After calling this, the caller is responsible for publishing the error
// (e.g. via MQTT or Supabase). The report is removed from the buffer.
bool pop_pending_error(ErrorReport* out);

// Clear all pending errors without publishing.
void clear_pending_errors();

#endif // ERROR_REPORTER_H
