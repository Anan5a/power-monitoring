#include "sensor_manager.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_INA3221.h>
#include <INA226.h>
#include <Adafruit_ADS1X15.h>

static Adafruit_INA3221 ina3221;
static INA226 ina226(INA226_ADDR, &Wire);
static Adafruit_ADS1115 ads1115;

static bool wire_started = false;

void init_sensors() {
    if (!wire_started) {
        Wire.begin(I2C_SDA, I2C_SCL);
        wire_started = true;
    }

#if ENABLE_INA3221
    if (!ina3221.begin(INA3221_ADDR, &Wire)) {
        Serial.println("INA3221 init failed");
    }
#else
    Serial.println("INA3221 disabled");
#endif

#if ENABLE_INA226
    if (!ina226.begin()) {
        Serial.println("INA226 init failed");
    }
#else
    Serial.println("INA226 disabled");
#endif

#if ENABLE_ADS1115
    if (!ads1115.begin(ADS1115_ADDR, &Wire)) {
        Serial.println("ADS1115 init failed");
    } else {
        ads1115.setGain(GAIN_ONE); // +/- 4.096V range
    }
#else
    Serial.println("ADS1115 disabled");
#endif
}

SensorData read_sensors() {
    SensorData d = {};

#if ENABLE_INA3221
    for (uint8_t ch = 0; ch < 3; ch++) {
        float v = ina3221.getBusVoltage(ch);
        float i = ina3221.getCurrentAmps(ch);
        float v_off = (ch == 0) ? INA3221_V_OFFSET_CH0 : (ch == 1) ? INA3221_V_OFFSET_CH1 : INA3221_V_OFFSET_CH2;
        float i_gain = (ch == 0) ? INA3221_I_GAIN_CH0 : (ch == 1) ? INA3221_I_GAIN_CH1 : INA3221_I_GAIN_CH2;
        d.ina3221_busV[ch] = v + v_off;
        d.ina3221_current[ch] = i * i_gain;
    }
#endif

#if ENABLE_INA226
    d.ina226_busV    = ina226.getBusVoltage() + INA226_V_OFFSET;
    d.ina226_current = ina226.getCurrent() * INA226_I_GAIN;
    d.ina226_power   = ina226.getPower();
#endif

#if ENABLE_ADS1115
    for (uint8_t ch = 0; ch < 4; ch++) {
        int16_t raw = ads1115.readADC_SingleEnded(ch);
        float v = ads1115.computeVolts(raw);
        float v_off = (ch == 0) ? ADS1115_OFFSET_CH0 : (ch == 1) ? ADS1115_OFFSET_CH1 : (ch == 2) ? ADS1115_OFFSET_CH2 : ADS1115_OFFSET_CH3;
        float v_gain = (ch == 0) ? ADS1115_GAIN_CH0 : (ch == 1) ? ADS1115_GAIN_CH1 : (ch == 2) ? ADS1115_GAIN_CH2 : ADS1115_GAIN_CH3;
        d.ads1115_volts[ch] = (v + v_off) * v_gain;
    }
#endif

    return d;
}

#if ENABLE_INA3221
float ina3221_getShuntVoltage(uint8_t ch) {
    return ina3221.getShuntVoltage(ch);
}
#else
float ina3221_getShuntVoltage(uint8_t) { return 0.0f; }
#endif

#if ENABLE_INA226
float ina226_getShuntVoltage() {
    return ina226.getShuntVoltage();
}
#else
float ina226_getShuntVoltage() { return 0.0f; }
#endif
