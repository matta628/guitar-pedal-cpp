#pragma once

#include <atomic>

#include "Effect.h"

// Cascaded first-order allpass sections, swept by an LFO, summed with the dry
// signal.
//
// The mechanism is worth being precise about, because it is what separates a
// phaser from a flanger. An allpass filter changes phase without changing
// magnitude; summing it with the dry signal makes the frequencies that came
// back inverted cancel, producing notches. Sweeping the allpass corner
// frequency sweeps the notches. A flanger's notches come from a delay instead,
// so they are harmonically spaced and there are hundreds of them; a phaser's
// come from N allpass stages, so there are N/2 of them and they are not
// harmonically related. That is why a phaser sounds like breathing and a
// flanger sounds like a jet.
//
// Four stages is the Small Stone / Phase 90 configuration, which is what both
// the Radiohead and Arctic Monkeys presets are after.
class Phaser : public Effect {
public:
    static constexpr int kMaxStages = 8;

    void set_rate_hz(float rate_hz);
    void set_depth(float depth);       // how far the notches sweep
    void set_feedback(float feedback); // resonance; sharpens the notches
    void set_stages(int stages);       // 2 .. 8, even
    void set_mix(float mix);

    float rate_hz() const { return rate_hz_.load(std::memory_order_relaxed); }
    float depth() const { return depth_.load(std::memory_order_relaxed); }
    float feedback() const { return feedback_.load(std::memory_order_relaxed); }
    float stages() const { return static_cast<float>(stages_.load(std::memory_order_relaxed)); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    void set_sample_rate(float sample_rate) { sample_rate_ = sample_rate; }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::atomic<float> rate_hz_{0.5f};
    std::atomic<float> depth_{0.8f};
    std::atomic<float> feedback_{0.4f};
    std::atomic<int> stages_{4};
    std::atomic<float> mix_{0.5f};

    float sample_rate_ = 48000.0f;
    float phase_ = 0.0f;
    float last_out_ = 0.0f;
    // One sample of history per allpass section. Sized for the maximum so
    // changing the stage count never allocates.
    float x1_[kMaxStages] = {};
    float y1_[kMaxStages] = {};
};
