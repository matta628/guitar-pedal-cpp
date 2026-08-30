#pragma once

#include <atomic>
#include <vector>

#include "Effect.h"

// A delay line with feedback, plus the two things that separate a tape echo
// from a digital one.
//
//  * tone — a lowpass in the *feedback path*, so each repeat is darker than
//           the one before it. Filtering the output tap instead would make
//           every repeat equally dull; putting it in the loop is what makes an
//           Echoplex or a Copicat decay into mud rather than just fade.
//  * modulation — tape does not move at a constant speed. A slow wobble on the
//           delay time is the wow and flutter of a real transport, and it is a
//           large part of why tape repeats sound alive next to digital ones.
//
// With both at zero this is an ordinary digital delay, which is what the
// Deftones and Radiohead presets want; turned up it is the Echoplex the Led
// Zeppelin preset needs.
class Delay : public Effect {
public:
    Delay(float sample_rate, float max_delay_seconds = 2.0f);

    void set_delay_seconds(float seconds);
    void set_feedback(float feedback);
    void set_mix(float mix);
    void set_tone(float tone);              // 1 = open, 0 = each repeat much darker
    void set_modulation_ms(float depth_ms); // wow/flutter depth, 0 = digital

    float delay_seconds() const {
        return delay_samples_.load(std::memory_order_relaxed) / sample_rate_;
    }
    float feedback() const { return feedback_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }
    float tone() const { return tone_.load(std::memory_order_relaxed); }
    float modulation_ms() const { return modulation_ms_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    float sample_rate_;
    std::vector<float> buffer_;
    std::size_t pos_ = 0;
    float phase_ = 0.0f;          // wow/flutter LFO
    float feedback_filter_ = 0.0f;

    std::atomic<float> delay_samples_;
    std::atomic<float> feedback_{0.35f};
    std::atomic<float> mix_{0.35f};
    std::atomic<float> tone_{1.0f};
    std::atomic<float> modulation_ms_{0.0f};
};
