#pragma once

#include <atomic>
#include <vector>

#include "Effect.h"

class Chorus : public Effect {
public:
    explicit Chorus(float sample_rate);

    void set_rate_hz(float rate_hz);
    void set_depth_ms(float depth_ms);
    void set_mix(float mix);

    float rate_hz() const { return rate_hz_.load(std::memory_order_relaxed); }
    float depth_ms() const { return depth_ms_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    float sample_rate_;
    std::vector<float> buffer_;
    std::size_t pos_ = 0;
    float phase_ = 0.0f;

    std::atomic<float> rate_hz_{0.8f};
    std::atomic<float> depth_ms_{4.0f};
    std::atomic<float> mix_{0.5f};

    static constexpr float kCenterDelayMs = 15.0f;
};
