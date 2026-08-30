#pragma once

#include <atomic>

#include "Effect.h"

// Hard, squared-off clipping, as distinct from Distortion's tanh curve.
//
// The difference is the point: Distortion saturates smoothly and keeps the
// note's envelope, which is what an overdrive does. A fuzz drives the signal so
// far past the rails that the output is nearly a square wave regardless of how
// hard you picked — a Tone Bender, a Big Muff, a RAT. Two knobs here exist only
// to chase that circuit behaviour:
//
//  * bias  — real germanium fuzzes clip asymmetrically because the transistor
//            isn't biased at the midpoint. That asymmetry is what adds even
//            harmonics and makes a Tone Bender sound warm rather than buzzy.
//            The DC it introduces is removed afterwards by the same kind of
//            coupling capacitor a real pedal has (see the DC blocker below).
//  * gate  — a dying-battery or velcro fuzz sputters and cuts out as the note
//            decays. Modelled as an envelope threshold, not a noise gate.
class Fuzz : public Effect {
public:
    void set_drive(float drive);      // 1 .. 100, input gain into the clipper
    void set_bias(float bias);        // -0.5 .. 0.5, clipping asymmetry
    void set_gate(float gate);        // 0 .. 1, sputter threshold
    void set_tone(float tone);        // 0 = dark, 1 = bright
    void set_level(float level);      // output trim
    void set_mix(float mix);

    float drive() const { return drive_.load(std::memory_order_relaxed); }
    float bias() const { return bias_.load(std::memory_order_relaxed); }
    float gate() const { return gate_.load(std::memory_order_relaxed); }
    float tone() const { return tone_.load(std::memory_order_relaxed); }
    float level() const { return level_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::atomic<float> drive_{20.0f};
    std::atomic<float> bias_{0.0f};
    std::atomic<float> gate_{0.0f};
    std::atomic<float> tone_{0.5f};
    std::atomic<float> level_{0.5f};
    std::atomic<float> mix_{1.0f};

    // Audio-thread state only.
    float lowpass_ = 0.0f;   // tone control
    float envelope_ = 0.0f;  // gate detector
    float dc_x1_ = 0.0f;     // DC blocker history
    float dc_y1_ = 0.0f;
};
