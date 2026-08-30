#include "Tremolo.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
}

void Tremolo::set_rate_hz(float rate_hz) {
    rate_hz_.store(std::clamp(rate_hz, 0.1f, 20.0f), std::memory_order_relaxed);
}
void Tremolo::set_depth(float depth) {
    depth_.store(std::clamp(depth, 0.0f, 1.0f), std::memory_order_relaxed);
}
void Tremolo::set_shape(float shape) {
    shape_.store(std::clamp(shape, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Tremolo::process(float* buffer, std::size_t n_frames) {
    const float rate = rate_hz_.load(std::memory_order_relaxed);
    const float depth = depth_.load(std::memory_order_relaxed);
    const float shape = shape_.load(std::memory_order_relaxed);
    const float increment = rate / sample_rate_;

    // tanh(k*sin) approaches a square as k grows, and is exactly sin at k -> 0.
    // One expression covers the whole knob, and it stays band-limited enough
    // not to alias the way a hard square would.
    const float hardness = 1.0f + shape * 12.0f;
    const float normalize = 1.0f / std::tanh(hardness);

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float raw = std::sin(kTwoPi * phase_);
        const float shaped = (shape > 0.0f) ? std::tanh(hardness * raw) * normalize : raw;
        // Map -1..1 to a gain that never exceeds unity: tremolo ducks, it does
        // not boost, so full depth reaches silence rather than +6 dB.
        const float gain = 1.0f - depth * (0.5f - 0.5f * shaped);

        buffer[i] *= gain;

        phase_ += increment;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
    }
}
