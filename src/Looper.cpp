#include "Looper.h"

Looper::Looper(float sample_rate, float max_loop_seconds)
    : buffer_(static_cast<std::size_t>(max_loop_seconds * sample_rate), 0.0f) {}

void Looper::on_trigger() { trigger_pending_.store(true, std::memory_order_relaxed); }

void Looper::process(float* buffer, std::size_t n_frames) {
    if (trigger_pending_.exchange(false, std::memory_order_relaxed)) {
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
                write_index_ = 0;
                read_index_ = 0;
                loop_length_ = 0;
                state_ = State::Empty;
                break;
        }
    }

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
        }
        if (state_ == State::Playing && loop_length_ > 0) {
            buffer[i] += buffer_[read_index_];
            read_index_ = (read_index_ + 1) % loop_length_;
        }
    }
}
