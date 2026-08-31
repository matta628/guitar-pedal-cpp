#include "EnvFilter.h"

#include <algorithm>
#include <cmath>

namespace {
// Coefficients are recomputed every this many samples, not every sample.
//
// Biquad's header is explicit that set_lowpass() is a control-path call: it
// runs several transcendentals. An auto-wah needs a cutoff that moves
// continuously, so those two facts have to be reconciled rather than ignored.
// 32 samples is 1.5 kHz at 48 kHz -- far above the rate at which a picking
// envelope actually changes, so the sweep is smooth to the ear, while the
// per-sample cost drops to a thirty-second of the naive version. The envelope
// itself still updates every sample; only the filter design is throttled.
constexpr std::size_t kControlInterval = 32;
}  // namespace

EnvFilter::EnvFilter(float sample_rate) : sample_rate_(sample_rate) {
    filter_.set_lowpass(sample_rate_, base_hz_.load(std::memory_order_relaxed),
                        resonance_.load(std::memory_order_relaxed));
}

void EnvFilter::set_base_hz(float hz) {
    base_hz_.store(std::clamp(hz, 80.0f, 1500.0f), std::memory_order_relaxed);
}
void EnvFilter::set_range_hz(float hz) {
    range_hz_.store(std::clamp(hz, 0.0f, 6000.0f), std::memory_order_relaxed);
}
void EnvFilter::set_sensitivity(float s) {
    sensitivity_.store(std::clamp(s, -2.0f, 4.0f), std::memory_order_relaxed);
}
void EnvFilter::set_resonance(float q) {
    resonance_.store(std::clamp(q, 0.5f, 12.0f), std::memory_order_relaxed);
}
void EnvFilter::set_attack_ms(float ms) {
    attack_ms_.store(std::clamp(ms, 0.5f, 200.0f), std::memory_order_relaxed);
}
void EnvFilter::set_release_ms(float ms) {
    release_ms_.store(std::clamp(ms, 5.0f, 2000.0f), std::memory_order_relaxed);
}
void EnvFilter::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void EnvFilter::process(float* buffer, std::size_t n_frames) {
    const float base = base_hz_.load(std::memory_order_relaxed);
    const float range = range_hz_.load(std::memory_order_relaxed);
    const float sens = sensitivity_.load(std::memory_order_relaxed);
    const float q = resonance_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);

    // One-pole smoothing coefficients. exp(-1 / (t * fs)) is the per-sample
    // multiplier that decays to 1/e in t seconds.
    const float atk_ms = attack_ms_.load(std::memory_order_relaxed);
    const float rel_ms = release_ms_.load(std::memory_order_relaxed);
    const float atk = std::exp(-1.0f / (atk_ms * 0.001f * sample_rate_));
    const float rel = std::exp(-1.0f / (rel_ms * 0.001f * sample_rate_));

    const float nyquist = sample_rate_ * 0.5f;

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float dry = buffer[i];

        // Asymmetric follower: snap up on a transient, glide down after it.
        // Symmetric times here would chatter on every pick attack.
        const float rectified = std::fabs(dry);
        const float coeff = (rectified > env_) ? atk : rel;
        env_ = rectified + coeff * (env_ - rectified);

        if (i % kControlInterval == 0) {
            // Negative sensitivity is allowed and inverts the sweep, so the
            // filter closes as you dig in -- the "down wah" sound.
            float cutoff = base + range * env_ * sens;
            // Clamped well inside Nyquist: an RBJ lowpass designed at or above
            // it produces garbage coefficients rather than a steep filter.
            cutoff = std::clamp(cutoff, 40.0f, nyquist * 0.9f);
            if (std::fabs(cutoff - last_cutoff_) > 0.5f) {
                filter_.set_lowpass(sample_rate_, cutoff, q);
                last_cutoff_ = cutoff;
            }
        }

        const float wet = filter_.process(dry);
        buffer[i] = dry * (1.0f - mix) + wet * mix;
    }

    published_env_.store(env_, std::memory_order_relaxed);
}
