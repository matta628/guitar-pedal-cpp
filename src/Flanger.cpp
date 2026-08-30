#include "Flanger.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
constexpr float kMaxDelayMs = 25.0f;
}  // namespace

Flanger::Flanger(float sample_rate)
    : sample_rate_(sample_rate),
      buffer_(static_cast<std::size_t>(kMaxDelayMs * 0.001f * sample_rate) + 4, 0.0f) {}

void Flanger::set_rate_hz(float rate_hz) {
    rate_hz_.store(std::clamp(rate_hz, 0.02f, 10.0f), std::memory_order_relaxed);
}
void Flanger::set_depth_ms(float depth_ms) {
    depth_ms_.store(std::clamp(depth_ms, 0.0f, 8.0f), std::memory_order_relaxed);
}
void Flanger::set_delay_ms(float delay_ms) {
    delay_ms_.store(std::clamp(delay_ms, 0.5f, 10.0f), std::memory_order_relaxed);
}
void Flanger::set_feedback(float feedback) {
    feedback_.store(std::clamp(feedback, -0.95f, 0.95f), std::memory_order_relaxed);
}
void Flanger::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Flanger::process(float* buffer, std::size_t n_frames) {
    const float rate = rate_hz_.load(std::memory_order_relaxed);
    const float depth_ms = depth_ms_.load(std::memory_order_relaxed);
    const float delay_ms = delay_ms_.load(std::memory_order_relaxed);
    const float feedback = feedback_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);
    const float increment = rate / sample_rate_;
    const std::size_t size = buffer_.size();

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float lfo = std::sin(kTwoPi * phase_);
        // Clamped at 0.2 ms: at a delay of zero the wet path would be the dry
        // path, the feedback loop would become a bare gain, and the notches
        // would vanish rather than sweep.
        const float current_ms = std::max(0.2f, delay_ms + depth_ms * lfo);
        const float delay_samples = current_ms * 0.001f * sample_rate_;

        float read_pos = static_cast<float>(pos_) - delay_samples;
        while (read_pos < 0.0f) read_pos += static_cast<float>(size);

        const auto idx0 = static_cast<std::size_t>(read_pos) % size;
        const std::size_t idx1 = (idx0 + 1) % size;
        const float frac = read_pos - std::floor(read_pos);
        // Linear interpolation, so the sweep is smooth rather than stepping
        // between whole samples — audible as zipper noise otherwise.
        const float delayed = buffer_[idx0] * (1.0f - frac) + buffer_[idx1] * frac;

        const float dry = buffer[i];
        // tanh on the feedback path, not a clamp: at high feedback this loop
        // is meant to be pushed into self-oscillation, and soft saturation
        // makes that a howl rather than a burst of digital clipping.
        buffer_[pos_] = std::tanh(dry + delayed * feedback);
        buffer[i] = dry * (1.0f - mix) + delayed * mix;

        pos_ = (pos_ + 1) % size;
        phase_ += increment;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
    }
}
