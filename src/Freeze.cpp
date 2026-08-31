#include "Freeze.h"

#include <algorithm>
#include <cmath>

Freeze::Freeze(float sample_rate, float max_grain_seconds) : sample_rate_(sample_rate) {
    const std::size_t max_grain =
        static_cast<std::size_t>(sample_rate * max_grain_seconds) + 1;
    // History has to be at least as long as the longest grain we might capture.
    history_.assign(max_grain, 0.0f);
    grain_.assign(max_grain, 0.0f);
}

void Freeze::capture() { capture_pending_.store(true, std::memory_order_relaxed); }
void Freeze::release() { release_pending_.store(true, std::memory_order_relaxed); }

void Freeze::toggle() {
    if (published_frozen_.load(std::memory_order_relaxed)) {
        release();
    } else {
        capture();
    }
}

void Freeze::set_grain_ms(float ms) {
    grain_ms_.store(std::clamp(ms, 40.0f, 1500.0f), std::memory_order_relaxed);
}
void Freeze::set_level(float level) {
    level_.store(std::clamp(level, 0.0f, 2.0f), std::memory_order_relaxed);
}
void Freeze::set_decay(float decay) {
    decay_.store(std::clamp(decay, 0.9f, 1.0f), std::memory_order_relaxed);
}

void Freeze::process(float* buffer, std::size_t n_frames) {
    if (release_pending_.exchange(false, std::memory_order_relaxed)) {
        active_ = false;
    }

    if (capture_pending_.exchange(false, std::memory_order_relaxed)) {
        std::size_t want = static_cast<std::size_t>(grain_ms_.load(std::memory_order_relaxed) *
                                                    0.001f * sample_rate_);
        want = std::clamp<std::size_t>(want, 32, history_.size());

        // Copy the most recent `want` samples out of the circular history,
        // oldest first. This is a copy within buffers already owned -- no
        // allocation on the audio thread.
        for (std::size_t i = 0; i < want; ++i) {
            const std::size_t src = (history_pos_ + history_.size() - want + i) % history_.size();
            grain_[i] = history_[src];
        }

        // Crossfade the grain into itself so the wrap does not click: the tail
        // fades out while a copy of the head fades in over the same span. The
        // fade is a twelfth of the grain, long enough to hide the seam and
        // short enough not to hollow out the middle.
        const std::size_t fade = std::max<std::size_t>(1, want / 12);
        for (std::size_t i = 0; i < fade; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(fade);
            const std::size_t tail = want - fade + i;
            grain_[tail] = grain_[tail] * (1.0f - t) + grain_[i] * t;
        }

        grain_len_ = want;
        read_a_ = 0;
        // The second head starts half a period away, so the two never wrap at
        // the same instant and the repeat stops being obvious.
        read_b_ = want / 2;
        gain_ = 1.0f;
        active_ = true;
    }

    const float level = level_.load(std::memory_order_relaxed);
    const float decay = decay_.load(std::memory_order_relaxed);

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float dry = buffer[i];

        history_[history_pos_] = dry;
        history_pos_ = (history_pos_ + 1) % history_.size();

        if (active_ && grain_len_ > 0) {
            // Each head is windowed by a raised cosine over its own position,
            // and the two are half a period apart, so their windows sum to a
            // near-constant -- the standard two-grain overlap-add.
            const float pa = static_cast<float>(read_a_) / static_cast<float>(grain_len_);
            const float pb = static_cast<float>(read_b_) / static_cast<float>(grain_len_);
            const float wa = 0.5f - 0.5f * std::cos(pa * 6.28318531f);
            const float wb = 0.5f - 0.5f * std::cos(pb * 6.28318531f);

            const float pad = (grain_[read_a_] * wa + grain_[read_b_] * wb) * gain_;
            buffer[i] = dry + pad * level;

            read_a_ = (read_a_ + 1) % grain_len_;
            read_b_ = (read_b_ + 1) % grain_len_;
            gain_ *= decay;
            if (gain_ < 1e-5f) active_ = false;
        }
    }

    published_frozen_.store(active_, std::memory_order_relaxed);
}
