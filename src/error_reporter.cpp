#include "error_reporter.h"
#include <Arduino.h>
#include <string.h>

namespace {
struct ErrorRing {
    ErrorReport buf[ERROR_REPORTER_MAX_PENDING];
    uint8_t head;  // next write position
    uint8_t tail;  // next read position
    uint8_t count; // number of pending reports
};

static ErrorRing g_ring = {};

// Copy a string safely, truncating if necessary.
static void safe_copy(char* dst, const char* src, size_t dst_size) {
    if (!dst || !src || dst_size == 0) return;
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}
}  // namespace

void report_error(const char* subsystem, uint8_t code, const char* message) {
    // No locks needed: this is called from a single task (network task) or
    // from ISR-context callers that serialize via the caller's own lock.
    // If called from multiple tasks, add a spinlock.
    ErrorReport* e = &g_ring.buf[g_ring.head];
    e->uptime_ms = millis();
    safe_copy(e->subsystem, subsystem, sizeof(e->subsystem));
    e->code = code;
    safe_copy(e->message, message, sizeof(e->message));

    g_ring.head = (g_ring.head + 1) % ERROR_REPORTER_MAX_PENDING;
    if (g_ring.count < ERROR_REPORTER_MAX_PENDING) {
        g_ring.count++;
    } else {
        // Buffer full: overwrite oldest (tail advances with head)
        g_ring.tail = (g_ring.tail + 1) % ERROR_REPORTER_MAX_PENDING;
    }
}

uint8_t pending_error_count() {
    return g_ring.count;
}

bool pop_pending_error(ErrorReport* out) {
    if (g_ring.count == 0 || !out) return false;
    *out = g_ring.buf[g_ring.tail];
    g_ring.tail = (g_ring.tail + 1) % ERROR_REPORTER_MAX_PENDING;
    g_ring.count--;
    return true;
}

void clear_pending_errors() {
    g_ring.head = 0;
    g_ring.tail = 0;
    g_ring.count = 0;
}
