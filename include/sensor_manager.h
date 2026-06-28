#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <stdint.h>
#include "sensor_pod.h"

#define BURST_N 4
#define SPIKE_STDDEV_MULT 4
#define SPIKE_DEVIATION_MV 50.0f
#define SPIKE_DEVIATION_MA 20.0f
#define BASELINE_TICKS 10

struct ChannelCalibration;
bool settings_load_channel_calibration(struct ChannelCalibration* out);
void settings_save_channel_calibration(const struct ChannelCalibration* in);

void init_sensors();
void reinit_sensors();  // reload shunt/volt_ratio/resistor settings from NVS
SensorSnapshot read_sensors();
void discover_sensors();  // I2C scan + UART listen, auto-register pods, persist to NVS

const PhysicalChannel* sensor_get_logical_channel(uint8_t logical_ch);
const PhysicalChannel* sensor_get_logical_channel(const SensorSnapshot& snap, uint8_t logical_ch);
uint8_t sensor_get_logical_channel_count();
SampleMeta sensor_get_meta(uint8_t logical_ch);

// Backward-compatible flat-channel helpers (ch 0..3 were the legacy logical channels)
float get_channel_voltage(uint8_t ch);
float get_channel_current(uint8_t ch);
float get_channel_power(uint8_t ch);

// Snapshot-based flat-channel helpers (use these with queued/batched SensorSnapshots)
float get_channel_voltage(const SensorSnapshot& snap, uint8_t ch);
float get_channel_current(const SensorSnapshot& snap, uint8_t ch);
float get_channel_power(const SensorSnapshot& snap, uint8_t ch);

void sensor_calibrate_baseline();
void sensor_get_baseline_progress(float* stddev_out, uint8_t* tick_count_out);  // for calibration status reporting
bool sensor_is_calibrating();  // true when baseline calibration is collecting (1-9 ticks)
float ina3221_getShuntVoltage(uint8_t ch);
float ina226_getShuntVoltage();

void sensor_set_calibration(uint8_t ch, uint8_t type, float value);
void sensor_get_calibration(uint8_t ch, float* volt_offset_mv, float* volt_gain, float* curr_offset_mv, float* curr_gain);
void sensor_reset_calibration(uint8_t ch);
void sensor_set_invert_curr(uint8_t ch, bool invert);
void sensor_reset_invert_curr(uint8_t ch);
float ina3221_getVoltModuleBusVoltage(uint8_t ch);

#endif
