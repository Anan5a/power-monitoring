#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <stdint.h>

struct SensorData {
    // INA3221 current module (0x40): 3-channel bus voltage (V) and current (A)
    float ina3221_busV[3];
    float ina3221_current[3];

    // INA226: single-channel bus voltage (V), current (A), power (W)
    float ina226_busV;
    float ina226_current;
    float ina226_power;

    // Voltage module (0x42) readings: voltage in volts per channel (after divider ratio)
    // ads1115_volts[0..2] holds scaled voltage (V) for CH0-CH2 when INA3221_VOLT enabled
    float ads1115_volts[4];
};

// Forward declarations for calibration struct (defined in settings_manager.h)
struct ChannelCalibration;
bool settings_load_channel_calibration(struct ChannelCalibration* out);
void settings_save_channel_calibration(const struct ChannelCalibration* in);

void init_sensors();
SensorData read_sensors();
float ina3221_getShuntVoltage(uint8_t ch);
float ina226_getShuntVoltage();

void sensor_set_calibration(uint8_t ch, uint8_t type, float value);
void sensor_get_calibration(uint8_t ch, float* volt_offset_mv, float* volt_gain, float* curr_offset_ma, float* curr_gain);
void sensor_reset_calibration(uint8_t ch);
float ina3221_getVoltModuleBusVoltage(uint8_t ch);

#endif
