#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <stdint.h>

#define BURST_N 4
#define SPIKE_STDDEV_MULT 4
#define SPIKE_DEVIATION_MV 50.0f
#define SPIKE_DEVIATION_MA 20.0f
#define BASELINE_TICKS 10

struct SampleMeta {
    float stddev;
    bool spike;
};

struct SensorData {
    float ina3221_busV[3];
    float ina3221_current[3];
    float ina226_busV;
    float ina226_current;
    float ina226_power;
    float ads1115_volts[4];
};

struct ChannelCalibration;
bool settings_load_channel_calibration(struct ChannelCalibration* out);
void settings_save_channel_calibration(const struct ChannelCalibration* in);

void init_sensors();
void reinit_sensors();  // reload shunt/volt_ratio/resistor settings from NVS
SensorData read_sensors();
SampleMeta sensor_get_meta(uint8_t ch);  // ch 0-2=INA3221current, 3-5=INA3221voltage, 6=INA226, 7=ADS1115
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