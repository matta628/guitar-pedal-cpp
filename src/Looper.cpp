#include "Looper.h"

#include <algorithm>

Looper::Looper(float sample_rate, float max_loop_seconds)
    : sample_rate_(sample_rate),
      buffer_(static_cast<std::size_t>(max_loop_seconds * sample_rate), 0.0f),
      spare_(static_cast<std::size_t>(max_loop_seconds * sample_rate), 0.0f) {}

bool Looper::snapshot(std::vector<float>* out) const {
    const State s = published_state_.load(std::memory_order_relaxed);
    if (s == State::Recording || s == State::Overdubbing) return false;
    const std::size_t n = published_length_.load(std::memory_order_relaxed);
    if (n == 0 || n > buffer_.size()) return false;
    out->assign(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(n));
    return true;
}

bool Looper::load(const std::vector<float>& samples) {
    if (load_pending_.load(std::memory_order_acquire)) return false;  // one in flight already
    if (samples.empty() || samples.size() > spare_.size()) return false;
    std::copy(samples.begin(), samples.end(), spare_.begin());
    pending_load_length_ = samples.size();
    load_pending_.store(true, std::memory_order_release);
    return true;
}

void Looper::on_trigger() { trigger_pending_.store(true, std::memory_order_relaxed); }

void Looper::clear() { clear_pending_.store(true, std::memory_order_relaxed); }

void Looper::set_overdub_decay(float decay) {
    overdub_decay_.store(std::clamp(decay, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Looper::set_level(float level) {
    level_.store(level < 0.0f ? 0.0f : level, std::memory_order_relaxed);
}

void Looper::process(float* buffer, std::size_t n_frames) {
    if (load_pending_.load(std::memory_order_acquire)) {
        // vector::swap exchanges internal pointers. No allocation, no copy, and
        // the old contents leave with the spare, which is scratch anyway.
        buffer_.swap(spare_);
        loop_length_ = pending_load_length_;
        write_index_ = loop_length_;
        read_index_ = 0;
        state_ = State::Playing;
        trigger_pending_.store(false, std::memory_order_relaxed);
        load_pending_.store(false, std::memory_order_release);
    }

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
    const float level = level_.load(std::memory_order_relaxed);

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
            buffer[i] += buffer_[read_index_] * level;
            read_index_ = (read_index_ + 1) % loop_length_;
        } else if (state_ == State::Overdubbing && loop_length_ > 0) {
            const float existing = buffer_[read_index_];
            // The stored signal is deliberately scaled by decay only, never by
            // level: what is written back has to be independent of monitoring
            // volume, or turning the loop down would erase it a pass at a time.
            buffer_[read_index_] = existing * decay + buffer[i];
            buffer[i] += existing * level;
            read_index_ = (read_index_ + 1) % loop_length_;
        }
    }

    published_state_.store(state_, std::memory_order_relaxed);
    published_length_.store(state_ == State::Recording ? write_index_ : loop_length_,
                            std::memory_order_relaxed);
    published_position_.store(state_ == State::Recording ? write_index_ : read_index_,
                              std::memory_order_relaxed);
}
