#include "data_logger.h"
#include "config.h"
#include "log_serial.h"
#include "event_log.h"
#include <string.h>
#include <Arduino.h>
#include <WiFi.h>

#if defined(ESP32) || defined(ESP32C3) || defined(ESP32S3)
    #include <freertos/FreeRTOS.h>
    #include <freertos/task.h>
    // Cross-core protection: log_sample() runs on sensorTask (Core 1) and
    // log_pop_batch() runs on networkTask (Core 0). Without a critical
    // section the network task can observe a half-written BaseEntry or
    // DeltaEntry — the type byte (1) at tail might be from a new entry
    // while the body bytes are still from the prior entry. portMUX gives
    // us a tight, non-yielding critical section.
    static portMUX_TYPE g_log_mux = portMUX_INITIALIZER_UNLOCKED;
    #define LOG_LOCK()    taskENTER_CRITICAL(&g_log_mux)
    #define LOG_UNLOCK()  taskEXIT_CRITICAL(&g_log_mux)
#else
    // Host build (sim unit tests / sim binary): single-threaded. No lock.
    #define LOG_LOCK()    do {} while (0)
    #define LOG_UNLOCK()  do {} while (0)
#endif

// ── Single static 16KB ring buffer (no dynamic allocation) ─────────
static uint8_t buffer[LOG_BUFFER_BYTES];
static size_t head = 0, tail = 0;
static uint32_t entry_count = 0;

// High-water mark latch. True once the buffer has crossed the warning
// threshold (see LOG_BUFFER_HIGH_WATER_PCT). Used to fire a one-shot log
// per crossing so a near-overflow condition is visible on the serial
// console. Reset on init_data_logger() and on every drain back below
// the threshold.
static bool g_log_high_water_warned = false;
#define LOG_BUFFER_HIGH_WATER_PCT 80  // warn when >= 80% full

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

// ── SD card (auto-detect) ───────────────────────────────────────────
// SD card is auto-detected on every boot. If present, it serves two
// purposes:
//   1. Binary overflow file (/log_overflow.bin) — when the 16 KB RAM
//      ring fills and the network is down, entries spill here for later
//      drain by the MQTT/Supabase path. Capped at 1 MB.
//   2. Daily CSV logs (/logs/YYYY-MM-DD.csv) — one row per sensor
//      sample, human-readable, for long-term inspection. Oldest CSV
//      files are pruned when free space drops below 5%.
//
// Fault tolerance:
//   - Card presence checked before every write (re-init on failure).
//   - Write errors logged; data silently dropped (no crash).
//   - Card removal mid-write: file handle closed, next write re-inits.
//   - All I/O happens OUTSIDE the LOG_LOCK spinlock so a slow SD write
//     can't block the other core's log_pop_batch or trip the WDT.
#include <SD.h>
#include <SPI.h>

static File g_overflow_file;
static bool g_sd_initialized = false;
static bool g_sd_present = false;
static const size_t LOG_OVERFLOW_MAX_BYTES = 1UL * 1024UL * 1024UL;

// Scratch buffer for deferring SD writes out of the LOG_LOCK spinlock.
static uint8_t g_sd_flush_buf[LOG_BUFFER_BYTES];
static size_t g_pending_sd_flush_len = 0;

// CSV batching: accumulate lines and flush periodically.
#define CSV_LINE_BUF_SIZE 256
static char g_csv_line_buf[CSV_LINE_BUF_SIZE];
static size_t g_csv_line_len = 0;
static uint32_t g_csv_last_flush_ms = 0;
static const uint32_t CSV_FLUSH_INTERVAL_MS = 5000;  // flush every 5 s
static const float SD_LOW_SPACE_PCT = 5.0f;   // prune when < 5% free
static const float SD_TARGET_SPACE_PCT = 10.0f; // target after prune

static bool init_sd() {
    if (g_sd_initialized) return g_sd_present;
    SPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) {
        g_sd_present = false;
    } else {
        uint64_t sz = SD.cardSize() / (1024UL * 1024UL);
        LOG_PRINT("[SD] ready: %llu MB\n", (unsigned long long)sz);
        log_event(EVENT_LOG_INFO, "sd", "ready %llu MB", (unsigned long long)sz);
        g_sd_present = true;
    }
    g_sd_initialized = true;
    return g_sd_present;
}

static void sd_reset() {
    if (g_overflow_file) {
        g_overflow_file.close();
        g_overflow_file = File();
    }
    g_sd_initialized = false;
    g_sd_present = false;
}

// ── Storage management ──────────────────────────────────────────────
// When free space drops below SD_LOW_SPACE_PCT, delete the oldest CSV
// files in /logs/ until free space exceeds SD_TARGET_SPACE_PCT.
static void sd_prune_old_csv() {
    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    if (total == 0) return;
    float free_pct = (float)(total - used) / (float)total * 100.0f;
    if (free_pct >= SD_LOW_SPACE_PCT) return;

    LOG_PRINT("[SD] low space: %.1f%% free — pruning old CSV files\n", free_pct);

    File root = SD.open("/logs");
    if (!root || !root.isDirectory()) {
        if (root) root.close();
        return;
    }

    // Collect CSV filenames (they sort lexicographically = chronologically
    // when named YYYY-MM-DD.csv). We delete oldest first.
    struct CsvFile { char name[32]; size_t size; };
    CsvFile files[64];
    int count = 0;

    File entry = root.openNextFile();
    while (entry && count < 64) {
        if (!entry.isDirectory()) {
            const char* fn = entry.name();
            // Match YYYY-MM-DD.csv pattern
            size_t fnlen = strlen(fn);
            if (fnlen >= 14 && strcmp(fn + fnlen - 4, ".csv") == 0) {
                strncpy(files[count].name, fn, sizeof(files[count].name) - 1);
                files[count].name[sizeof(files[count].name) - 1] = '\0';
                files[count].size = entry.size();
                count++;
            }
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();

    // Sort by name (oldest first — YYYY-MM-DD sorts lexicographically)
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcmp(files[i].name, files[j].name) > 0) {
                CsvFile tmp = files[i];
                files[i] = files[j];
                files[j] = tmp;
            }
        }
    }

    // Delete oldest until above target
    for (int i = 0; i < count; i++) {
        used = SD.usedBytes();
        free_pct = (float)(total - used) / (float)total * 100.0f;
        if (free_pct >= SD_TARGET_SPACE_PCT) break;

        char path[64];
        snprintf(path, sizeof(path), "/logs/%s", files[i].name);
        if (SD.remove(path)) {
            LOG_PRINT("[SD] pruned %s (freed %u bytes)\n", files[i].name, (unsigned)files[i].size);
        }
    }
}

// ── Daily CSV logging ───────────────────────────────────────────────
// Build a CSV row from the current sensor snapshot and append it to
// /logs/YYYY-MM-DD.csv. Creates the file with a header row on first
// write of the day. Rows are batched in g_csv_line_buf and flushed
// every CSV_FLUSH_INTERVAL_MS to reduce write frequency.
static void sd_write_csv_row(const SensorSnapshot& data, uint32_t timestamp_ms) {
    if (!g_sd_present) return;

    // Build the date string for the filename
    time_t now_epoch = (g_log_epoch_valid)
        ? (epoch_offset + timestamp_ms / 1000)
        : 0;
    struct tm* tm_info = localtime(&now_epoch);
    char date_str[16];
    if (g_log_epoch_valid && tm_info) {
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", tm_info);
    } else {
        // No valid epoch — use a fallback name so we don't create
        // "1970-01-01.csv" which would collide with real data later.
        // Use the uptime day number instead.
        snprintf(date_str, sizeof(date_str), "uptime-%lu", timestamp_ms / 86400000UL);
    }

    // Build the CSV row
    float v0 = get_channel_voltage(data, 0);
    float i0 = get_channel_current(data, 0);
    float p0 = get_channel_power(data, 0);
    float v1 = get_channel_voltage(data, 1);
    float i1 = get_channel_current(data, 1);
    float p1 = get_channel_power(data, 1);
    float v2 = get_channel_voltage(data, 2);
    float i2 = get_channel_current(data, 2);
    float p2 = get_channel_power(data, 2);
    float v3 = get_channel_voltage(data, 3);
    float i3 = get_channel_current(data, 3);
    float p3 = get_channel_power(data, 3);

    int row_len = snprintf(g_csv_line_buf + g_csv_line_len,
        sizeof(g_csv_line_buf) - g_csv_line_len,
        "%u,%ld,%.3f,%.3f,%.1f,%.3f,%.3f,%.1f,%.3f,%.3f,%.1f,%.3f,%.3f,%.1f\n",
        timestamp_ms, (long)now_epoch,
        v0, i0, p0, v1, i1, p1, v2, i2, p2, v3, i3, p3);

    if (row_len > 0) {
        g_csv_line_len += row_len;
    }

    // Flush if buffer is full or interval has elapsed
    uint32_t now = millis();
    bool time_to_flush = (now - g_csv_last_flush_ms >= CSV_FLUSH_INTERVAL_MS);
    bool buf_full = (g_csv_line_len > sizeof(g_csv_line_buf) / 2);

    if (!time_to_flush && !buf_full) return;

    // Prune if space is low
    sd_prune_old_csv();

    // Ensure /logs/ directory exists
    if (!SD.exists("/logs")) {
        SD.mkdir("/logs");
    }

    char path[64];
    snprintf(path, sizeof(path), "/logs/%s.csv", date_str);

    // Write header if file is new (size == 0)
    bool needs_header = !SD.exists(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) {
        LOG_PRINTLN("[SD] CSV open failed");
        sd_reset();
        return;
    }
    if (needs_header) {
        f.print("timestamp_ms,epoch,ch0_V,ch0_I,ch0_P,ch1_V,ch1_I,ch1_P,"
                "ch2_V,ch2_I,ch2_P,ch3_V,ch3_I,ch3_P\n");
    }
    f.write((const uint8_t*)g_csv_line_buf, g_csv_line_len);
    f.close();
    g_csv_line_len = 0;
    g_csv_last_flush_ms = now;
}

// ── Binary overflow file ────────────────────────────────────────────
// When the RAM ring fills and the network is down, the ring is copied
// to g_sd_flush_buf under the lock and written here after the lock is
// released. The binary format (BaseEntry/DeltaEntry) is what the
// MQTT/Supabase drain path expects.
static void flush_buffer_to_sd(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;
    if (!init_sd()) return;

    if (g_overflow_file) {
        g_overflow_file.close();
        g_overflow_file = File();
    }

    // Cap check: 1 MB max
    if (SD.exists(LOG_OVERFLOW_FILE)) {
        File f = SD.open(LOG_OVERFLOW_FILE, FILE_READ);
        if (f) {
            size_t sz = f.size();
            f.close();
            if (sz >= LOG_OVERFLOW_MAX_BYTES) {
                LOG_PRINT("[SD] overflow at %u bytes — wiping\n",
                          (unsigned)sz);
                SD.remove(LOG_OVERFLOW_FILE);
            }
        }
    }

    File f = SD.open(LOG_OVERFLOW_FILE, FILE_APPEND);
    if (!f) {
        LOG_PRINTLN("[SD] overflow open failed");
        sd_reset();
        return;
    }
    size_t written = f.write(data, len);
    f.close();
    if (written > 0) {
        LOG_PRINT("[SD] overflow: %u bytes\n", (unsigned)written);
    } else {
        LOG_PRINTLN("[SD] overflow write failed");
        sd_reset();
    }
}

// ── Public API ──────────────────────────────────────────────────────

void init_data_logger() {
    memset(buffer, 0, sizeof(buffer));
    head = tail = 0;
    entry_count = 0;
    have_base = false;
    g_log_high_water_warned = false;
    g_csv_line_len = 0;
    g_csv_last_flush_ms = 0;
    // Auto-detect SD card. If present, we get daily CSV logs + binary
    // overflow backlog. If absent, logger is RAM-only (16 KB ring).
    init_sd();
    if (g_sd_present) {
        LOG_PRINTLN("Data logger: 16 KB ring + SD card (CSV + overflow)");
    } else {
        LOG_PRINTLN("Data logger: RAM-only (16 KB ring, no SD card)");
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

    // Build the entry outside the lock. The struct build is independent
    // of the ring state; the lock only needs to bracket the head/tail/
    // entry_count mutations and the memcpy into the buffer. Keeping the
    // lock window short (and the high-water log outside it) avoids
    // unnecessarily blocking the network task that calls log_pop_batch.
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
        // Compute the full 32-bit delta before the range check. The old code
        // truncated to uint16_t first, so a real gap > 65.5 s (or a millis()
        // wrap) wrapped into a small value, bypassed the 60 s BaseEntry
        // fallback, and silently shifted every later reconstructed timestamp.
        uint32_t dt32 = (uint32_t)(timestamp_ms - last_ts);
        if (overflow || dt32 > 60000) {
            base_e = { ENTRY_BASE, timestamp_ms };
            memcpy(base_e.v, v, sizeof(v)); memcpy(base_e.i, i, sizeof(i)); memcpy(base_e.p, p, sizeof(p));
            is_base = true;
            entry_size = sizeof(BaseEntry);
        } else {
            delta_e = { ENTRY_DELTA, (uint16_t)dt32 };
            memcpy(delta_e.dv, dv, sizeof(dv)); memcpy(delta_e.di, di, sizeof(di)); memcpy(delta_e.dp, dp, sizeof(dp));
            is_base = false;
            entry_size = sizeof(DeltaEntry);
        }
    }

    memcpy(last_v, v, sizeof(v)); memcpy(last_i, i, sizeof(i)); memcpy(last_p, p, sizeof(p));
    last_ts = timestamp_ms;

    uint8_t* entry_ptr = is_base ? (uint8_t*)&base_e : (uint8_t*)&delta_e;

    // Snapshot occupancy before taking the lock so the high-water log
    // can run outside it. We log only on the rising edge across
    // LOG_BUFFER_HIGH_WATER_PCT and clear the latch on drain so a
    // sustained full buffer prints again on the next recovery cycle.
    bool crossed_high_water = false;
    size_t used_before;
    LOG_LOCK();
    used_before = (head >= tail)
                      ? (head - tail)
                      : (LOG_BUFFER_BYTES - tail + head);
    if (!g_log_high_water_warned &&
        used_before * 100u >= (size_t)LOG_BUFFER_HIGH_WATER_PCT * LOG_BUFFER_BYTES) {
        g_log_high_water_warned = true;
        crossed_high_water = true;
    }

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
        // Buffer full. If the network is down, flush the ring to SD card
        // (if available) so entries survive until connectivity returns.
        // If the network is up, entries are published from RAM and we just
        // drop the oldest to make room.
        bool network_down = (WiFi.status() != WL_CONNECTED);
        size_t flush_len = 0;

        if (network_down && init_sd()) {
            // Copy the ring into the scratch buffer and advance tail/head
            // so the ring is empty and ready for the new entry. The actual
            // SD write happens after LOG_UNLOCK().
            if (head >= tail) {
                flush_len = head - tail;
                memcpy(g_sd_flush_buf, buffer + tail, flush_len);
            } else {
                size_t n1 = LOG_BUFFER_BYTES - tail;
                memcpy(g_sd_flush_buf, buffer + tail, n1);
                if (head > 0) memcpy(g_sd_flush_buf + n1, buffer, head);
                flush_len = n1 + head;
            }
            tail = head;
            entry_count = 0;
        } else {
            // Network up (or no SD) — drop oldest; entries are published
            // from RAM.
            LOG_PRINTLN("[LOG] buffer full, dropping oldest entries");
            tail = head;
            entry_count = 0;
        }

        // Retry the write of the current entry now that the ring has space.
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
        g_pending_sd_flush_len = flush_len;
    }
    LOG_UNLOCK();

    // SD writes happen here, outside the spinlock.
    if (g_pending_sd_flush_len > 0) {
        size_t n = g_pending_sd_flush_len;
        g_pending_sd_flush_len = 0;
        flush_buffer_to_sd(g_sd_flush_buf, n);
    }
    // Write a CSV row to the daily log file (batched, flushed every 5 s).
    // This runs on every tick so the CSV is always current, but the actual
    // SD write is rate-limited by the CSV flush interval.
    if (g_sd_present) {
        sd_write_csv_row(data, timestamp_ms);
    }

    if (crossed_high_water) {
        LOG_PRINT("[LOG] buffer occupancy crossed %d%% (%u/%u bytes) — "
                  "drain rate may be insufficient\n",
                  LOG_BUFFER_HIGH_WATER_PCT,
                  (unsigned)used_before,
                  (unsigned)LOG_BUFFER_BYTES);
    }
}

size_t log_pop_batch(uint8_t* out_buf, size_t out_len) {
    LOG_LOCK();
    size_t n = pop_ring(out_buf, out_len);
    // If a successful drain brought us back below the high-water mark,
    // re-arm the warning latch so a subsequent fill prints a fresh
    // message rather than staying silent.
    if (g_log_high_water_warned) {
        size_t used = (head >= tail)
                          ? (head - tail)
                          : (LOG_BUFFER_BYTES - tail + head);
        if (used * 100u < (size_t)LOG_BUFFER_HIGH_WATER_PCT * LOG_BUFFER_BYTES) {
            g_log_high_water_warned = false;
        }
    }
    LOG_UNLOCK();
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

size_t log_buffer_used_pct() {
    LOG_LOCK();
    size_t used = (head >= tail)
                      ? (head - tail)
                      : (LOG_BUFFER_BYTES - tail + head);
    size_t pct = (used * 100u) / LOG_BUFFER_BYTES;
    LOG_UNLOCK();
    return pct;
}

bool sd_is_present() { return g_sd_present; }

bool log_has_overflow_file() {
    if (!init_sd()) return false;
    return SD.exists(LOG_OVERFLOW_FILE);
}

size_t log_overflow_file_size() {
    if (!init_sd()) return 0;
    if (g_overflow_file) return g_overflow_file.size();
    File f = SD.open(LOG_OVERFLOW_FILE, FILE_READ);
    size_t s = f ? f.size() : 0;
    if (f) f.close();
    return s;
}

bool log_open_overflow_for_read() {
    if (!init_sd()) return false;
    if (!SD.exists(LOG_OVERFLOW_FILE)) return false;
    g_overflow_file = SD.open(LOG_OVERFLOW_FILE, FILE_READ);
    return g_overflow_file;
}

size_t log_read_overflow_chunk(uint8_t* buf, size_t len) {
    if (!g_overflow_file) return 0;
    return g_overflow_file.read(buf, len);
}

void log_close_overflow() {
    if (g_overflow_file) {
        g_overflow_file.close();
        SD.remove(LOG_OVERFLOW_FILE);
        g_overflow_file = File();
    }
}

void log_close_overflow_keep() {
    if (g_overflow_file) {
        g_overflow_file.close();
        g_overflow_file = File();
    }
}

// ── Telemetry overflow queueing ─────────────────────────────────────
// When MQTT publish fails, the JSON payload is saved to
// /telemetry_overflow.jsonl (one line per payload) and retried on
// subsequent ticks. Capped at 1 MB to prevent filling the card.
static const char* TELEM_OVERFLOW_FILE = "/telemetry_overflow.jsonl";
static const size_t TELEM_OVERFLOW_MAX_BYTES = 1UL * 1024UL * 1024UL;
static File g_telem_overflow_file;

bool save_telemetry_overflow(const char* data, size_t len) {
    if (!data || len == 0) return false;
    if (!init_sd()) return false;

    // Cap check
    if (SD.exists(TELEM_OVERFLOW_FILE)) {
        File f = SD.open(TELEM_OVERFLOW_FILE, FILE_READ);
        if (f) {
            size_t sz = f.size();
            f.close();
            if (sz >= TELEM_OVERFLOW_MAX_BYTES) {
                SD.remove(TELEM_OVERFLOW_FILE);
            }
        }
    }

    File f = SD.open(TELEM_OVERFLOW_FILE, FILE_APPEND);
    if (!f) return false;
    size_t written = f.write((const uint8_t*)data, len);
    // Append newline as JSONL delimiter
    if (written == len) f.write((const uint8_t*)"\n", 1);
    f.close();
    return (written == len);
}

size_t drain_telemetry_overflow(char* out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    if (!init_sd()) return 0;
    if (!SD.exists(TELEM_OVERFLOW_FILE)) return 0;

    // Open for reading if not already open
    if (!g_telem_overflow_file) {
        g_telem_overflow_file = SD.open(TELEM_OVERFLOW_FILE, FILE_READ);
        if (!g_telem_overflow_file) return 0;
    }

    // Read one line (up to newline or EOF)
    size_t pos = 0;
    while (pos < out_len - 1) {
        int c = g_telem_overflow_file.read();
        if (c < 0) break;  // EOF
        if (c == '\n') break;  // end of this entry
        out[pos++] = (char)c;
    }
    out[pos] = '\0';

    if (pos == 0) {
        // Empty line or EOF — close and remove the file
        g_telem_overflow_file.close();
        g_telem_overflow_file = File();
        SD.remove(TELEM_OVERFLOW_FILE);
        return 0;
    }

    // Check if we've consumed the whole file
    bool at_end = (g_telem_overflow_file.available() == 0);
    if (at_end) {
        g_telem_overflow_file.close();
        g_telem_overflow_file = File();
        SD.remove(TELEM_OVERFLOW_FILE);
    }

    return pos;
}
