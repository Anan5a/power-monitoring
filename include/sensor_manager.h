#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <stdint.h>

struct SensorData {
    // INA3221: 3-channel bus voltage (V) and current (A)
    float ina3221_busV[3];
    float ina3221_current[3];

    // INA226: single-channel bus voltage (V), current (A), power (W)
    float ina226_busV;
    float ina226_current;
    float ina226_power;

    // ADS1115: 4 single-ended channels in volts
    float ads1115_volts[4];
};

float ina3221_getShuntVoltage(uint8_t ch);
float ina226_getShuntVoltage();

void init_sensors();
SensorData read_sensors();

#endif
