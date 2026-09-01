#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Effect.h"

// Holds a chord you just played as a sustained pad while you keep playing over
// the top.
//
// Different from the Looper in the one way that matters: the looper is a
// performance tool you drive deliberately and it records what you MEAN to
// record. Freeze grabs whatever happened to be in the buffer the instant you
// hit it and holds it indefinitely. You use it to turn a chord into a drone,
// not to build an arrangement.
//
// --- why this is a spectral freeze and not a grain looper ------------------
//
// The obvious implementation captures a slice of audio and loops it, crossfaded
// at the seam. That was the original here, and it has a flaw no amount of
// tuning removes: looping a slice replays everything in the slice -- the pick
// attack, the amplitude envelope, the vibrato. A 300 ms grain therefore
// re-triggers three times a second, and the ear hears a rhythm rather than a
// sustain. Longer grains and longer crossfades bury it; they cannot remove it,
// because repetition is the mechanism.
//
// So this does not replay audio at all. It takes an FFT of the captured moment,
// keeps the magnitude spectrum -- how much energy sits at each frequency --
// throws the timing away, and continuously resynthesises with fresh phase each
// frame, overlap-added back into a continuous signal. Nothing is replayed, so
// nothing can repeat. What is held is the harmonic content of the chord, which
// is why it sounds like a sustain rather than a loop.
//
// The cost is arithmetic: a 4096-point FFT every 1024 samples. On the target Pi
// that is a fraction of a callback that currently uses 0.08% of its budget, so
// the trade is real time for an artefact that could not otherwise be removed.
//
// Allocation happens once, in the constructor. capture() only sets a flag; the
// analysis runs at the top of the next process() call, on the audio thread,
// which is where every other state change in this project already happens.
class Freeze : public Effect {
public:
    explicit Freeze(float sample_rate);

    // Thread-safe from any thread.
    void capture();
    void release();
    void toggle();
    bool frozen() const { return published_frozen_.load(std::memory_order_relaxed); }

    void set_level(float level);      // pad level against the dry guitar
    void set_decay(float decay);      // 1 = holds forever, below 1 fades away
    void set_shimmer(float shimmer);  // 0 = phase advances coherently, 1 = fully random

    float level() const { return level_.load(std::memory_order_relaxed); }
    float decay() const { return decay_.load(std::memory_order_relaxed); }
    float shimmer() const { return shimmer_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

    static constexpr std::size_t kFftSize = 4096;  // ~85 ms at 48 kHz; 11.7 Hz bins
    static constexpr std::size_t kHop = kFftSize / 4;

private:
    void analyse();       // history_ -> magnitude_
    void synthesise();    // magnitude_ -> one frame overlap-added into out_
    void fft(std::vector<float>& re, std::vector<float>& im, bool inverse);

    float sample_rate_;

    // Rolling window of recent input. One hop longer than the transform so
    // capture() can analyse *two* overlapping frames and measure how far each
    // bin's phase actually moved between them -- see analyse().
    std::vector<float> history_;
    std::size_t history_pos_ = 0;

    std::vector<float> window_;     // Hann, applied on analysis and again on synthesis
    std::vector<float> magnitude_;  // kFftSize/2 + 1 bins, the frozen spectrum
    std::vector<float> phase_;      // running phase per bin, so shimmer=0 stays coherent
    std::vector<float> advance_;    // per-bin true phase advance per hop, measured at capture
    std::vector<std::size_t> peak_of_;  // which spectral peak each bin belongs to
    std::vector<float> offset_;         // bin's phase relative to its peak, fixed at capture

    std::vector<float> re_, im_;    // FFT scratch, sized once
    std::vector<std::size_t> bitrev_;
    std::vector<float> tw_cos_, tw_sin_;

    std::vector<float> out_;        // overlap-add ring, kFftSize long
    std::size_t out_pos_ = 0;
    std::size_t until_next_frame_ = 0;

    bool active_ = false;
    std::uint32_t rng_ = 0x9E3779B9u;  // xorshift; no allocation, no locks, no <random>

    std::atomic<bool> capture_pending_{false};
    std::atomic<bool> release_pending_{false};
    std::atomic<bool> published_frozen_{false};

    std::atomic<float> level_{0.7f};
    std::atomic<float> decay_{1.0f};
    std::atomic<float> shimmer_{0.85f};
};
