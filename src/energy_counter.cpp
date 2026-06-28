#include "energy_counter.h"
#include "settings_manager.h"
#include "sensor_manager.h"
#include "connectivity_manager.h"
#include <Arduino.h>

static float accumulated_Wh[4] = {0};
static unsigned long last_persist_ms = 0;

void init_energy_counter() {
    for (uint8_t ch = 0; ch < 4; ch++) {
        accumulated_Wh[ch] = settings_load_energy_Wh(ch);
    }
    last_persist_ms = millis();
}

void update_energy_counter(const SensorSnapshot& data, float dt_seconds) {
    (void)data;
    for (uint8_t vc = 0; vc < 4; vc++) {
        VirtualChannelConfig vc_cfg;
        float power;

        if (settings_load_virtual_channel(vc, &vc_cfg) && (vc_cfg.voltage_src > 0 || vc_cfg.current_src > 0)) {
            // Use virtual channel mapping
            float v = get_sensor_voltage(vc_cfg.voltage_src, vc_cfg.voltage_idx, data);
            float i = get_sensor_current(vc_cfg.current_src, vc_cfg.current_idx, data);
            power = v * i;
        } else {
            // Fall back to legacy logical channels 0..3
            power = get_channel_power(vc);
        }

        accumulated_Wh[vc] += power * dt_seconds / 3600.0f;
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
