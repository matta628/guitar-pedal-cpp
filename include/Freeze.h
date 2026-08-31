#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include "Effect.h"

// Captures a slice of what you just played and loops it as a sustained pad
// while you keep playing over the top.
//
// Different from the Looper in the one way that matters: the looper is a
// performance tool you drive deliberately with your foot and it records what
// you MEAN to record. Freeze grabs whatever happened to be in the buffer the
// instant you hit it -- a fraction of a second -- and holds it indefinitely.
// You use it to turn a chord you just played into a drone, not to build an
// arrangement.
//
// Two problems have to be solved to make a short loop sound like a pad:
//
//  * A slice that ends where it did not begin clicks on every wrap. So the
//    grain is crossfaded into itself: the last few milliseconds fade out while
//    the first few fade in, and the seam stops being audible.
//  * A single grain repeating at an exact period sounds obviously, mechanically
//    looped. So two grains run half a period out of phase and are summed, which
//    smears the repeat into something closer to a sustain.
//
// Allocation happens once, in the constructor. Engaging freeze on the audio
// thread only flips a flag and copies within the buffer already owned.
class Freeze : public Effect {
public:
    explicit Freeze(float sample_rate, float max_grain_seconds = 1.5f);

    // Thread-safe from any thread. Capture takes the most recent `grain`
    // seconds; release stops the pad.
    void capture();
    void release();
    void toggle();

    bool frozen() const { return published_frozen_.load(std::memory_order_relaxed); }

    void set_grain_ms(float ms);   // 40 .. 1500
    void set_level(float level);   // pad level against the dry guitar
    void set_decay(float decay);   // 1 = holds forever, below 1 fades away

    float grain_ms() const { return grain_ms_.load(std::memory_order_relaxed); }
    float level() const { return level_.load(std::memory_order_relaxed); }
    float decay() const { return decay_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    float sample_rate_;

    // A rolling window of recent input, so capture() has something to grab.
    // Sized once; never resized.
    std::vector<float> history_;
    std::size_t history_pos_ = 0;

    // The captured grain, played back by two read heads half a period apart.
    std::vector<float> grain_;
    std::size_t grain_len_ = 0;
    std::size_t read_a_ = 0;
    std::size_t read_b_ = 0;
    float gain_ = 1.0f;

    bool active_ = false;

    std::atomic<bool> capture_pending_{false};
    std::atomic<bool> release_pending_{false};
    std::atomic<bool> published_frozen_{false};

    std::atomic<float> grain_ms_{300.0f};
    std::atomic<float> level_{0.7f};
    std::atomic<float> decay_{1.0f};
};
