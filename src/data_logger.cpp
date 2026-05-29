#include "data_logger.h"
#include "config.h"
#include <string.h>
#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <LittleFS.h>

// ── Tier 1: static 16KB primary ring buffer (guaranteed) ──────────
static uint8_t buffer[LOG_BUFFER_BYTES];
static size_t head = 0, tail = 0;
static uint32_t entry_count = 0;

// ── Tier 2: dynamically allocated extension (created when T1 fills) ─
static uint8_t* ext_buffer = nullptr;
static size_t   ext_size = 0;
static size_t   ext_head = 0, ext_tail = 0;
static uint32_t ext_count = 0;

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

// ── Generic ring buffer helpers (size param for buffer length) ────
static inline size_t next_pos(size_t pos, size_t n, size_t sz) {
    size_t np = pos + n;
    if (np >= sz) np -= sz;
    return np;
}

static inline size_t free_space(size_t h, size_t t, size_t sz) {
    if (h >= t) return sz - (h - t) - 1;
    return t - h - 1;
}

// ── Per-buffer write ───────────────────────────────────────────────
static void write_t1(const uint8_t* src, size_t n) {
    size_t avail = free_space(head, tail, LOG_BUFFER_BYTES);
    if (n > avail) n = avail;
    size_t first = (head + n >= LOG_BUFFER_BYTES) ? LOG_BUFFER_BYTES - head : n;
    memcpy(buffer + head, src, first);
    head = (head + first) % LOG_BUFFER_BYTES;
    if (n > first) {
        memcpy(buffer + head, src + first, n - first);
        head = (head + n - first) % LOG_BUFFER_BYTES;
    }
}

static void write_t2(const uint8_t* src, size_t n) {
    if (!ext_buffer || !ext_size) return;
    size_t avail = free_space(ext_head, ext_tail, ext_size);
    if (n > avail) n = avail;
    size_t first = (ext_head + n >= ext_size) ? ext_size - ext_head : n;
    memcpy(ext_buffer + ext_head, src, first);
    ext_head = (ext_head + first) % ext_size;
    if (n > first) {
        memcpy(ext_buffer + ext_head, src + first, n - first);
        ext_head = (ext_head + n - first) % ext_size;
    }
}

static bool can_fit_t1(size_t n) {
    if (n >= LOG_BUFFER_BYTES) return false;
    return free_space(head, tail, LOG_BUFFER_BYTES) > n;
}

static bool can_fit_t2(size_t n) {
    if (!ext_buffer || !ext_size) return false;
    if (n >= ext_size) return false;
    return free_space(ext_head, ext_tail, ext_size) > n;
}

static uint32_t count_entries(const uint8_t* buf, size_t t, size_t h, size_t sz) {
    uint32_t count = 0;
    size_t pos = t;
    while (pos != h) {
        uint8_t type = buf[pos];
        if (type != ENTRY_BASE && type != ENTRY_DELTA) break; // garbage or partial entry
        size_t es = (type == ENTRY_BASE) ? sizeof(BaseEntry) : sizeof(DeltaEntry);
        size_t dist = (h >= pos) ? (h - pos) : (sz - pos + h);
        if (dist < es) break;
        pos = next_pos(pos, es, sz);
        count++;
    }
    return count;
}

static size_t pop_ring(uint8_t* buf, size_t* t, size_t* h, size_t sz,
                       uint32_t* cnt, uint8_t* out, size_t out_len) {
    size_t written = 0;
    while (written + sizeof(DeltaEntry) <= out_len && *t != *h) {
        uint8_t type = buf[*t];
        size_t entry_size = (type == ENTRY_BASE) ? sizeof(BaseEntry) : sizeof(DeltaEntry);
        if (written + entry_size > out_len) break;
        for (size_t i = 0; i < entry_size; i++) {
            out[written++] = buf[*t];
            *t = next_pos(*t, 1, sz);
        }
        if (*cnt) (*cnt)--;
    }
    return written;
}

static void flush_t1_to_t2() {
    if (!ext_buffer || !ext_size || head == tail) return;
    size_t n = (head >= tail) ? (head - tail) : (LOG_BUFFER_BYTES - tail + head);
    if (n == 0) return;
    size_t avail = free_space(ext_head, ext_tail, ext_size);
    if (n > avail) {
        Serial.printf("[LOG] T2 full, dropping %u bytes from T1\n", n - avail);
        tail = next_pos(tail, n - avail, LOG_BUFFER_BYTES);
        n = avail;
    }
    // Byte-by-byte copy is safe regardless of wrap-around state
    size_t src = tail;
    size_t dst = ext_head;
    for (size_t i = 0; i < n; i++) {
        ext_buffer[dst] = buffer[src];
        src = next_pos(src, 1, LOG_BUFFER_BYTES);
        dst = next_pos(dst, 1, ext_size);
    }
    ext_head = dst;
    uint32_t moved = count_entries(buffer, tail, head, LOG_BUFFER_BYTES);
    ext_count += moved;
    tail = head;
    entry_count = 0;
}

static void create_ext_buffer() {
    if (ext_buffer) return;
    const size_t try_sizes[] = {48*1024, 32*1024, 16*1024};
    for (size_t sz : try_sizes) {
        ext_buffer = (uint8_t*)malloc(sz);
        if (ext_buffer) {
            ext_size = sz;
            ext_head = ext_tail = 0;
            ext_count = 0;
            Serial.printf("[LOG] T2 allocated %u bytes\n", ext_size);
            return;
        }
    }
    Serial.println("[LOG] T2 allocation failed");
}

static void destroy_ext_buffer() {
    if (ext_buffer) {
        free(ext_buffer);
        ext_buffer = nullptr;
        ext_size = 0;
        ext_head = ext_tail = 0;
        ext_count = 0;
    }
}

static void flush_ram_to_littlefs() {
    // Write T2 first (older data), then T1 (newer data)
    File f = LittleFS.open(LOG_OVERFLOW_FILE, FILE_APPEND);
    if (!f) {
        Serial.println("[LittleFS] open failed for log append");
        return;
    }
    bool ok = true;
    size_t written = 0;

    // T2
    if (ext_buffer && ext_head != ext_tail) {
        if (ext_head >= ext_tail) {
            size_t n = ext_head - ext_tail;
            if (n > 0) {
                size_t w = f.write(ext_buffer + ext_tail, n);
                if (w != n) ok = false;
                written += w;
            }
        } else {
            size_t n1 = ext_size - ext_tail;
            if (n1 > 0) {
                size_t w1 = f.write(ext_buffer + ext_tail, n1);
                if (w1 != n1) ok = false;
                written += w1;
                if (ok && ext_head > 0) {
                    size_t w2 = f.write(ext_buffer, ext_head);
                    if (w2 != ext_head) ok = false;
                    written += w2;
                }
            }
        }
    }

    // T1
    if (ok && head != tail) {
        if (head >= tail) {
            size_t n = head - tail;
            if (n > 0) {
                size_t w = f.write(buffer + tail, n);
                if (w != n) ok = false;
                written += w;
            }
        } else {
            size_t n1 = LOG_BUFFER_BYTES - tail;
            if (n1 > 0) {
                size_t w1 = f.write(buffer + tail, n1);
                if (w1 != n1) ok = false;
                written += w1;
                if (ok && head > 0) {
                    size_t w2 = f.write(buffer, head);
                    if (w2 != head) ok = false;
                    written += w2;
                }
            }
        }
    }

    f.close();
    if (ok) {
        tail = head;
        entry_count = 0;
        destroy_ext_buffer();
        Serial.printf("[LittleFS] flushed %u bytes\n", written);
    } else {
        Serial.printf("[LittleFS] partial write %u bytes, keeping RAM buffers\n", written);
    }
}

// ── Public API ────────────────────────────────────────────────────

void init_data_logger() {
    memset(buffer, 0, sizeof(buffer));
    head = tail = 0;
    entry_count = 0;
    have_base = false;
    destroy_ext_buffer();
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS init failed");
    } else {
        size_t total = LittleFS.totalBytes();
        size_t used = LittleFS.usedBytes();
        Serial.printf("LittleFS ready: %u/%u bytes used (%.1f%% free)\n",
                      used, total, (total - used) * 100.0f / total);
        if (LittleFS.exists(LOG_OVERFLOW_FILE)) {
            File f = LittleFS.open(LOG_OVERFLOW_FILE, FILE_READ);
            if (f) {
                Serial.printf("LittleFS overflow file exists: %u bytes\n", f.size());
                f.close();
            }
        }
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

    // Compute entry before touching last_* state
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

    // Update delta state regardless of write success (same as original)
    memcpy(last_v, v, sizeof(v)); memcpy(last_i, i, sizeof(i)); memcpy(last_p, p, sizeof(p));
    last_ts = timestamp_ms;

    uint8_t* entry_ptr = is_base ? (uint8_t*)&base_e : (uint8_t*)&delta_e;

    // Try T1 first
    bool wrote = false;
    if (can_fit_t1(entry_size)) {
        write_t1(entry_ptr, entry_size); entry_count++; wrote = true;
    } else {
        // T1 full — flush it to T2 so T1 always holds the newest data
        if (!ext_buffer) create_ext_buffer();
        flush_t1_to_t2();
        if (can_fit_t1(entry_size)) {
            write_t1(entry_ptr, entry_size); entry_count++; wrote = true;
        } else if (can_fit_t2(entry_size)) {
            write_t2(entry_ptr, entry_size); ext_count++; wrote = true;
        }
    }

    if (!wrote && WiFi.status() != WL_CONNECTED) {
        // Both T1 and T2 full — flush everything to LittleFS
        size_t fs_total = LittleFS.totalBytes();
        size_t fs_used  = LittleFS.usedBytes();
        if (fs_used >= fs_total || (fs_total - fs_used) < 4096) {
            Serial.println("[LittleFS] full, dropping oldest batch");
            tail = head;
            entry_count = 0;
            destroy_ext_buffer();
        } else {
            flush_ram_to_littlefs();
            // After flush, try T1 again
            if (can_fit_t1(entry_size)) {
                write_t1(entry_ptr, entry_size); entry_count++; wrote = true;
            }
        }
    }
}

size_t log_pop_batch(uint8_t* out_buf, size_t out_len) {
    // Drain T2 first (older data), then T1 (newer data)
    size_t n = 0;
    if (ext_buffer && ext_head != ext_tail) {
        n = pop_ring(ext_buffer, &ext_tail, &ext_head, ext_size, &ext_count, out_buf, out_len);
    }
    if (n < out_len && tail != head) {
        n += pop_ring(buffer, &tail, &head, LOG_BUFFER_BYTES, &entry_count, out_buf + n, out_len - n);
    }
    // If T2 emptied, free it to save heap
    if (ext_buffer && ext_head == ext_tail && ext_count == 0) {
        destroy_ext_buffer();
    }
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

uint32_t log_entries_count() { return entry_count + ext_count; }
bool log_is_full() { return !can_fit_t1(sizeof(DeltaEntry)) && !can_fit_t2(sizeof(DeltaEntry)); }
size_t log_buffer_capacity() { return LOG_BUFFER_BYTES + ext_size; }

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
