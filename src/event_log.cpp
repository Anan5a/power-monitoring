#include "event_log.h"
#include "log_serial.h"
#include <Arduino.h>
#include <string.h>
#include <stdarg.h>

namespace {
struct EventRing {
    EventLogEntry buf[EVENT_LOG_MAX_ENTRIES];
    uint8_t head;       // next write position
    uint8_t count;      // entries currently in ring
    uint32_t total;     // total events logged since boot (including dropped)
};

static EventRing g_ring = {};
static bool g_initialized = false;
}  // namespace

void init_event_log() {
    memset(&g_ring, 0, sizeof(g_ring));
    g_initialized = true;
    log_event(EVENT_LOG_INFO, "system", "event log initialized");
}

void log_event(uint8_t severity, const char* subsystem, const char* fmt, ...) {
    if (!g_initialized) return;
    if (!subsystem || !fmt) return;

    EventLogEntry* e = &g_ring.buf[g_ring.head];
    e->uptime_ms = millis();
    e->severity = (severity > EVENT_LOG_ERROR) ? EVENT_LOG_ERROR : severity;

    // Copy subsystem (truncate if needed)
    strncpy(e->subsystem, subsystem, sizeof(e->subsystem) - 1);
    e->subsystem[sizeof(e->subsystem) - 1] = '\0';

    // Format message
    va_list args;
    va_start(args, fmt);
    vsnprintf(e->message, sizeof(e->message), fmt, args);
    va_end(args);
    e->message[sizeof(e->message) - 1] = '\0';

    // Advance head
    g_ring.head = (g_ring.head + 1) % EVENT_LOG_MAX_ENTRIES;
    if (g_ring.count < EVENT_LOG_MAX_ENTRIES) {
        g_ring.count++;
    }
    g_ring.total++;
}

uint32_t get_total_event_count() {
    return g_ring.total;
}

uint8_t get_ring_event_count() {
    return g_ring.count;
}

bool get_ring_event(uint8_t idx, EventLogEntry* out) {
    if (!out || idx >= g_ring.count) return false;
    // Calculate the actual buffer index: tail = (head - count) mod N
    uint8_t tail = (g_ring.head - g_ring.count) % EVENT_LOG_MAX_ENTRIES;
    uint8_t pos = (tail + idx) % EVENT_LOG_MAX_ENTRIES;
    *out = g_ring.buf[pos];
    return true;
}

void dump_event_log_serial() {
    if (g_ring.count == 0) {
        LOG_PRINTLN("[EVENT] no events logged");
        return;
    }
    LOG_PRINT("[EVENT] --- %u events (%u total since boot) ---\n",
              (unsigned)g_ring.count, (unsigned)g_ring.total);
    for (uint8_t i = 0; i < g_ring.count; i++) {
        EventLogEntry e;
        get_ring_event(i, &e);
        const char* sev = "?";
        if (e.severity == EVENT_LOG_DEBUG) sev = "DBG";
        else if (e.severity == EVENT_LOG_INFO) sev = "INF";
        else if (e.severity == EVENT_LOG_WARN) sev = "WRN";
        else if (e.severity == EVENT_LOG_ERROR) sev = "ERR";
        LOG_PRINT("[%s] [%s] %s: %s\n", sev, e.subsystem,
                  e.uptime_ms > 0 ? "          " : "boot", e.message);
        // Actually print uptime properly
    }
    // Re-dump with proper formatting
    for (uint8_t i = 0; i < g_ring.count; i++) {
        EventLogEntry e;
        get_ring_event(i, &e);
        const char* sev = "?";
        if (e.severity == EVENT_LOG_DEBUG) sev = "DBG";
        else if (e.severity == EVENT_LOG_INFO) sev = "INF";
        else if (e.severity == EVENT_LOG_WARN) sev = "WRN";
        else if (e.severity == EVENT_LOG_ERROR) sev = "ERR";
        LOG_PRINT("[%5lus] [%s] [%s] %s\n",
                  (unsigned long)(e.uptime_ms / 1000),
                  sev, e.subsystem, e.message);
    }
}
