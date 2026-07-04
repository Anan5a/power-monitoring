#include "coulomb_counter.h"
#include "settings_manager.h"
#include "sensor_manager.h"
#include "sensor_pod.h"  // MAX_LOGICAL_CHANNELS
#include <Arduino.h>

static float accumulated_mAh[MAX_LOGICAL_CHANNELS] = {0};
static unsigned long last_persist_ms = 0;

// Sign convention: positive current = charge INTO the battery, negative
// current = discharge OUT of the battery. Each tick we integrate
//   accumulated_mAh += current_a * dt_seconds / 3600 * 1000
// so a fresh battery (0 mAh accumulated) reads as 100% SoC via
//   soc = 100.0f + (net_mAh / cap_mAh) * 100.0f
// in cycle_counter.cpp. A charge-then-discharge cycle that nets to zero
// correctly returns SoC to 100%.
//
// The legacy BatteryConfig.initial_soc_pct field could let a user offset
// the starting SoC (e.g. for a battery that ships at 50%), but the
// current cycle_counter implementation does not apply it — it always
// assumes 100% at boot. TODO: thread initial_soc_pct into the SoC
// formula so the operator can presize the state.

void init_coulomb_counter() {
    uint8_t lcount = sensor_get_logical_channel_count();
    if (lcount > MAX_LOGICAL_CHANNELS) lcount = MAX_LOGICAL_CHANNELS;
    for (uint8_t ch = 0; ch < lcount; ch++) {
        accumulated_mAh[ch] = settings_load_coulomb_mAh(ch);
    }
    last_persist_ms = millis();
}

void update_coulomb_counter(const SensorSnapshot& data, float dt_seconds) {
    (void)data;
    uint8_t lcount = sensor_get_logical_channel_count();
    if (lcount > MAX_LOGICAL_CHANNELS) lcount = MAX_LOGICAL_CHANNELS;
    for (uint8_t ch = 0; ch < lcount; ch++) {
        float current_a = get_channel_current(ch);
        accumulated_mAh[ch] += current_a * dt_seconds / 3600.0f * 1000.0f;
    }
    if (millis() - last_persist_ms >= 300000) {
        for (uint8_t ch = 0; ch < lcount; ch++) {
            settings_save_coulomb_mAh(ch, accumulated_mAh[ch]);
        }
        last_persist_ms = millis();
    }
}

float get_coulomb_mAh(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return 0;
    return accumulated_mAh[channel];
}

void reset_coulomb_counter(uint8_t channel) {
    if (channel >= MAX_LOGICAL_CHANNELS) return;
    accumulated_mAh[channel] = 0;
    settings_save_coulomb_mAh(channel, 0);
}
