#pragma once

#include <atomic>
#include <vector>

#include "Effect.h"

// A very short modulated delay, fed back on itself.
//
// Structurally this is Chorus with two changes, and both matter. The delay is
// an order of magnitude shorter (0.5-10 ms rather than 15 ms), which puts the
// comb notches inside the audible range and harmonically spaced rather than
// producing the detuned-double effect chorus gives. And it has feedback, which
// chorus does not — that is what turns a gentle sweep into the jet-engine
// whoosh, and what lets it self-oscillate at the top of its range.
//
// Negative feedback is deliberately allowed: inverting the feedback path moves
// the notches to sit between the harmonics instead of on them, which is the
// hollower "through-zero-ish" flange, and it is how Robin Guthrie's broken
// BF-2 is described as behaving.
class Flanger : public Effect {
public:
    explicit Flanger(float sample_rate);

    void set_rate_hz(float rate_hz);
    void set_depth_ms(float depth_ms);     // sweep width
    void set_delay_ms(float delay_ms);     // centre of the sweep, 0.5 .. 10
    void set_feedback(float feedback);     // -0.95 .. 0.95
    void set_mix(float mix);

    float rate_hz() const { return rate_hz_.load(std::memory_order_relaxed); }
    float depth_ms() const { return depth_ms_.load(std::memory_order_relaxed); }
    float delay_ms() const { return delay_ms_.load(std::memory_order_relaxed); }
    float feedback() const { return feedback_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    float sample_rate_;
    std::vector<float> buffer_;
    std::size_t pos_ = 0;
    float phase_ = 0.0f;

    std::atomic<float> rate_hz_{0.25f};
    std::atomic<float> depth_ms_{2.0f};
    std::atomic<float> delay_ms_{3.0f};
    std::atomic<float> feedback_{0.5f};
    std::atomic<float> mix_{0.5f};
};
