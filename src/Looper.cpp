#include "Looper.h"

#include <algorithm>

Looper::Looper(float sample_rate, float max_loop_seconds)
    : buffer_(static_cast<std::size_t>(max_loop_seconds * sample_rate), 0.0f) {}

void Looper::on_trigger() { trigger_pending_.store(true, std::memory_order_relaxed); }

void Looper::clear() { clear_pending_.store(true, std::memory_order_relaxed); }

void Looper::set_overdub_decay(float decay) {
    overdub_decay_.store(std::clamp(decay, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Looper::process(float* buffer, std::size_t n_frames) {
    if (clear_pending_.exchange(false, std::memory_order_relaxed)) {
        write_index_ = 0;
        read_index_ = 0;
        loop_length_ = 0;
        state_ = State::Empty;
        trigger_pending_.store(false, std::memory_order_relaxed);
    } else if (trigger_pending_.exchange(false, std::memory_order_relaxed)) {
        switch (state_) {
            case State::Empty:
                write_index_ = 0;
                state_ = State::Recording;
                break;
            case State::Recording:
                loop_length_ = write_index_;
                read_index_ = 0;
                state_ = State::Playing;
                break;
            case State::Playing:
                state_ = State::Overdubbing;
                break;
            case State::Overdubbing:
                state_ = State::Playing;
                break;
        }
    }

    const float decay = overdub_decay_.load(std::memory_order_relaxed);

    for (std::size_t i = 0; i < n_frames; ++i) {
        if (state_ == State::Recording) {
            if (write_index_ < buffer_.size()) {
                buffer_[write_index_] = buffer[i];
                ++write_index_;
            } else {
                loop_length_ = write_index_;
                read_index_ = 0;
                state_ = State::Playing;
            }
        } else if (state_ == State::Playing && loop_length_ > 0) {
            buffer[i] += buffer_[read_index_];
            read_index_ = (read_index_ + 1) % loop_length_;
        } else if (state_ == State::Overdubbing && loop_length_ > 0) {
            const float existing = buffer_[read_index_];
            buffer_[read_index_] = existing * decay + buffer[i];
            buffer[i] += existing;
            read_index_ = (read_index_ + 1) % loop_length_;
        }
    }

    published_state_.store(state_, std::memory_order_relaxed);
    published_length_.store(state_ == State::Recording ? write_index_ : loop_length_,
                            std::memory_order_relaxed);
    published_position_.store(state_ == State::Recording ? write_index_ : read_index_,
                              std::memory_order_relaxed);
}
