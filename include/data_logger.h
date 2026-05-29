#ifndef DATA_LOGGER_H
#define DATA_LOGGER_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "sensor_manager.h"

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

struct LogSnapshot {
    uint32_t timestamp_s;    // epoch seconds
    float voltage[4];
    float current[4];
    float power[4];
};

void init_data_logger();
void log_sample(const SensorData& data, uint32_t timestamp_ms);
void log_set_epoch(time_t epoch);
time_t log_to_epoch(uint32_t timestamp_ms);

size_t log_pop_batch(uint8_t* out_buf, size_t out_len);
bool log_peek_latest(LogSnapshot* out);
uint32_t log_entries_count();
bool log_is_full();

// LittleFS overflow management
bool log_has_overflow_file();
size_t log_overflow_file_size();
bool log_open_overflow_for_read();
size_t log_read_overflow_chunk(uint8_t* buf, size_t len);
void log_close_overflow();

#endif
