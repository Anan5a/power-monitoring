#ifndef DATA_LOGGER_H
#define DATA_LOGGER_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "sensor_manager.h"

enum EntryType : uint8_t { ENTRY_BASE = 0xB0, ENTRY_DELTA = 0xD0 };

// Data logger wire format: logs the FIRST 4 logical channels only. The
// pod model exposes up to MAX_LOGICAL_CHANNELS (16) channels via telemetry
// (MQTT/HTTP/Supabase), but the on-device buffer is fixed at 4 channels
// to keep the ring buffer small and the delta-compression math simple.
// Channels 4..15 are not recorded in the local data buffer — they only
// reach consumers via the v1 telemetry schema that includes all 16.
// Bumping the channel count to MAX_LOGICAL_CHANNELS would be a wire-
// format break (BaseEntry/DeltaEntry would need a count byte and a v2
// entry type).
struct BaseEntry {
    uint8_t  type;
    uint32_t timestamp_ms;
    int16_t  v[4];
    int16_t  i[4];
    int16_t  p[4];
} __attribute__((packed));

struct DeltaEntry {
    uint8_t  type;
    // dt_ms is uint16_t: max representable gap is 65535 ms (~65.5 s).
    // log_sample() clamps the gap to 60 s and forces a BaseEntry when the
    // wall-clock delta exceeds that, so on-wire dt_ms is always <= 60000.
    // Reconstruction code in connectivity_manager.cpp relies on this clamp.
    uint16_t dt_ms;
    int16_t  dv[4];
    int16_t  di[4];
    int16_t  dp[4];
} __attribute__((packed));

struct LogSnapshot {
    uint32_t timestamp_s;    // epoch seconds
    float voltage[4];
    float current[4];
    float power[4];
};

void init_data_logger();
void log_sample(const SensorSnapshot& data, uint32_t timestamp_ms);
void log_set_epoch(time_t epoch);
time_t log_to_epoch(uint32_t timestamp_ms);

// True once a trusted (NTP-derived) wall-clock epoch has been set; false
// means log timestamps are computed from uptime and will drift from real
// time. Consumers should mark timestamps "untrusted" if this is false.
//
// log_epoch_valid_set() lets external modules (e.g. connectivity_manager's
// sync_time()) flag the validity explicitly, since the validity is owned
// by whoever last touched the clock. The init code should call
//   log_epoch_valid_set(time(nullptr) > 1700000000)
// right after the first NTP attempt, and on every later sync.
void log_epoch_valid_set(bool valid);
bool log_epoch_valid();

size_t log_pop_batch(uint8_t* out_buf, size_t out_len);
bool log_peek_latest(LogSnapshot* out);
uint32_t log_entries_count();
bool log_is_full();
size_t log_buffer_capacity();

// Returns the current ring-buffer occupancy as a percentage 0..100.
// Used by telemetry / status paths to surface near-overflow conditions
// before the buffer actually fills and entries are dropped. The value
// is computed under the same lock that log_sample / log_pop_batch take,
// so callers see a consistent snapshot.
size_t log_buffer_used_pct();

// LittleFS overflow management
bool log_has_overflow_file();
size_t log_overflow_file_size();
bool log_open_overflow_for_read();
size_t log_read_overflow_chunk(uint8_t* buf, size_t len);
void log_close_overflow();

#endif
