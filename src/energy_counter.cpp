#include "energy_counter.h"
#include "settings_manager.h"
#include "sensor_manager.h"
#include "sensor_pod.h"  // MAX_LOGICAL_CHANNELS
#include "connectivity_manager.h"
#include <Arduino.h>

static float accumulated_Wh[MAX_LOGICAL_CHANNELS] = {0};
static unsigned long last_persist_ms = 0;

void init_energy_counter() {
    uint8_t lcount = sensor_get_logical_channel_count();
    if (lcount > MAX_LOGICAL_CHANNELS) lcount = MAX_LOGICAL_CHANNELS;
    for (uint8_t ch = 0; ch < lcount; ch++) {
        accumulated_Wh[ch] = settings_load_energy_Wh(ch);
    }
    last_persist_ms = millis();
}

void update_energy_counter(const SensorSnapshot& data, float dt_seconds) {
    uint8_t lcount = sensor_get_logical_channel_count();
    if (lcount > MAX_LOGICAL_CHANNELS) lcount = MAX_LOGICAL_CHANNELS;
    for (uint8_t vc = 0; vc < lcount; vc++) {
        VirtualChannelConfig vc_cfg;
        float power;

        if (settings_load_virtual_channel(vc, &vc_cfg) && (vc_cfg.voltage_src > 0 || vc_cfg.current_src > 0)) {
            // Use virtual channel mapping
            float v = get_sensor_voltage(vc_cfg.voltage_src, vc_cfg.voltage_idx, data);
            float i = get_sensor_current(vc_cfg.current_src, vc_cfg.current_idx, data);
            power = v * i;
        } else {
            // Fall back to logical channel vc
            power = get_channel_power(vc);
        }

        accumulated_Wh[vc] += power * dt_seconds / 3600.0f;
    }
    if (millis() - last_persist_ms >= 300000) {
        for (uint8_t ch = 0; ch < lcount; ch++) settings_save_energy_Wh(ch, accumulated_Wh[ch]);
        last_persist_ms = millis();
    }
}

float get_energy_Wh(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0;
    return accumulated_Wh[channel];
}

void reset_energy_counter(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return;
    accumulated_Wh[channel] = 0;
    settings_save_energy_Wh(channel, 0);
}
