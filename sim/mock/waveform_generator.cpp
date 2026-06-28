#include "waveform_generator.h"
#include <math.h>
#include <stdlib.h>

static inline float randf() {
    return static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
}

WaveformGenerator::WaveformGenerator(WaveformType type, float min_val, float max_val,
                                     float period_seconds, float noise_amplitude)
    : type_(type), min_(min_val), max_(max_val), period_(period_seconds),
      noise_amp_(noise_amplitude) {}

float WaveformGenerator::sample(float t_seconds) {
    float base = 0.0f;
    const float midpoint = (min_ + max_) * 0.5f;
    const float amplitude = (max_ - min_) * 0.5f;

    switch (type_) {
        case WaveformType::SINE:
            base = midpoint + amplitude * sinf(2.0f * 3.14159265f * t_seconds / period_);
            break;
        case WaveformType::RAMP: {
            float phase = fmodf(t_seconds, period_) / period_;
            base = min_ + (max_ - min_) * phase;
            break;
        }
        case WaveformType::STEP: {
            float phase = fmodf(t_seconds, period_) / period_;
            base = (phase < 0.5f) ? min_ : max_;
            break;
        }
        case WaveformType::NOISE:
            base = min_ + (max_ - min_) * randf();
            break;
    }

    if (noise_amp_ > 0.0f) {
        base += (randf() * 2.0f - 1.0f) * noise_amp_;
    }

    return base;
}
