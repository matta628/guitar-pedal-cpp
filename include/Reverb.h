#pragma once

#include <atomic>
#include <vector>

#include "Effect.h"

class Reverb : public Effect {
public:
    explicit Reverb(float sample_rate);

    void set_room_size(float room_size);
    void set_damping(float damping);
    void set_mix(float mix);

    void process(float* buffer, std::size_t n_frames) override;

private:
    struct CombFilter {
        std::vector<float> buffer;
        std::size_t pos = 0;
        float filterstore = 0.0f;
        float feedback = 0.5f;
        float damping = 0.5f;

        float process(float input);
    };

    struct AllpassFilter {
        std::vector<float> buffer;
        std::size_t pos = 0;
        static constexpr float kFeedback = 0.5f;

        float process(float input);
    };

    std::atomic<float> room_size_{0.5f};
    std::atomic<float> damping_{0.5f};
    std::atomic<float> mix_{0.3f};

    std::vector<CombFilter> combs_;
    std::vector<AllpassFilter> allpasses_;
};
