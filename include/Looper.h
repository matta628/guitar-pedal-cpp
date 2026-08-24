#pragma once

#include <atomic>
#include <vector>

#include "Effect.h"

class Looper : public Effect {
public:
    enum class State { Empty, Recording, Playing };

    explicit Looper(float sample_rate, float max_loop_seconds = 30.0f);

    // Safe to call from any thread (e.g. a keyboard/GPIO listener). Only sets
    // a flag; the actual state transition happens on the audio thread at the
    // top of the next process() call.
    void on_trigger();

    State state() const { return state_; }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::vector<float> buffer_;
    std::size_t write_index_ = 0;
    std::size_t read_index_ = 0;
    std::size_t loop_length_ = 0;
    State state_ = State::Empty;

    std::atomic<bool> trigger_pending_{false};
};
