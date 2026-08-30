#pragma once

#include <atomic>

#include "Effect.h"

// Amplitude modulation by a low-frequency oscillator.
//
// The oldest effect here and the only one that is purely a volume control: no
// delay line, no filter, no state beyond the LFO phase. It is in this project
// because the Velvet Underground's sound is largely a Vox amp's built-in
// tremolo, and because Jonny Greenwood's Demeter Tremulator is all over
// Radiohead's mid-period records.
//
// `shape` matters more than it looks. A sine tremolo breathes; a square one
// chops, which is the harder, more rhythmic 1960s amp sound. Rather than two
// oscillators, the sine is progressively hardened towards a square as the knob
// turns up.
class Tremolo : public Effect {
public:
    void set_rate_hz(float rate_hz);   // 0.1 .. 20
    void set_depth(float depth);       // 0 = off, 1 = full chop
    void set_shape(float shape);       // 0 = sine, 1 = near-square

    float rate_hz() const { return rate_hz_.load(std::memory_order_relaxed); }
    float depth() const { return depth_.load(std::memory_order_relaxed); }
    float shape() const { return shape_.load(std::memory_order_relaxed); }

    void set_sample_rate(float sample_rate) { sample_rate_ = sample_rate; }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::atomic<float> rate_hz_{5.0f};
    std::atomic<float> depth_{0.6f};
    std::atomic<float> shape_{0.0f};

    float sample_rate_ = 48000.0f;
    float phase_ = 0.0f;
};
