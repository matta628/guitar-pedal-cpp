#pragma once

#include <atomic>

#include "Effect.h"

// Distortion that folds instead of clipping.
//
// Every other drive here (Fuzz, Distortion, Amp) saturates: push past the
// ceiling and the waveform flattens against it, which adds harmonics that fall
// off steadily as they climb. A folder does the opposite -- past the threshold
// the signal reverses direction and travels back down, so a loud input crosses
// the fold several times per half-cycle and sprays high harmonics that do NOT
// fall off with frequency.
//
// Musically that means the timbre changes wildly with playing dynamics rather
// than just getting louder and dirtier: the number of folds is set by how hard
// you hit the string. Turn `drive` up and single notes gain a metallic, almost
// ring-modulated edge, which is the West Coast synth sound (Buchla, Serge) and
// nothing else in this chain can make it.
//
// `symmetry` offsets the signal before folding. Off-centre, the positive and
// negative halves fold at different points, so even harmonics appear and the
// tone gets hollower and more vocal.
class WaveFolder : public Effect {
public:
    void set_drive(float drive);       // 1 .. 20, how many folds a loud note sees
    void set_symmetry(float offset);   // -0.5 .. 0.5, DC offset before folding
    void set_level(float level);       // makeup, since folding loses energy
    void set_mix(float mix);

    float drive() const { return drive_.load(std::memory_order_relaxed); }
    float symmetry() const { return symmetry_.load(std::memory_order_relaxed); }
    float level() const { return level_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::atomic<float> drive_{3.0f};
    std::atomic<float> symmetry_{0.0f};
    std::atomic<float> level_{0.6f};
    std::atomic<float> mix_{1.0f};
};
