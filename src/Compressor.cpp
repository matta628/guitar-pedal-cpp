#include "Compressor.h"

#include <algorithm>
#include <cmath>

void Compressor::set_threshold_db(float db) {
    threshold_db_.store(std::clamp(db, -60.0f, 0.0f), std::memory_order_relaxed);
}
void Compressor::set_ratio(float ratio) {
    ratio_.store(std::clamp(ratio, 1.0f, 20.0f), std::memory_order_relaxed);
}
void Compressor::set_attack_ms(float ms) {
    attack_ms_.store(std::clamp(ms, 0.1f, 200.0f), std::memory_order_relaxed);
}
void Compressor::set_release_ms(float ms) {
    release_ms_.store(std::clamp(ms, 5.0f, 2000.0f), std::memory_order_relaxed);
}
void Compressor::set_makeup_db(float db) {
    makeup_db_.store(std::clamp(db, -12.0f, 24.0f), std::memory_order_relaxed);
}

void Compressor::process(float* buffer, std::size_t n_frames) {
    const float threshold = std::pow(10.0f, threshold_db_.load(std::memory_order_relaxed) / 20.0f);
    const float ratio = ratio_.load(std::memory_order_relaxed);
    const float makeup = std::pow(10.0f, makeup_db_.load(std::memory_order_relaxed) / 20.0f);

    // Time constants as one-pole coefficients. exp() twice per buffer, not per
    // sample — the knobs cannot move inside a buffer.
    const float attack =
        1.0f - std::exp(-1.0f / (0.001f * attack_ms_.load(std::memory_order_relaxed) * sample_rate_));
    const float release =
        1.0f - std::exp(-1.0f / (0.001f * release_ms_.load(std::memory_order_relaxed) * sample_rate_));

    // Everything above the threshold is scaled by this exponent. Working in
    // the linear domain rather than converting to dB and back saves a log per
    // sample; one pow() remains, which at ~30 ns and 256 frames is under 8 us
    // of a 5333 us budget.
    const float exponent = 1.0f - 1.0f / ratio;

    float min_gain = 1.0f;

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float rectified = std::fabs(buffer[i]);
        // Attack when the signal is rising, release when it is falling: a
        // single coefficient would either miss transients or pump on decays.
        envelope_ += (rectified > envelope_ ? attack : release) * (rectified - envelope_);

        float gain = 1.0f;
        if (envelope_ > threshold && envelope_ > 1e-9f) {
            gain = std::pow(threshold / envelope_, exponent);
        }
        min_gain = std::min(min_gain, gain);

        buffer[i] = buffer[i] * gain * makeup;
    }

    reduction_db_.store(20.0f * std::log10(std::max(min_gain, 1e-6f)), std::memory_order_relaxed);
}
