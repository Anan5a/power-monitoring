#include "coulomb_counter.h"
#include "settings_manager.h"
#include <Arduino.h>

static float accumulated_mAh[4] = {0};
static unsigned long last_persist_ms = 0;

void init_coulomb_counter() {
    for (uint8_t ch = 0; ch < 4; ch++) {
        accumulated_mAh[ch] = settings_load_coulomb_mAh(ch);
    }
    last_persist_ms = millis();
}

void update_coulomb_counter(const SensorData& data, float dt_seconds) {
    float currents[4] = {
        data.ina3221_current[0], data.ina3221_current[1],
        data.ina3221_current[2], data.ina226_current
    };
    for (uint8_t ch = 0; ch < 4; ch++) {
        accumulated_mAh[ch] += currents[ch] * dt_seconds / 3600.0f;
    }
    if (millis() - last_persist_ms >= 300000) {
        for (uint8_t ch = 0; ch < 4; ch++) settings_save_coulomb_mAh(ch, accumulated_mAh[ch]);
        last_persist_ms = millis();
    }
}

float get_coulomb_mAh(uint8_t channel) {
    return (channel > 3) ? 0 : accumulated_mAh[channel];
}

void reset_coulomb_counter(uint8_t channel) {
    if (channel > 3) return;
    accumulated_mAh[channel] = 0;
    settings_save_coulomb_mAh(channel, 0);
}
