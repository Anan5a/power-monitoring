#include "connectivity_manager.h"

float get_sensor_voltage(uint8_t src, uint8_t idx, const SensorSnapshot& data) {
    (void)data;
    if (src == 1 || src == 2) return get_channel_voltage(idx < 4 ? idx : 0);
    if (src == 3) return get_channel_voltage(3);
    if (src == 4) return 0.0f;
    return 0.0f;
}

float get_sensor_current(uint8_t src, uint8_t idx, const SensorSnapshot& data) {
    (void)data;
    if (src == 1 || src == 2) return get_channel_current(idx < 4 ? idx : 0);
    if (src == 3) return get_channel_current(3);
    return 0.0f;
}

float get_sensor_power(uint8_t src, uint8_t idx, const SensorSnapshot& data) {
    (void)data;
    if (src == 1 || src == 2) {
        uint8_t ch = idx < 4 ? idx : 0;
        return get_channel_voltage(ch) * get_channel_current(ch);
    }
    if (src == 3) return get_channel_power(3);
    return 0.0f;
}
