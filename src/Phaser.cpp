#include "Phaser.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
// The sweep range. Below ~200 Hz the notches sit under the guitar's
// fundamental and stop being audible as movement; above ~1.6 kHz they run out
// of harmonics to cancel.
constexpr float kMinHz = 200.0f;
constexpr float kMaxHz = 1600.0f;
}  // namespace

void Phaser::set_rate_hz(float rate_hz) {
    rate_hz_.store(std::clamp(rate_hz, 0.05f, 10.0f), std::memory_order_relaxed);
}
void Phaser::set_depth(float depth) {
    depth_.store(std::clamp(depth, 0.0f, 1.0f), std::memory_order_relaxed);
}
void Phaser::set_feedback(float feedback) {
    // Capped below 1: the feedback path runs through an allpass chain that can
    // sustain its own oscillation, and unlike a delay there is no decay to
    // stop it.
    feedback_.store(std::clamp(feedback, 0.0f, 0.9f), std::memory_order_relaxed);
}
void Phaser::set_stages(int stages) {
    stages_.store(std::clamp(stages, 2, kMaxStages) & ~1, std::memory_order_relaxed);
}
void Phaser::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Phaser::process(float* buffer, std::size_t n_frames) {
    const float rate = rate_hz_.load(std::memory_order_relaxed);
    const float depth = depth_.load(std::memory_order_relaxed);
    const float feedback = feedback_.load(std::memory_order_relaxed);
    const int stages = stages_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);
    const float increment = rate / sample_rate_;

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float lfo = 0.5f + 0.5f * std::sin(kTwoPi * phase_);
        // Sweep the corner frequency logarithmically: the ear hears pitch that
        // way, so a linear sweep spends most of its time sounding stationary
        // at the top of the range.
        const float centre = kMinHz * std::pow(kMaxHz / kMinHz, lfo * depth);

        // First-order allpass coefficient for that corner frequency.
        const float t = std::tan(3.14159265f * centre / sample_rate_);
        const float a = (t - 1.0f) / (t + 1.0f);

        const float dry = buffer[i];
        float x = dry + feedback * last_out_;

        for (int s = 0; s < stages; ++s) {
            const float y = a * x + x1_[s] - a * y1_[s];
            x1_[s] = x;
            y1_[s] = y;
            x = y;
        }
        last_out_ = x;

        buffer[i] = dry * (1.0f - mix) + x * mix;

        phase_ += increment;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
    }
}
