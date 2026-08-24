#include "Reverb.h"

#include <algorithm>

namespace {
// Freeverb's tuned comb/allpass delay lengths, in samples at 44100 Hz.
constexpr int kCombTunings[] = {1116, 1188, 1277, 1356, 1422, 1497, 1617, 1557};
constexpr int kAllpassTunings[] = {556, 441, 341, 225};
}  // namespace

float Reverb::CombFilter::process(float input) {
    const float output = buffer[pos];
    filterstore = output * (1.0f - damping) + filterstore * damping;
    buffer[pos] = input + filterstore * feedback;
    pos = (pos + 1) % buffer.size();
    return output;
}

float Reverb::AllpassFilter::process(float input) {
    const float bufout = buffer[pos];
    const float output = -input + bufout;
    buffer[pos] = input + bufout * kFeedback;
    pos = (pos + 1) % buffer.size();
    return output;
}

Reverb::Reverb(float sample_rate) {
    const float scale = sample_rate / 44100.0f;

    for (int tuning : kCombTunings) {
        CombFilter comb;
        comb.buffer.assign(static_cast<std::size_t>(static_cast<float>(tuning) * scale), 0.0f);
        combs_.push_back(std::move(comb));
    }
    for (int tuning : kAllpassTunings) {
        AllpassFilter allpass;
        allpass.buffer.assign(static_cast<std::size_t>(static_cast<float>(tuning) * scale), 0.0f);
        allpasses_.push_back(std::move(allpass));
    }
}

void Reverb::set_room_size(float room_size) {
    room_size_.store(std::clamp(room_size, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Reverb::set_damping(float damping) {
    damping_.store(std::clamp(damping, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Reverb::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Reverb::process(float* buffer, std::size_t n_frames) {
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

        buffer[i] = dry * (1.0f - mix) + sum * mix;
    }
}
