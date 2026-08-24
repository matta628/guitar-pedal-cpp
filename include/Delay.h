#pragma once

#include <atomic>
#include <vector>

#include "Effect.h"

class Delay : public Effect {
public:
    Delay(float sample_rate, float max_delay_seconds = 2.0f);

    void set_delay_seconds(float seconds);
    void set_feedback(float feedback);
    void set_mix(float mix);

    void process(float* buffer, std::size_t n_frames) override;

private:
    float sample_rate_;
    std::vector<float> buffer_;
    std::size_t pos_ = 0;

    std::atomic<float> delay_samples_;
    std::atomic<float> feedback_{0.35f};
    std::atomic<float> mix_{0.35f};
};
