#include "Delay.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;
// Real tape wow sits under 1 Hz; flutter is faster and shallower. One slow
// oscillator is enough to stop the repeats sounding mechanically identical.
constexpr float kWowHz = 0.4f;
}  // namespace

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

void Delay::set_tone(float tone) {
    tone_.store(std::clamp(tone, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Delay::set_modulation_ms(float depth_ms) {
    modulation_ms_.store(std::clamp(depth_ms, 0.0f, 20.0f), std::memory_order_relaxed);
}

void Delay::process(float* buffer, std::size_t n_frames) {
    const float base_samples = delay_samples_.load(std::memory_order_relaxed);
    const float feedback = feedback_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);
    const float tone = tone_.load(std::memory_order_relaxed);
    const float modulation = modulation_ms_.load(std::memory_order_relaxed) * 0.001f * sample_rate_;
    const std::size_t size = buffer_.size();
    const float increment = kWowHz / sample_rate_;

    // tone = 1 leaves the coefficient at 1, which makes the one-pole a
    // pass-through, so the digital case costs an add and a multiply rather
    // than a branch per sample.
    const float tone_coeff = 0.02f + tone * 0.98f;

    for (std::size_t i = 0; i < n_frames; ++i) {
        float delay_samples = base_samples;
        if (modulation > 0.0f) {
            delay_samples += modulation * std::sin(kTwoPi * phase_);
            delay_samples = std::clamp(delay_samples, 1.0f, static_cast<float>(size - 2));
        }

        float read_pos = static_cast<float>(pos_) - delay_samples;
        while (read_pos < 0.0f) read_pos += static_cast<float>(size);

        // Fractional read: without interpolation a modulated delay time would
        // step between whole samples and the wobble would be heard as a click
        // train rather than a pitch drift.
        const auto idx0 = static_cast<std::size_t>(read_pos) % size;
        const std::size_t idx1 = (idx0 + 1) % size;
        const float frac = read_pos - std::floor(read_pos);
        const float delayed = buffer_[idx0] * (1.0f - frac) + buffer_[idx1] * frac;

        feedback_filter_ += tone_coeff * (delayed - feedback_filter_);

        buffer_[pos_] = buffer[i] + feedback_filter_ * feedback;
        buffer[i] = buffer[i] * (1.0f - mix) + delayed * mix;

        pos_ = (pos_ + 1) % size;
        phase_ += increment;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
    }
}
