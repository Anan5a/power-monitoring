#include "sensor_manager.h"
#include "settings_manager.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_INA3221.h>
#include <INA226.h>
#include <Adafruit_ADS1X15.h>

static Adafruit_INA3221 ina3221;      // 0x40 — current sensing
static Adafruit_INA3221 ina3221_volt; // 0x42 — voltage sensing (resistor dividers)

static bool wire_started = false;

// Voltage divider ratios (hardware fixed)
static const float volt_ratios[3] = {
    VOLT_RATIO_CH0,
    VOLT_RATIO_CH1,
    VOLT_RATIO_CH2,
};

// Active calibration (loaded from NVS or defaults from config.h)
static ChannelCalibration cal = {
    .volt_offset_mv = {CAL_VOLT_OFFSET_MV_CH0, CAL_VOLT_OFFSET_MV_CH1, CAL_VOLT_OFFSET_MV_CH2},
    .volt_gain = {CAL_VOLT_GAIN_CH0, CAL_VOLT_GAIN_CH1, CAL_VOLT_GAIN_CH2},
    .curr_offset_ma = {CAL_CURR_OFFSET_MA_CH0, CAL_CURR_OFFSET_MA_CH1, CAL_CURR_OFFSET_MA_CH2},
    .curr_gain = {CAL_CURR_GAIN_CH0, CAL_CURR_GAIN_CH1, CAL_CURR_GAIN_CH2},
};

void init_sensors() {
    if (!wire_started) {
        Wire.begin(I2C_SDA, I2C_SCL);
        Wire.setClock(I2C_FREQ);
        wire_started = true;
    }

    // Load calibration from NVS (falls back to config.h defaults on first boot)
    ChannelCalibration saved;
    if (settings_load_channel_calibration(&saved)) {
        cal = saved;
    }

#if ENABLE_INA3221
    if (!ina3221.begin(INA3221_ADDR, &Wire)) {
        Serial.println("INA3221 current (0x40) init failed");
    } else {
        for (uint8_t ch = 0; ch < 3; ch++) {
            float shunt = 0.0f;
            if (settings_load_shunt(ch, &shunt) && shunt > 0.0f) {
                ina3221.setShuntResistance(ch, shunt);
                Serial.printf("CH%d shunt: %.6f Ohm\n", ch, shunt);
            }
        }
    }
#endif

#if ENABLE_INA3221_VOLT
    if (!ina3221_volt.begin(0x42, &Wire)) {
        Serial.println("INA3221 voltage (0x42) init failed");
    }
#endif

#if ENABLE_INA226
    if (!ina226.begin()) {
        Serial.println("INA226 disabled");
    }
#else
    Serial.println("INA226 disabled");
#endif

#if ENABLE_ADS1115
    if (!ads1115.begin(ADS1115_ADDR, &Wire)) {
        Serial.println("ADS1115 init failed");
    } else {
        ads1115.setGain(GAIN_ONE);
    }
#else
    Serial.println("ADS1115 disabled");
#endif
}

SensorData read_sensors() {
    SensorData d = {0};

#if ENABLE_INA3221  // Current module 0x40 — shunt voltage → current
    for (uint8_t ch = 0; ch < 3; ch++) {
        float raw_mv = ina3221.getBusVoltage(ch);  // mV (int16_t millivolts)
        float raw_ma = ina3221.getCurrentAmps(ch) * 1000.0f;  // A→mA

        float cal_mv = (raw_mv + cal.volt_offset_mv[ch]) * cal.volt_gain[ch];
        float cal_ma = raw_ma - cal.curr_offset_ma[ch];
        cal_ma *= cal.curr_gain[ch];

        // Dead-zone: if current magnitude below threshold, treat as zero
        if (fabsf(cal_ma) < 5.0f) cal_ma = 0.0f;

        d.ina3221_current[ch] = cal_ma / 1000.0f;  // store as A
    }
#endif

#if ENABLE_INA3221_VOLT  // Voltage module 0x42 — bus voltage through resistor dividers
    for (uint8_t ch = 0; ch < 3; ch++) {
        float raw_mv = ina3221_volt.getBusVoltage(ch);  // mV from INA3221

        // Apply calibration offset and gain
        float cal_mv = (raw_mv + cal.volt_offset_mv[ch]) * cal.volt_gain[ch];

        // Apply resistor divider ratio → store as volts
        d.ads1115_volts[ch] = (cal_mv / 1000.0f) * volt_ratios[ch];
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
        d.ads1115_volts[ch] = v;
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

#if ENABLE_INA3221_VOLT
float ina3221_getVoltModuleBusVoltage(uint8_t ch) {
    return ina3221_volt.getBusVoltage(ch);
}
#else
float ina3221_getVoltModuleBusVoltage(uint8_t) { return 0.0f; }
#endif

void sensor_set_calibration(uint8_t ch, uint8_t type, float value) {
    switch (type) {
        case 0: cal.volt_offset_mv[ch] = value; break;
        case 1: cal.volt_gain[ch] = value; break;
        case 2: cal.curr_offset_ma[ch] = value; break;
        case 3: cal.curr_gain[ch] = value; break;
        default: return;
    }
    settings_save_channel_calibration(&cal);
}

void sensor_get_calibration(uint8_t ch, float* volt_offset_mv, float* volt_gain, float* curr_offset_ma, float* curr_gain) {
    *volt_offset_mv = cal.volt_offset_mv[ch];
    *volt_gain = cal.volt_gain[ch];
    *curr_offset_ma = cal.curr_offset_ma[ch];
    *curr_gain = cal.curr_gain[ch];
}

void sensor_reset_calibration(uint8_t ch) {
    cal.volt_offset_mv[ch] = 0.0f;
    cal.volt_gain[ch] = 1.0f;
    cal.curr_offset_ma[ch] = 0.0f;
    cal.curr_gain[ch] = 1.0f;
    settings_save_channel_calibration(&cal);
}