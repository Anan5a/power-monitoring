#ifndef SENSOR_POD_H
#define SENSOR_POD_H

#include <stdint.h>

#define MAX_PODS 8
#define MAX_CHANNELS_PER_POD 2
#define MAX_LOGICAL_CHANNELS (MAX_PODS * MAX_CHANNELS_PER_POD)

enum PodType { POD_INA226 = 0, POD_BL0939 };

struct SampleMeta {
    float stddev;
    bool spike;
};

struct PhysicalChannel {
    uint8_t pod_id;
    uint8_t pod_channel;
    float voltage;
    float current;
    float power;
    float energy_Wh;
    float coulomb_mAh;
    SampleMeta meta;
};

struct PodState {
    uint8_t id;
    PodType type;
    char name[16];
    uint8_t num_channels;
    PhysicalChannel channels[MAX_CHANNELS_PER_POD];
};

struct SensorSnapshot {
    uint32_t timestamp_ms;
    uint8_t num_pods;
    PodState pods[MAX_PODS];
    uint8_t total_logical_channels;
};

#endif
