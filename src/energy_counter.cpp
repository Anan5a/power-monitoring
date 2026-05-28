#include "energy_counter.h"
#include "settings_manager.h"
#include "sensor_manager.h"
#include <Arduino.h>

static float accumulated_Wh[4] = {0};
static unsigned long last_persist_ms = 0;

void init_energy_counter() {
    for (uint8_t ch = 0; ch < 4; ch++) {
        accumulated_Wh[ch] = settings_load_energy_Wh(ch);
    }
    last_persist_ms = millis();
}

void update_energy_counter(const SensorData& data, float dt_seconds) {
    float power[4] = {
        data.ina3221_busV[0] * data.ina3221_current[0],
        data.ina3221_busV[1] * data.ina3221_current[1],
        data.ina3221_busV[2] * data.ina3221_current[2],
        data.ina226_power
    };

    for (uint8_t ch = 0; ch < 4; ch++) {
        accumulated_Wh[ch] += power[ch] * dt_seconds / 3600.0f;
    }
    if (millis() - last_persist_ms >= 300000) {
        for (uint8_t ch = 0; ch < 4; ch++) settings_save_energy_Wh(ch, accumulated_Wh[ch]);
        last_persist_ms = millis();
    }
}

float get_energy_Wh(uint8_t channel) {
    return (channel > 3) ? 0 : accumulated_Wh[channel];
}

void reset_energy_counter(uint8_t channel) {
    if (channel > 3) return;
    accumulated_Wh[channel] = 0;
    settings_save_energy_Wh(channel, 0);
}
