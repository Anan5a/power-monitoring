#ifndef WAVEFORM_GENERATOR_H
#define WAVEFORM_GENERATOR_H

enum class WaveformType {
    SINE,
    RAMP,
    STEP,
    NOISE
};

class WaveformGenerator {
public:
    WaveformGenerator(WaveformType type, float min_val, float max_val,
                      float period_seconds, float noise_amplitude = 0.0f);

    float sample(float t_seconds);

private:
    WaveformType type_;
    float min_;
    float max_;
    float period_;
    float noise_amp_;
};

#endif
