#include "Delay.h"

#include <algorithm>
#include <cmath>

Delay::Delay(float sample_rate, float max_delay_seconds)
    : sample_rate_(sample_rate),
      buffer_(static_cast<std::size_t>(max_delay_seconds * sample_rate), 0.0f),
      delay_samples_(0.3f * sample_rate) {}

void Delay::set_delay_seconds(float seconds) {
    const float max_samples = static_cast<float>(buffer_.size() - 1);
    delay_samples_.store(std::clamp(seconds * sample_rate_, 0.0f, max_samples),
                          std::memory_order_relaxed);
}

void Delay::set_feedback(float feedback) {
    feedback_.store(std::clamp(feedback, 0.0f, 0.95f), std::memory_order_relaxed);
}

void Delay::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Delay::process(float* buffer, std::size_t n_frames) {
    const auto delay_samples = static_cast<std::size_t>(delay_samples_.load(std::memory_order_relaxed));
    const float feedback = feedback_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);
    const std::size_t size = buffer_.size();

    for (std::size_t i = 0; i < n_frames; ++i) {
        const std::size_t read_index = (pos_ + size - delay_samples) % size;
        const float delayed = buffer_[read_index];

        buffer_[pos_] = buffer[i] + delayed * feedback;
        buffer[i] = buffer[i] * (1.0f - mix) + delayed * mix;

        pos_ = (pos_ + 1) % size;
    }
}
