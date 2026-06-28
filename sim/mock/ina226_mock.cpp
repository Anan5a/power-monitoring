#include "ina226_mock.h"

INA226Mock::INA226Mock()
    : voltage_gen_(WaveformType::SINE, 11.5f, 14.5f, 30.0f, 0.05f),
      current_gen_(WaveformType::RAMP, -2.0f, 8.0f, 20.0f, 0.02f) {}

INA226Reading INA226Mock::sample(float t_seconds) {
    float v = voltage_gen_.sample(t_seconds);
    float i = current_gen_.sample(t_seconds);
    return INA226Reading{v, i, v * i};
}
