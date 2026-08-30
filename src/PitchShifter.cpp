#include "PitchShifter.h"

#include <algorithm>
#include <cmath>

namespace {

// 50 ms. The trade is direct: a longer window splices less often, so the
// warble is slower and less busy, but the two taps are further apart in time
// and the result smears transients. 50 ms is about where a picked guitar note
// still sounds picked.
constexpr float kWindowSeconds = 0.05f;
constexpr float kTwoPi = 6.28318530718f;

}  // namespace

PitchShifter::PitchShifter(float sample_rate)
    : window_(static_cast<std::size_t>(kWindowSeconds * sample_rate)) {
    // Twice the window plus slack: the furthest either tap ever looks back is
    // one whole window, and linear interpolation reads one sample past that.
    buffer_.assign(window_ * 2 + 8, 0.0f);
}

void PitchShifter::set_semitones(float semitones) {
    semitones_.store(std::clamp(semitones, -24.0f, 24.0f), std::memory_order_relaxed);
}

void PitchShifter::set_cents(float cents) {
    cents_.store(std::clamp(cents, -50.0f, 50.0f), std::memory_order_relaxed);
}

void PitchShifter::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void PitchShifter::process(float* buffer, std::size_t n_frames) {
    const float semitones = semitones_.load(std::memory_order_relaxed) +
                            cents_.load(std::memory_order_relaxed) * 0.01f;
    const float mix = mix_.load(std::memory_order_relaxed);
    const std::size_t size = buffer_.size();
    const float window = static_cast<float>(window_);

    // Twelve-tone equal temperament: an octave is a doubling, and each of the
    // twelve steps is the twelfth root of two. One pow per buffer.
    const float ratio = std::pow(2.0f, semitones / 12.0f);
    // Write advances one sample per sample and read must advance `ratio`, so
    // the gap between them changes by exactly this much each sample.
    const float drift = 1.0f - ratio;

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float dry = buffer[i];
        buffer_[pos_] = dry;

        // Tap two sits half a window away from tap one, so exactly one of them
        // is ever near a wrap point.
        float offset0 = offset_;
        float offset1 = offset_ + window * 0.5f;
        if (offset1 >= window) offset1 -= window;

        const float u = offset_ / window;
        // Hann crossfade. The second gain is 1 - gain0 rather than a second
        // cosine, because the taps are exactly half a window apart and
        // cos(x + pi) == -cos(x). Constant sum, one transcendental per sample.
        const float gain0 = 0.5f - 0.5f * std::cos(kTwoPi * u);
        const float gain1 = 1.0f - gain0;

        float wet = 0.0f;
        for (int tap = 0; tap < 2; ++tap) {
            const float offset = (tap == 0) ? offset0 : offset1;
            float read_pos = static_cast<float>(pos_) - offset;
            while (read_pos < 0.0f) read_pos += static_cast<float>(size);

            const auto idx0 = static_cast<std::size_t>(read_pos) % size;
            const std::size_t idx1 = (idx0 + 1) % size;
            const float frac = read_pos - std::floor(read_pos);
            const float sample = buffer_[idx0] * (1.0f - frac) + buffer_[idx1] * frac;

            wet += sample * ((tap == 0) ? gain0 : gain1);
        }

        buffer[i] = dry * (1.0f - mix) + wet * mix;

        pos_ = (pos_ + 1) % size;
        offset_ += drift;
        // Wrap rather than let the read pointer overtake the write pointer
        // (pitch up) or fall a whole buffer behind it (pitch down). The
        // crossfade gain is zero at both ends of this range, so the jump is
        // silent.
        if (offset_ >= window) offset_ -= window;
        if (offset_ < 0.0f) offset_ += window;
    }
}
