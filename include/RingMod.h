#pragma once

#include <atomic>

#include "Effect.h"

// Multiplies the signal by a sine oscillator.
//
// The output contains the sum and difference of every input frequency with the
// carrier and nothing else — the original notes are gone. Because those sums
// and differences are not harmonically related to the input, the result is
// inharmonic and metallic rather than musical, which is exactly why it earns a
// place here: it is a large part of why The Voidz sound broken on purpose.
//
// At carrier frequencies below ~20 Hz this stops being a ring modulator and
// becomes a tremolo, which is the same maths heard differently.
class RingMod : public Effect {
public:
    void set_frequency_hz(float hz);   // 1 .. 4000
    void set_mix(float mix);

    float frequency_hz() const { return frequency_hz_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    void set_sample_rate(float sample_rate) { sample_rate_ = sample_rate; }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::atomic<float> frequency_hz_{200.0f};
    std::atomic<float> mix_{0.5f};

    float sample_rate_ = 48000.0f;
    float phase_ = 0.0f;
};
