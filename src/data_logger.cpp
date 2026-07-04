#include "data_logger.h"
#include "config.h"
#include "log_serial.h"
#include <string.h>
#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>

// ── Single static 16KB ring buffer (no dynamic allocation) ─────────
static uint8_t buffer[LOG_BUFFER_BYTES];
static size_t head = 0, tail = 0;
static uint32_t entry_count = 0;

static int16_t last_v[4] = {}, last_i[4] = {}, last_p[4] = {};
static uint32_t last_ts = 0;
static bool have_base = false;
static time_t epoch_offset = 0;
// True once log_set_epoch() has been called with a trusted post-2023 value
// (i.e. NTP has synced). Until then log_to_epoch() returns uptime-derived
// seconds and downstream consumers should treat timestamps as untrusted.
static bool g_log_epoch_valid = false;
// One-shot warning guard so we don't spam the serial console.
static bool g_log_untrusted_warned = false;

void log_set_epoch(time_t epoch) {
    epoch_offset = epoch - millis() / 1000;
    if (epoch > 1700000000) {
        g_log_epoch_valid = true;
        LOG_PRINT("Log: epoch offset set, time %s", ctime(&epoch));
    } else {
        // Caller passed an untrusted value (boot pre-NTP). Leave the flag
        // false and let the next valid sync set it.
        LOG_PRINT("Log: epoch offset set to UNTRUSTED value %lld — log timestamps will drift\n", (long long)epoch);
    }
}

bool log_epoch_valid() { return g_log_epoch_valid; }
void log_epoch_valid_set(bool valid) { g_log_epoch_valid = valid; }

time_t log_to_epoch(uint32_t timestamp_ms) {
    // Gate on epoch validity. Until log_set_epoch() has been called with a
    // trusted (post-2023) value the math is uptime-derived and will drift
    // from real wall-clock. Return -1 as a sentinel so callers can detect
    // "untrusted" and stamp 0 (or skip) instead of writing a bogus epoch.
    if (!g_log_epoch_valid) {
        return (time_t)-1;
    }
    return epoch_offset + timestamp_ms / 1000;
}

// ── Ring buffer helpers ─────────────────────────────────────────────
static inline size_t next_pos(size_t pos, size_t n, size_t sz) {
    size_t np = pos + n;
    if (np >= sz) np -= sz;
    return np;
}

static inline size_t free_space(size_t h, size_t t, size_t sz) {
    if (h >= t) return sz - (h - t) - 1;
    return t - h - 1;
}

static inline bool can_fit(size_t n) {
    if (n >= LOG_BUFFER_BYTES) return false;
    return free_space(head, tail, LOG_BUFFER_BYTES) > n;
}

static size_t pop_ring(uint8_t* out, size_t out_len) {
    size_t written = 0;
    while (written + sizeof(DeltaEntry) <= out_len && tail != head) {
        uint8_t type = buffer[tail];
        size_t es = (type == ENTRY_BASE) ? sizeof(BaseEntry) : sizeof(DeltaEntry);
        if (written + es > out_len) break;
        for (size_t i = 0; i < es; i++) {
            out[written++] = buffer[tail];
            tail = next_pos(tail, 1, LOG_BUFFER_BYTES);
        }
        if (entry_count) entry_count--;
    }
    return written;
}

static File g_overflow_file;

// ── Flush ring buffer to LittleFS overflow file ─────────────────────
// 1 MB cap on the overflow file. LittleFS doesn't support truncating-from-
// middle, so the simplest correct safety cap is to close and remove the
// file when it reaches the limit. The data is lost; this prevents the
// flash from filling up and bricking the device on a long network outage.
static const size_t LOG_OVERFLOW_MAX_BYTES = 1UL * 1024UL * 1024UL;

static void flush_to_littlefs() {
    if (tail == head) return;
    // Close read handle first — LittleFS can't have same file open twice
    if (g_overflow_file) {
        g_overflow_file.close();
        g_overflow_file = File();
    }

    // Cap check: if the overflow file is at or above the limit, drop it
    // entirely and start fresh. The data currently in the ring buffer is
    // also lost (the next iteration will re-flush, but we already chose
    // to drop history).
    if (LittleFS.exists(LOG_OVERFLOW_FILE)) {
        File existing = LittleFS.open(LOG_OVERFLOW_FILE, FILE_READ);
        if (existing) {
            size_t sz = existing.size();
            existing.close();
            if (sz >= LOG_OVERFLOW_MAX_BYTES) {
                LOG_PRINT("[LittleFS] overflow file at %u bytes (cap %u) — wiping\n",
                          (unsigned)sz, (unsigned)LOG_OVERFLOW_MAX_BYTES);
                LittleFS.remove(LOG_OVERFLOW_FILE);
            }
        }
    }

    File f = LittleFS.open(LOG_OVERFLOW_FILE, FILE_APPEND);
    if (!f) {
        LOG_PRINTLN("[LittleFS] open failed for log append");
        return;
    }
    size_t written = 0;
    if (head >= tail) {
        size_t n = head - tail;
        written = f.write(buffer + tail, n);
    } else {
        size_t n1 = LOG_BUFFER_BYTES - tail;
        written = f.write(buffer + tail, n1);
        if (written == n1 && head > 0) {
            written += f.write(buffer, head);
        }
    }
    f.close();
    if (written > 0) {
        tail = head;
        entry_count = 0;
        LOG_PRINT("[LittleFS] flushed %u bytes\n", written);
    }
}

// ── Public API ──────────────────────────────────────────────────────

void init_data_logger() {
    memset(buffer, 0, sizeof(buffer));
    head = tail = 0;
    entry_count = 0;
    have_base = false;
    if (!LittleFS.begin(true)) {
        LOG_PRINTLN("LittleFS init failed");
    } else {
        size_t total = LittleFS.totalBytes();
        size_t used = LittleFS.usedBytes();
        LOG_PRINT("LittleFS ready: %u/%u bytes used (%.1f%% free)\n",
                      used, total, (total - used) * 100.0f / total);
    }
    // One-shot warning if NTP hasn't synced before log_sample() starts writing.
    // The check is deferred to the first log_sample() call so that any boot-time
    // log_set_epoch() (from setup() with get_epoch_time()) gets a chance to flip
    // g_log_epoch_valid first. See log_sample() below.
}

void log_sample(const SensorSnapshot& data, uint32_t timestamp_ms) {
    // One-shot warning: if NTP has not synced, log timestamps are uptime-based
    // and will drift from real time. Mark this once per boot so the operator
    // can decide to fix the network or NTP config. The timestamp math itself
    // is intentionally unchanged — we just flag the result.
    if (!g_log_epoch_valid && !g_log_untrusted_warned) {
        LOG_PRINTLN("[WARN] log timestamps are based on uptime (NTP not yet synced) — will drift from real time");
        g_log_untrusted_warned = true;
    }
    int16_t v[4] = {
        (int16_t)(get_channel_voltage(data, 0) * 1000),
        (int16_t)(get_channel_voltage(data, 1) * 1000),
        (int16_t)(get_channel_voltage(data, 2) * 1000),
        (int16_t)(get_channel_voltage(data, 3) * 1000)
    };
    int16_t i[4] = {
        (int16_t)(get_channel_current(data, 0) * 1000),
        (int16_t)(get_channel_current(data, 1) * 1000),
        (int16_t)(get_channel_current(data, 2) * 1000),
        (int16_t)(get_channel_current(data, 3) * 1000)
    };
    int16_t p[4] = {
        (int16_t)(get_channel_power(data, 0)),
        (int16_t)(get_channel_power(data, 1)),
        (int16_t)(get_channel_power(data, 2)),
        (int16_t)(get_channel_power(data, 3))
    };

    bool is_base;
    BaseEntry base_e;
    DeltaEntry delta_e;
    size_t entry_size;

    if (!have_base) {
        base_e = { ENTRY_BASE, timestamp_ms };
        memcpy(base_e.v, v, sizeof(v)); memcpy(base_e.i, i, sizeof(i)); memcpy(base_e.p, p, sizeof(p));
        is_base = true;
        entry_size = sizeof(BaseEntry);
        have_base = true;
    } else {
        int16_t dv[4], di[4], dp[4];
        bool overflow = false;
        for (int ch = 0; ch < 4; ch++) {
            dv[ch] = v[ch] - last_v[ch]; di[ch] = i[ch] - last_i[ch]; dp[ch] = p[ch] - last_p[ch];
            if (abs(dv[ch]) > LOG_MAX_DELTA_MV || abs(di[ch]) > LOG_MAX_DELTA_MA || abs(dp[ch]) > LOG_MAX_DELTA_POWER)
                overflow = true;
        }
        uint16_t dt = (uint16_t)(timestamp_ms - last_ts);
        if (overflow || dt > 60000) {
            base_e = { ENTRY_BASE, timestamp_ms };
            memcpy(base_e.v, v, sizeof(v)); memcpy(base_e.i, i, sizeof(i)); memcpy(base_e.p, p, sizeof(p));
            is_base = true;
            entry_size = sizeof(BaseEntry);
        } else {
            delta_e = { ENTRY_DELTA, dt };
            memcpy(delta_e.dv, dv, sizeof(dv)); memcpy(delta_e.di, di, sizeof(di)); memcpy(delta_e.dp, dp, sizeof(dp));
            is_base = false;
            entry_size = sizeof(DeltaEntry);
        }
    }

    memcpy(last_v, v, sizeof(v)); memcpy(last_i, i, sizeof(i)); memcpy(last_p, p, sizeof(p));
    last_ts = timestamp_ms;

    uint8_t* entry_ptr = is_base ? (uint8_t*)&base_e : (uint8_t*)&delta_e;

    if (can_fit(entry_size)) {
        size_t avail = free_space(head, tail, LOG_BUFFER_BYTES);
        if (entry_size > avail) entry_size = avail;
        size_t first = (head + entry_size >= LOG_BUFFER_BYTES) ? LOG_BUFFER_BYTES - head : entry_size;
        memcpy(buffer + head, entry_ptr, first);
        head = (head + first) % LOG_BUFFER_BYTES;
        if (entry_size > first) {
            memcpy(buffer + head, entry_ptr + first, entry_size - first);
            head = (head + entry_size - first) % LOG_BUFFER_BYTES;
        }
        entry_count++;
    } else {
        // Buffer full — only write to LittleFS if network is down.
        // When WiFi is connected, entries go to RAM and get published directly.
        bool network_down = (WiFi.status() != WL_CONNECTED);
        size_t fs_total = LittleFS.totalBytes();
        size_t fs_used  = LittleFS.usedBytes();
        if (fs_used >= fs_total || (fs_total - fs_used) < 4096) {
            LOG_PRINTLN("[LOG] flash full, dropping oldest entries");
            tail = head;
            entry_count = 0;
        } else if (network_down) {
            flush_to_littlefs();
        } else {
            // Network up — just drop oldest entries, they get published from RAM
            LOG_PRINTLN("[LOG] buffer full but network up, dropping oldest");
            tail = head;
            entry_count = 0;
        }
        // After flush (or drop), retry write
        if (can_fit(entry_size)) {
            size_t first = (head + entry_size >= LOG_BUFFER_BYTES) ? LOG_BUFFER_BYTES - head : entry_size;
            memcpy(buffer + head, entry_ptr, first);
            head = (head + first) % LOG_BUFFER_BYTES;
            if (entry_size > first) {
                memcpy(buffer + head, entry_ptr + first, entry_size - first);
                head = (head + entry_size - first) % LOG_BUFFER_BYTES;
            }
            entry_count++;
        }
    }
}

size_t log_pop_batch(uint8_t* out_buf, size_t out_len) {
    return pop_ring(out_buf, out_len);
}

bool log_peek_latest(LogSnapshot* out) {
    if (!have_base) return false;
    out->timestamp_s = epoch_offset + last_ts / 1000;
    for (int ch = 0; ch < 4; ch++) {
        out->voltage[ch] = last_v[ch] / 1000.0f;
        out->current[ch] = last_i[ch] / 1000.0f;
        out->power[ch]   = last_p[ch] / 1.0f;
    }
    return true;
}

uint32_t log_entries_count() { return entry_count; }
bool log_is_full() { return !can_fit(sizeof(DeltaEntry)); }
size_t log_buffer_capacity() { return LOG_BUFFER_BYTES; }

bool log_has_overflow_file() { return LittleFS.exists(LOG_OVERFLOW_FILE); }
size_t log_overflow_file_size() {
    if (g_overflow_file) {
        size_t s = g_overflow_file.size();
        return s;
    }
    File f = LittleFS.open(LOG_OVERFLOW_FILE, FILE_READ);
    size_t s = f ? f.size() : 0;
    if (f) f.close();
    return s;
}

bool log_open_overflow_for_read() {
    if (!LittleFS.exists(LOG_OVERFLOW_FILE)) return false;
    g_overflow_file = LittleFS.open(LOG_OVERFLOW_FILE, FILE_READ);
    return g_overflow_file;
}

size_t log_read_overflow_chunk(uint8_t* buf, size_t len) {
    if (!g_overflow_file) return 0;
    return g_overflow_file.read(buf, len);
}

void log_close_overflow() {
    if (g_overflow_file) {
        g_overflow_file.close();
        LittleFS.remove(LOG_OVERFLOW_FILE);
        g_overflow_file = File();
    }
}
