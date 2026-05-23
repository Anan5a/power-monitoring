#include "sensor_manager.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_INA3221.h>
#include <INA226.h>
#include <Adafruit_ADS1X15.h>

static Adafruit_INA3221 ina3221;
static INA226 ina226(INA226_ADDR, &Wire);
static Adafruit_ADS1115 ads1115;

void init_sensors() {
    Wire.begin(I2C_SDA, I2C_SCL);

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
        d.ina3221_busV[ch]    = ina3221.getBusVoltage(ch);
        d.ina3221_current[ch] = ina3221.getCurrentAmps(ch);
    }
#endif

#if ENABLE_INA226
    d.ina226_busV    = ina226.getBusVoltage();
    d.ina226_current = ina226.getCurrent();
    d.ina226_power   = ina226.getPower();
#endif

#if ENABLE_ADS1115
    for (uint8_t ch = 0; ch < 4; ch++) {
        int16_t raw = ads1115.readADC_SingleEnded(ch);
        d.ads1115_volts[ch] = ads1115.computeVolts(raw);
    }
#endif

    return d;
}
