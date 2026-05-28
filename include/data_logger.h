#ifndef DATA_LOGGER_H
#define DATA_LOGGER_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "sensor_manager.h"

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

// SPIFFS overflow management
bool log_has_overflow_file();
size_t log_overflow_file_size();
bool log_open_overflow_for_read();
size_t log_read_overflow_chunk(uint8_t* buf, size_t len);
void log_close_overflow();

#endif
