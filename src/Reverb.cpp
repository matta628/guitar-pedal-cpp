#include "Reverb.h"

#include <algorithm>
#include <cmath>

namespace {

// Freeverb's tuned comb/allpass delay lengths, in samples at 44100 Hz. They
// are mutually prime on purpose: shared factors would make the combs reinforce
// each other at the same instants and the tail would ring rather than diffuse.
constexpr int kCombTunings[] = {1116, 1188, 1277, 1356, 1422, 1497, 1617, 1557};
constexpr int kAllpassTunings[] = {556, 441, 341, 225};

// How much longer or shorter than Freeverb's default each space is, and the
// band its wet signal is squeezed into.
struct ModeSpec {
    float size_scale;
    float lowpass_hz;
    float highpass_hz;
};

constexpr ModeSpec kModes[] = {
    /* Room   */ {0.62f, 6500.0f, 120.0f},
    /* Plate  */ {0.85f, 9000.0f, 250.0f},
    /* Hall   */ {1.35f, 4800.0f, 90.0f},
    // A spring tank is short, resonant and famously band-limited — almost
    // nothing below 200 Hz or above 3 kHz survives it, which is most of why
    // it sounds like a spring rather than a room.
    /* Spring */ {0.34f, 3000.0f, 320.0f},
};

// The largest size_scale above. The buffers are allocated at this so any mode
// fits without reallocating.
constexpr float kMaxSizeScale = 1.35f;

}  // namespace

float Reverb::CombFilter::process(float input) {
    const float output = buffer[pos];
    filterstore = output * (1.0f - damping) + filterstore * damping;
    buffer[pos] = input + filterstore * feedback;
    pos = (pos + 1) % length;
    return output;
}

float Reverb::AllpassFilter::process(float input) {
    const float bufout = buffer[pos];
    const float output = -input + bufout;
    buffer[pos] = input + bufout * kFeedback;
    pos = (pos + 1) % length;
    return output;
}

Reverb::Reverb(float sample_rate) : sample_rate_(sample_rate) {
    const float scale = sample_rate / 44100.0f;

    for (int tuning : kCombTunings) {
        CombFilter comb;
        comb.buffer.assign(
            static_cast<std::size_t>(static_cast<float>(tuning) * scale * kMaxSizeScale) + 1, 0.0f);
        combs_.push_back(std::move(comb));
    }
    for (int tuning : kAllpassTunings) {
        AllpassFilter allpass;
        allpass.buffer.assign(
            static_cast<std::size_t>(static_cast<float>(tuning) * scale * kMaxSizeScale) + 1, 0.0f);
        allpasses_.push_back(std::move(allpass));
    }

    apply_mode(mode_.load(std::memory_order_relaxed));
}

void Reverb::set_mode(Mode mode) { mode_.store(mode, std::memory_order_relaxed); }

void Reverb::set_room_size(float room_size) {
    room_size_.store(std::clamp(room_size, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Reverb::set_damping(float damping) {
    damping_.store(std::clamp(damping, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Reverb::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Reverb::apply_mode(Mode mode) {
    const ModeSpec& spec = kModes[static_cast<int>(mode)];
    const float scale = sample_rate_ / 44100.0f;

    for (std::size_t i = 0; i < combs_.size(); ++i) {
        const auto length = static_cast<std::size_t>(
            static_cast<float>(kCombTunings[i]) * scale * spec.size_scale);
        // Never past what was allocated, and never zero — the modulo in
        // process() would divide by it.
        combs_[i].length = std::clamp<std::size_t>(length, 1, combs_[i].buffer.size());
        combs_[i].pos %= combs_[i].length;
    }
    for (std::size_t i = 0; i < allpasses_.size(); ++i) {
        const auto length = static_cast<std::size_t>(
            static_cast<float>(kAllpassTunings[i]) * scale * spec.size_scale);
        allpasses_[i].length = std::clamp<std::size_t>(length, 1, allpasses_[i].buffer.size());
        allpasses_[i].pos %= allpasses_[i].length;
    }

    // One-pole coefficients for the wet-path band.
    lowpass_coeff_ = 1.0f - std::exp(-6.28318530718f * spec.lowpass_hz / sample_rate_);
    highpass_coeff_ = 1.0f - std::exp(-6.28318530718f * spec.highpass_hz / sample_rate_);

    applied_mode_ = mode;
    mode_applied_ = true;
}

void Reverb::process(float* buffer, std::size_t n_frames) {
    const Mode mode = mode_.load(std::memory_order_relaxed);
    if (!mode_applied_ || mode != applied_mode_) {
        // Retuning the network, not resizing it: this walks twelve filters and
        // computes two exponentials. Bounded, and only on the buffer after the
        // mode actually changes.
        apply_mode(mode);
    }

    const float room_size = room_size_.load(std::memory_order_relaxed);
    const float damping = damping_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);
    const float feedback = 0.7f + room_size * 0.28f;

    for (auto& comb : combs_) {
        comb.feedback = feedback;
        comb.damping = damping;
    }

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float dry = buffer[i];

        float sum = 0.0f;
        for (auto& comb : combs_) {
            sum += comb.process(dry);
        }
        sum /= static_cast<float>(combs_.size());

        for (auto& allpass : allpasses_) {
            sum = allpass.process(sum);
        }

        lowpass_state_ += lowpass_coeff_ * (sum - lowpass_state_);
        highpass_state_ += highpass_coeff_ * (lowpass_state_ - highpass_state_);
        // Lowpass minus its own lowpass is a bandpass: what the mode's two
        // corner frequencies leave behind.
        const float wet = lowpass_state_ - highpass_state_;

        buffer[i] = dry * (1.0f - mix) + wet * mix;
    }
}
