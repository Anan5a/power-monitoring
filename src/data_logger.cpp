#include "data_logger.h"
#include "config.h"
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

void log_set_epoch(time_t epoch) {
    epoch_offset = epoch - millis() / 1000;
    Serial.printf("Log: epoch offset set, time %s", ctime(&epoch));
}

time_t log_to_epoch(uint32_t timestamp_ms) {
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

// ── Flush ring buffer to LittleFS overflow file ─────────────────────
static void flush_to_littlefs() {
    if (tail == head) return;
    File f = LittleFS.open(LOG_OVERFLOW_FILE, FILE_APPEND);
    if (!f) {
        Serial.println("[LittleFS] open failed for log append");
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
        Serial.printf("[LittleFS] flushed %u bytes\n", written);
    }
}

// ── Public API ──────────────────────────────────────────────────────

void init_data_logger() {
    memset(buffer, 0, sizeof(buffer));
    head = tail = 0;
    entry_count = 0;
    have_base = false;
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS init failed");
    } else {
        size_t total = LittleFS.totalBytes();
        size_t used = LittleFS.usedBytes();
        Serial.printf("LittleFS ready: %u/%u bytes used (%.1f%% free)\n",
                      used, total, (total - used) * 100.0f / total);
    }
}

void log_sample(const SensorData& data, uint32_t timestamp_ms) {
    int16_t v[4] = {
        (int16_t)(data.ina3221_busV[0] * 1000),
        (int16_t)(data.ina3221_busV[1] * 1000),
        (int16_t)(data.ina3221_busV[2] * 1000),
        (int16_t)(data.ina226_busV * 1000)
    };
    int16_t i[4] = {
        (int16_t)(data.ina3221_current[0] * 1000),
        (int16_t)(data.ina3221_current[1] * 1000),
        (int16_t)(data.ina3221_current[2] * 1000),
        (int16_t)(data.ina226_current * 1000)
    };
    int16_t p[4] = {
        (int16_t)(data.ina3221_busV[0] * data.ina3221_current[0]),
        (int16_t)(data.ina3221_busV[1] * data.ina3221_current[1]),
        (int16_t)(data.ina3221_busV[2] * data.ina3221_current[2]),
        (int16_t)(data.ina226_power)
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
        // Buffer full — flush to LittleFS, only drop if flash itself is full
        size_t fs_total = LittleFS.totalBytes();
        size_t fs_used  = LittleFS.usedBytes();
        if (fs_used >= fs_total || (fs_total - fs_used) < 4096) {
            Serial.println("[LOG] flash full, dropping oldest entries");
            tail = head;
            entry_count = 0;
        } else {
            flush_to_littlefs();
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
    size_t n = pop_ring(out_buf, out_len);
    // If ring buffer empty and overflow file exists, drain from LittleFS next
    return n;
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
    File f = LittleFS.open(LOG_OVERFLOW_FILE, FILE_READ);
    size_t s = f ? f.size() : 0;
    if (f) f.close();
    return s;
}

static File g_overflow_file;

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