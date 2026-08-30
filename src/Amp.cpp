#include "Amp.h"

#include <algorithm>
#include <cmath>

namespace {
// Tone stack centres, roughly where a Fender-style passive stack puts them.
constexpr float kBassHz = 120.0f;
constexpr float kMidHz = 650.0f;
constexpr float kMidQ = 0.7f;
constexpr float kTrebleHz = 2800.0f;
constexpr float kPresenceHz = 4000.0f;
}  // namespace

void Amp::set_gain(float gain) {
    gain_.store(std::clamp(gain, 1.0f, 50.0f), std::memory_order_relaxed);
}
void Amp::set_bass_db(float db) {
    bass_db_.store(std::clamp(db, -12.0f, 12.0f), std::memory_order_relaxed);
}
void Amp::set_mid_db(float db) {
    mid_db_.store(std::clamp(db, -12.0f, 12.0f), std::memory_order_relaxed);
}
void Amp::set_treble_db(float db) {
    treble_db_.store(std::clamp(db, -12.0f, 12.0f), std::memory_order_relaxed);
}
void Amp::set_master(float master) {
    master_.store(std::clamp(master, 1.0f, 20.0f), std::memory_order_relaxed);
}
void Amp::set_presence_db(float db) {
    presence_db_.store(std::clamp(db, -6.0f, 12.0f), std::memory_order_relaxed);
}
void Amp::set_volume(float volume) {
    volume_.store(std::clamp(volume, 0.0f, 1.5f), std::memory_order_relaxed);
}
void Amp::set_cab(float cab) {
    cab_.store(std::clamp(cab, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Amp::recompute() {
    low_.set_low_shelf(sample_rate_, kBassHz, cached_bass_);
    mid_.set_peaking(sample_rate_, kMidHz, cached_mid_, kMidQ);
    high_.set_high_shelf(sample_rate_, kTrebleHz, cached_treble_);
    presence_.set_high_shelf(sample_rate_, kPresenceHz, cached_presence_);
    // The cab knob sweeps the speaker's corner from 12 kHz (barely there) down
    // to 2.5 kHz (a closed-back 4x12). Q above 0.707 leaves a small resonant
    // bump just before the rolloff, which real guitar speakers have and which
    // is a surprising amount of why they sound like speakers.
    const float corner = 12000.0f * std::pow(2500.0f / 12000.0f, cached_cab_);
    speaker_.set_lowpass(sample_rate_, corner, 0.9f);
}

void Amp::process(float* buffer, std::size_t n_frames) {
    const float gain = gain_.load(std::memory_order_relaxed);
    const float bass = bass_db_.load(std::memory_order_relaxed);
    const float mid = mid_db_.load(std::memory_order_relaxed);
    const float treble = treble_db_.load(std::memory_order_relaxed);
    const float master = master_.load(std::memory_order_relaxed);
    const float presence = presence_db_.load(std::memory_order_relaxed);
    const float volume = volume_.load(std::memory_order_relaxed);
    const float cab = cab_.load(std::memory_order_relaxed);

    if (bass != cached_bass_ || mid != cached_mid_ || treble != cached_treble_ ||
        presence != cached_presence_ || cab != cached_cab_ || sample_rate_ != cached_rate_) {
        cached_bass_ = bass;
        cached_mid_ = mid;
        cached_treble_ = treble;
        cached_presence_ = presence;
        cached_cab_ = cab;
        cached_rate_ = sample_rate_;
        recompute();
    }

    // Both gain stages are normalised by their own drive so that turning gain
    // up makes the sound dirtier without also making it 30 dB louder. Without
    // this the gain knob would be unusable — every adjustment would need a
    // matching volume correction.
    const float preamp_norm = 1.0f / std::tanh(gain);
    const float power_norm = 1.0f / std::tanh(master);

    for (std::size_t i = 0; i < n_frames; ++i) {
        float x = std::tanh(gain * buffer[i]) * preamp_norm;

        x = high_.process(mid_.process(low_.process(x)));

        // Asymmetric on purpose: a real power stage clips its two halves
        // differently, and the even harmonics that produces are most of the
        // difference between "distorted" and "driven".
        x = std::tanh(master * x + 0.06f) * power_norm;

        x = speaker_.process(presence_.process(x));

        buffer[i] = x * volume;
    }
}
