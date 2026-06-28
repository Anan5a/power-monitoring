#ifndef INA226_MOCK_H
#define INA226_MOCK_H

#include "waveform_generator.h"

struct INA226Reading {
    float busVoltage_V;
    float current_A;
    float power_W;
};

class INA226Mock {
public:
    INA226Mock();
    INA226Reading sample(float t_seconds);

private:
    WaveformGenerator voltage_gen_;
    WaveformGenerator current_gen_;
};

#endif
