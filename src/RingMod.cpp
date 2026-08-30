#include "RingMod.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
}

void RingMod::set_frequency_hz(float hz) {
    frequency_hz_.store(std::clamp(hz, 1.0f, 4000.0f), std::memory_order_relaxed);
}
void RingMod::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void RingMod::process(float* buffer, std::size_t n_frames) {
    const float frequency = frequency_hz_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);
    const float increment = frequency / sample_rate_;

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float carrier = std::sin(kTwoPi * phase_);
        buffer[i] = buffer[i] * (1.0f - mix) + buffer[i] * carrier * mix;

        phase_ += increment;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
    }
}
