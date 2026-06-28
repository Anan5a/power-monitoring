#ifndef BL0939_MOCK_H
#define BL0939_MOCK_H

#include <stdint.h>
#include "waveform_generator.h"

struct BL0939Channel {
    float voltage_V;
    float current_A;
    float power_W;
};

class BL0939Mock {
public:
    BL0939Mock();
    BL0939Channel sampleChannel(uint8_t ch, float t_seconds);

private:
    WaveformGenerator voltage_gen_;
    WaveformGenerator current_a_gen_;
    WaveformGenerator current_b_gen_;
};

#endif
