#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include "Effect.h"

class Looper : public Effect {
public:
    enum class State { Empty, Recording, Playing, Overdubbing };

    // The buffer is allocated once, here, because the audio callback may not
    // allocate -- so the maximum length has to be chosen up front rather than
    // grown on demand. It is not a musical limit, only a memory one: mono
    // float at 48 kHz costs ~192 kB per second, so 120 s is ~23 MB.
    explicit Looper(float sample_rate, float max_loop_seconds = 120.0f);

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

    // --- saving and restoring loops -------------------------------------
    //
    // snapshot() copies the recorded loop out for the web thread to write to
    // disk. It refuses while Recording or Overdubbing, because those are the
    // only states in which the audio thread writes buffer_, and copying out
    // from under a writer is a data race. In Playing and Empty the audio
    // thread only reads, so a concurrent read is safe.
    bool snapshot(std::vector<float>* out) const;

    // load() is called from the web thread with a loop read off disk. The copy
    // happens *here*, on that thread, into a spare buffer allocated at
    // construction -- then the audio thread swaps the two vectors, which is a
    // pointer exchange and allocates nothing. That is the whole reason the
    // spare exists: without it, loading would either allocate in the callback
    // or memcpy up to 23 MB inside it, and both are ways to miss a deadline.
    //
    // Returns false if a previous load has not been picked up yet, or if the
    // loop is longer than the buffer this looper was built with.
    bool load(const std::vector<float>& samples);

    float sample_rate() const { return sample_rate_; }
    std::size_t capacity() const { return buffer_.size(); }

private:
    float sample_rate_ = 48000.0f;
    std::vector<float> buffer_;
    std::vector<float> spare_;          // staging for load(); swapped in, never allocated in process()
    std::size_t pending_load_length_ = 0;
    std::atomic<bool> load_pending_{false};
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
