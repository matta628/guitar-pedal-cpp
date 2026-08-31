#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include "Effect.h"

class Looper : public Effect {
public:
    enum class State { Empty, Recording, Playing, Overdubbing };

    explicit Looper(float sample_rate, float max_loop_seconds = 30.0f);

    // Safe to call from any thread (e.g. a keyboard/GPIO listener). Only sets
    // a flag; the actual state transition happens on the audio thread at the
    // top of the next process() call.
    //
    // Cycle: Empty -> Recording -> Playing -> Overdubbing -> Playing -> Overdubbing -> ...
    void on_trigger();

    // Also thread-safe to call from any thread. Resets to Empty from any
    // state, regardless of what on_trigger() would otherwise do next.
    void clear();

    void set_overdub_decay(float decay);

    // Playback level for the recorded loop, 0 = silent, 1 = unity. Applies to
    // what you HEAR, never to what is stored: turning it down while
    // overdubbing must not quietly erase the loop.
    void set_level(float level);

    // Reads the state published at the end of the last process() call, so a
    // non-audio thread (the LED indicator loop) can poll it without racing
    // the audio thread's plain state_ member.
    State state() const { return published_state_.load(std::memory_order_relaxed); }

    float overdub_decay() const { return overdub_decay_.load(std::memory_order_relaxed); }

    float level() const { return level_.load(std::memory_order_relaxed); }

    // Published alongside state() at the end of each process() call, for the
    // same reason: an observer thread must not read the audio thread's plain
    // members. While recording, length() reports how much has been captured so
    // far, so a progress display grows in real time instead of sitting at zero.
    std::size_t length() const { return published_length_.load(std::memory_order_relaxed); }
    std::size_t position() const { return published_position_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::vector<float> buffer_;
    std::size_t write_index_ = 0;
    std::size_t read_index_ = 0;
    std::size_t loop_length_ = 0;
    State state_ = State::Empty;
    std::atomic<State> published_state_{State::Empty};
    std::atomic<std::size_t> published_length_{0};
    std::atomic<std::size_t> published_position_{0};

    std::atomic<bool> trigger_pending_{false};
    std::atomic<bool> clear_pending_{false};
    std::atomic<float> overdub_decay_{0.98f};
    std::atomic<float> level_{1.0f};
};
