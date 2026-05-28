#include "data_logger.h"
#include "config.h"
#include <string.h>
#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <SPIFFS.h>

enum EntryType : uint8_t { ENTRY_BASE = 0xB0, ENTRY_DELTA = 0xD0 };

struct BaseEntry {
    uint8_t  type;
    uint32_t timestamp_ms;
    int16_t  v[4];
    int16_t  i[4];
    int16_t  p[4];
} __attribute__((packed));

struct DeltaEntry {
    uint8_t  type;
    uint16_t dt_ms;
    int16_t  dv[4];
    int16_t  di[4];
    int16_t  dp[4];
} __attribute__((packed));

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

static inline size_t next_pos(size_t pos, size_t n) {
    size_t np = pos + n;
    if (np >= LOG_BUFFER_BYTES) np -= LOG_BUFFER_BYTES;
    return np;
}

static inline size_t free_space() {
    if (head >= tail) return LOG_BUFFER_BYTES - (head - tail) - 1;
    return tail - head - 1;
}

static void write_bytes(const uint8_t* src, size_t n) {
    size_t avail = (head >= tail) ? LOG_BUFFER_BYTES - (head - tail) - 1 : tail - head - 1;
    if (n > avail) n = avail;
    size_t first = (head + n >= LOG_BUFFER_BYTES) ? LOG_BUFFER_BYTES - head : n;
    memcpy(buffer + head, src, first);
    head = (head + first) % LOG_BUFFER_BYTES;
    if (n > first) {
        memcpy(buffer + head, src + first, n - first);
        head = (head + n - first) % LOG_BUFFER_BYTES;
    }
}

static bool can_fit(size_t n) {
    if (n >= LOG_BUFFER_BYTES) return false;
    return free_space() > n;
}

void init_data_logger() {
    memset(buffer, 0, sizeof(buffer));
    head = tail = 0;
    entry_count = 0;
    have_base = false;
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS init failed");
    } else {
        size_t total = SPIFFS.totalBytes();
        size_t used = SPIFFS.usedBytes();
        Serial.printf("SPIFFS ready: %u/%u bytes used (%.1f%% free)\n",
                      used, total, (total - used) * 100.0f / total);
        if (SPIFFS.exists(LOG_SPIFFS_FILE)) {
            File f = SPIFFS.open(LOG_SPIFFS_FILE, FILE_READ);
            if (f) {
                Serial.printf("SPIFFS overflow file exists: %u bytes\n", f.size());
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
        (int16_t)(data.ina3221_busV[0] * data.ina3221_current[0] * 1000),
        (int16_t)(data.ina3221_busV[1] * data.ina3221_current[1] * 1000),
        (int16_t)(data.ina3221_busV[2] * data.ina3221_current[2] * 1000),
        (int16_t)(data.ina226_power * 1000)
    };

    if (!have_base) {
        BaseEntry e = { ENTRY_BASE, timestamp_ms };
        memcpy(e.v, v, sizeof(v)); memcpy(e.i, i, sizeof(i)); memcpy(e.p, p, sizeof(p));
        if (can_fit(sizeof(e))) { write_bytes((uint8_t*)&e, sizeof(e)); entry_count++; }
        have_base = true;
    } else {
        int16_t dv[4], di[4], dp[4];
        bool overflow = false;
        for (int ch = 0; ch < 4; ch++) {
            dv[ch] = v[ch] - last_v[ch]; di[ch] = i[ch] - last_i[ch]; dp[ch] = p[ch] - last_p[ch];
            if (abs(dv[ch]) > LOG_MAX_DELTA_MV || abs(di[ch]) > LOG_MAX_DELTA_MA || abs(dp[ch]) > LOG_MAX_DELTA_MW)
                overflow = true;
        }
        uint16_t dt = (uint16_t)(timestamp_ms - last_ts);
        if (overflow || dt > 60000) {
            BaseEntry e = { ENTRY_BASE, timestamp_ms };
            memcpy(e.v, v, sizeof(v)); memcpy(e.i, i, sizeof(i)); memcpy(e.p, p, sizeof(p));
            if (can_fit(sizeof(e))) { write_bytes((uint8_t*)&e, sizeof(e)); entry_count++; }
        } else {
            DeltaEntry e = { ENTRY_DELTA, dt };
            memcpy(e.dv, dv, sizeof(dv)); memcpy(e.di, di, sizeof(di)); memcpy(e.dp, dp, sizeof(dp));
            if (can_fit(sizeof(e))) { write_bytes((uint8_t*)&e, sizeof(e)); entry_count++; }
        }
    }

    memcpy(last_v, v, sizeof(v)); memcpy(last_i, i, sizeof(i)); memcpy(last_p, p, sizeof(p));
    last_ts = timestamp_ms;

    // Fallback: if buffer full and network likely down, write to SPIFFS
    if (!can_fit(sizeof(DeltaEntry))) {
        if (WiFi.status() != WL_CONNECTED) {
            size_t spiffs_total = SPIFFS.totalBytes();
            size_t spiffs_used  = SPIFFS.usedBytes();
            if (spiffs_used >= spiffs_total || (spiffs_total - spiffs_used) < 4096) {
                Serial.println("[SPIFFS] full, dropping oldest batch");
                tail = head;
                entry_count = 0;
            } else {
                File f = SPIFFS.open(LOG_SPIFFS_FILE, FILE_APPEND);
                if (f) {
                    bool ok = true;
                    size_t written = 0;
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
                    f.close();
                    if (ok) {
                        tail = head;
                        entry_count = 0;
                    } else {
                        Serial.printf("[SPIFFS] partial write %u bytes, keeping buffer\n", written);
                    }
                } else {
                    Serial.println("[SPIFFS] open failed for log append");
                }
            }
        }
    }
}

size_t log_pop_batch(uint8_t* out_buf, size_t out_len) {
    size_t written = 0;
    while (written + sizeof(DeltaEntry) <= out_len && tail != head) {
        uint8_t type = buffer[tail];
        size_t entry_size = (type == ENTRY_BASE) ? sizeof(BaseEntry) : sizeof(DeltaEntry);
        if (written + entry_size > out_len) break;
        for (size_t i = 0; i < entry_size; i++) {
            out_buf[written++] = buffer[tail];
            tail = next_pos(tail, 1);
        }
        entry_count--;
    }
    return written;
}

bool log_peek_latest(LogSnapshot* out) {
    if (!have_base) return false;
    out->timestamp_s = epoch_offset + last_ts / 1000;
    for (int ch = 0; ch < 4; ch++) {
        out->voltage[ch] = last_v[ch] / 1000.0f;
        out->current[ch] = last_i[ch] / 1000.0f;
        out->power[ch]   = last_p[ch] / 1000.0f;
    }
    return true;
}

uint32_t log_entries_count() { return entry_count; }
bool log_is_full() { return free_space() < sizeof(DeltaEntry); }

bool log_has_overflow_file() { return SPIFFS.exists(LOG_SPIFFS_FILE); }
size_t log_overflow_file_size() {
    File f = SPIFFS.open(LOG_SPIFFS_FILE, FILE_READ);
    size_t s = f ? f.size() : 0;
    if (f) f.close();
    return s;
}

static File g_overflow_file;

bool log_open_overflow_for_read() {
    if (!SPIFFS.exists(LOG_SPIFFS_FILE)) return false;
    g_overflow_file = SPIFFS.open(LOG_SPIFFS_FILE, FILE_READ);
    return g_overflow_file;
}

size_t log_read_overflow_chunk(uint8_t* buf, size_t len) {
    if (!g_overflow_file) return 0;
    return g_overflow_file.read(buf, len);
}

void log_close_overflow() {
    if (g_overflow_file) {
        g_overflow_file.close();
        SPIFFS.remove(LOG_SPIFFS_FILE);
        g_overflow_file = File();
    }
}
