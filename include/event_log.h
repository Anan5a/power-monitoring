#ifndef EVENT_LOG_H
#define EVENT_LOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Maximum number of events kept in the ring buffer. Oldest are dropped
// when full. 64 entries × ~80 bytes ≈ 5 KB RAM.
#define EVENT_LOG_MAX_ENTRIES 64

// Maximum length of an event message (including null terminator).
#define EVENT_LOG_MSG_MAX 60

// Severity levels (lower = more severe, for easy filtering).
#define EVENT_LOG_DEBUG 0
#define EVENT_LOG_INFO  1
#define EVENT_LOG_WARN  2
#define EVENT_LOG_ERROR 3

// A single device event.
struct EventLogEntry {
    uint32_t uptime_ms;         // millis() when the event occurred
    uint8_t  severity;          // EVENT_LOG_DEBUG/INFO/WARN/ERROR
    char     subsystem[12];     // e.g. "wifi", "mqtt", "ota", "sd", "sensor"
    char     message[EVENT_LOG_MSG_MAX];
};

// Initialize the event log ring buffer.
void init_event_log();

// Log a device event. Safe to call from any context (no allocations).
// severity: EVENT_LOG_DEBUG/INFO/WARN/ERROR.
void log_event(uint8_t severity, const char* subsystem, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Get the total number of events logged since boot (including dropped).
uint32_t get_total_event_count();

// Get the number of events currently in the ring buffer.
uint8_t get_ring_event_count();

// Copy the event at index `idx` (0 = oldest in ring) into `out`.
// Returns false if idx is out of range.
bool get_ring_event(uint8_t idx, EventLogEntry* out);

// Dump all ring events to the serial console (for the serial `log` command).
void dump_event_log_serial();

#endif // EVENT_LOG_H
