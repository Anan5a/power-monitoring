#include "sensor_manager.h"

static SensorSnapshot g_last_snapshot;

void init_sensors() {}
void reinit_sensors() {}

SensorSnapshot read_sensors() {
    return g_last_snapshot;
}

void sim_set_last_snapshot(const SensorSnapshot& snap) {
    g_last_snapshot = snap;
}

static const PhysicalChannel* logical_channel_from_snapshot(const SensorSnapshot* snap, uint8_t logical_ch) {
    if (!snap || logical_ch >= snap->total_logical_channels) return nullptr;
    uint8_t logical = 0;
    for (uint8_t i = 0; i < snap->num_pods; i++) {
        uint8_t n = snap->pods[i].num_channels;
        if (logical_ch < logical + n) {
            return &snap->pods[i].channels[logical_ch - logical];
        }
        logical += n;
    }
    return nullptr;
}

const PhysicalChannel* sensor_get_logical_channel(uint8_t logical_ch) {
    return logical_channel_from_snapshot(&g_last_snapshot, logical_ch);
}

const PhysicalChannel* sensor_get_logical_channel(const SensorSnapshot& snap, uint8_t logical_ch) {
    return logical_channel_from_snapshot(&snap, logical_ch);
}

uint8_t sensor_get_logical_channel_count() {
    return g_last_snapshot.total_logical_channels;
}

SampleMeta sensor_get_meta(uint8_t logical_ch) {
    const PhysicalChannel* pc = logical_channel_from_snapshot(&g_last_snapshot, logical_ch);
    if (!pc) return SampleMeta{0.0f, false};
    return pc->meta;
}

float get_channel_voltage(uint8_t ch) {
    return get_channel_voltage(g_last_snapshot, ch);
}

float get_channel_current(uint8_t ch) {
    return get_channel_current(g_last_snapshot, ch);
}

float get_channel_power(uint8_t ch) {
    return get_channel_power(g_last_snapshot, ch);
}

float get_channel_voltage(const SensorSnapshot& snap, uint8_t ch) {
    const PhysicalChannel* pc = logical_channel_from_snapshot(&snap, ch);
    return pc ? pc->voltage : 0.0f;
}

float get_channel_current(const SensorSnapshot& snap, uint8_t ch) {
    const PhysicalChannel* pc = logical_channel_from_snapshot(&snap, ch);
    return pc ? pc->current : 0.0f;
}

float get_channel_power(const SensorSnapshot& snap, uint8_t ch) {
    const PhysicalChannel* pc = logical_channel_from_snapshot(&snap, ch);
    return pc ? pc->power : 0.0f;
}

void sensor_calibrate_baseline() {}
void sensor_get_baseline_progress(float* stddev_out, uint8_t* tick_count_out) {
    if (stddev_out) {
        for (int i = 0; i < MAX_LOGICAL_CHANNELS; i++) stddev_out[i] = 0.0f;
    }
    if (tick_count_out) *tick_count_out = 0;
}
bool sensor_is_calibrating() { return false; }

float ina3221_getShuntVoltage(uint8_t ch) {
    (void)ch;
    return 0.0001f;
}

float ina226_getShuntVoltage() {
    return 0.0001f;
}

void sensor_set_calibration(uint8_t ch, uint8_t type, float value) {
    (void)ch; (void)type; (void)value;
}
void sensor_get_calibration(uint8_t ch, float* volt_offset_mv, float* volt_gain, float* curr_offset_mv, float* curr_gain) {
    (void)ch;
    *volt_offset_mv = 0.0f; *volt_gain = 1.0f; *curr_offset_mv = 0.0f; *curr_gain = 1.0f;
}
void sensor_reset_calibration(uint8_t ch) { (void)ch; }
void sensor_set_invert_curr(uint8_t ch, bool invert) { (void)ch; (void)invert; }
void sensor_reset_invert_curr(uint8_t ch) { (void)ch; }
float ina3221_getVoltModuleBusVoltage(uint8_t ch) { (void)ch; return 0.0f; }
