#include "Chorus.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
}

Chorus::Chorus(float sample_rate)
    : sample_rate_(sample_rate),
      buffer_(static_cast<std::size_t>(0.05f * sample_rate) + 4, 0.0f) {}

void Chorus::set_rate_hz(float rate_hz) {
    rate_hz_.store(std::clamp(rate_hz, 0.05f, 10.0f), std::memory_order_relaxed);
}

void Chorus::set_depth_ms(float depth_ms) {
    depth_ms_.store(std::clamp(depth_ms, 0.0f, 10.0f), std::memory_order_relaxed);
}

void Chorus::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Chorus::process(float* buffer, std::size_t n_frames) {
    const float rate_hz = rate_hz_.load(std::memory_order_relaxed);
    const float depth_ms = depth_ms_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);
    const float phase_increment = rate_hz / sample_rate_;
    const std::size_t size = buffer_.size();

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float lfo = std::sin(kTwoPi * phase_);
        const float delay_ms = kCenterDelayMs + depth_ms * lfo;
        const float delay_samples = delay_ms * 0.001f * sample_rate_;

        float read_pos = static_cast<float>(pos_) - delay_samples;
        while (read_pos < 0.0f) {
            read_pos += static_cast<float>(size);
        }
        const auto idx0 = static_cast<std::size_t>(read_pos) % size;
        const std::size_t idx1 = (idx0 + 1) % size;
        const float frac = read_pos - std::floor(read_pos);
        const float delayed = buffer_[idx0] * (1.0f - frac) + buffer_[idx1] * frac;

        buffer_[pos_] = buffer[i];
        buffer[i] = buffer[i] * (1.0f - mix) + delayed * mix;

        pos_ = (pos_ + 1) % size;
        phase_ += phase_increment;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
        }
    }
}
