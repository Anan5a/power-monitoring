#include "bl0939_mock.h"

BL0939Mock::BL0939Mock()
    : voltage_gen_(WaveformType::SINE, 220.0f, 245.0f, 60.0f, 0.5f),
      current_a_gen_(WaveformType::STEP, 0.2f, 4.5f, 12.0f, 0.05f),
      current_b_gen_(WaveformType::RAMP, 0.1f, 2.8f, 25.0f, 0.03f) {}

BL0939Channel BL0939Mock::sampleChannel(uint8_t ch, float t_seconds) {
    float v = voltage_gen_.sample(t_seconds);
    float i = (ch == 0) ? current_a_gen_.sample(t_seconds) : current_b_gen_.sample(t_seconds);
    return BL0939Channel{v, i, v * i};
}
