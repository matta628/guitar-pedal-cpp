#pragma once

#include <atomic>
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

    State state() const { return state_; }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::vector<float> buffer_;
    std::size_t write_index_ = 0;
    std::size_t read_index_ = 0;
    std::size_t loop_length_ = 0;
    State state_ = State::Empty;

    std::atomic<bool> trigger_pending_{false};
    std::atomic<bool> clear_pending_{false};
    std::atomic<float> overdub_decay_{0.98f};
};
