#include "Tone.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kBassHz = 200.0f;
constexpr float kTrebleHz = 3000.0f;
constexpr float kShelfSlope = 0.7f;
}  // namespace

void Tone::set_bass_db(float db) {
    bass_db_.store(std::clamp(db, -18.0f, 18.0f), std::memory_order_relaxed);
}
void Tone::set_mid_db(float db) {
    mid_db_.store(std::clamp(db, -18.0f, 18.0f), std::memory_order_relaxed);
}
void Tone::set_mid_hz(float hz) {
    mid_hz_.store(std::clamp(hz, 200.0f, 3000.0f), std::memory_order_relaxed);
}
void Tone::set_mid_q(float q) {
    mid_q_.store(std::clamp(q, 0.3f, 8.0f), std::memory_order_relaxed);
}
void Tone::set_treble_db(float db) {
    treble_db_.store(std::clamp(db, -18.0f, 18.0f), std::memory_order_relaxed);
}

void Tone::recompute() {
    low_.set_low_shelf(sample_rate_, kBassHz, cached_bass_, kShelfSlope);
    mid_.set_peaking(sample_rate_, cached_mid_hz_, cached_mid_, cached_mid_q_);
    high_.set_high_shelf(sample_rate_, kTrebleHz, cached_treble_, kShelfSlope);
}

void Tone::process(float* buffer, std::size_t n_frames) {
    const float bass = bass_db_.load(std::memory_order_relaxed);
    const float mid = mid_db_.load(std::memory_order_relaxed);
    const float mid_hz = mid_hz_.load(std::memory_order_relaxed);
    const float mid_q = mid_q_.load(std::memory_order_relaxed);
    const float treble = treble_db_.load(std::memory_order_relaxed);

    if (bass != cached_bass_ || mid != cached_mid_ || mid_hz != cached_mid_hz_ ||
        mid_q != cached_mid_q_ || treble != cached_treble_ || sample_rate_ != cached_rate_) {
        cached_bass_ = bass;
        cached_mid_ = mid;
        cached_mid_hz_ = mid_hz;
        cached_mid_q_ = mid_q;
        cached_treble_ = treble;
        cached_rate_ = sample_rate_;
        // Six transcendentals, on the one buffer after a knob moves. Bounded
        // and rare — but it is real-time work, so it is worth knowing it is
        // here rather than being surprised by it in a latency trace.
        recompute();
    }

    for (std::size_t i = 0; i < n_frames; ++i) {
        buffer[i] = high_.process(mid_.process(low_.process(buffer[i])));
    }
}
