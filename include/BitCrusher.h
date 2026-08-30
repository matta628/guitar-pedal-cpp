#pragma once

#include <atomic>

#include "Effect.h"

// Two separate kinds of digital damage, deliberately kept as separate knobs
// because they sound nothing alike.
//
//  * bits — amplitude quantisation. Rounding each sample to a coarse grid adds
//           a distortion that scales with the *signal*, so it is loudest on
//           quiet passages and vanishes on loud ones. That inverted dynamic is
//           what makes it sound broken rather than merely distorted.
//  * rate — sample-and-hold decimation. Holding each sample for N frames folds
//           everything above the new Nyquist back down as aliasing, adding
//           inharmonic content that moves in the opposite direction to the
//           notes being played. There is no anti-aliasing filter here, and
//           that is the point: the aliasing *is* the effect.
//
// Both are here for The Voidz, whose guitars sound like they have been through
// a failing sampler.
class BitCrusher : public Effect {
public:
    void set_bits(float bits);         // 1 .. 16
    void set_downsample(float factor); // 1 (off) .. 64
    void set_mix(float mix);

    float bits() const { return bits_.load(std::memory_order_relaxed); }
    float downsample() const { return downsample_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::atomic<float> bits_{8.0f};
    std::atomic<float> downsample_{1.0f};
    std::atomic<float> mix_{1.0f};

    float held_ = 0.0f;
    int hold_counter_ = 0;
};
