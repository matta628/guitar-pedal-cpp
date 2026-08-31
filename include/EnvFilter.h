#pragma once

#include <atomic>

#include "Biquad.h"
#include "Effect.h"

// An auto-wah: a resonant lowpass whose cutoff is dragged around by how hard
// you are playing.
//
// This is the only effect here whose sound is controlled by the guitar rather
// than by a knob or an LFO. A wah pedal needs a foot; a tremolo or phaser
// sweeps on a timer regardless of what you play. Here the envelope follower
// measures the signal and the filter follows it, so digging in opens the
// filter and backing off closes it -- the effect plays you back.
//
// The follower is deliberately asymmetric, and that asymmetry is the whole
// feel of the effect. Attack is fast so a pick attack snaps the filter open on
// the transient; release is slow so it glides shut over the note's decay
// rather than chattering. Equal times would sound like a broken gate.
//
// `sensitivity` scales how far a given input level moves the cutoff, and
// `range` sets how far it can travel from `base`. Negative sensitivity
// inverts it -- the filter closes when you dig in, which is the "down wah"
// sound and is much stranger.
class EnvFilter : public Effect {
public:
    explicit EnvFilter(float sample_rate);

    void set_base_hz(float hz);          // resting cutoff, 80 .. 1500
    void set_range_hz(float hz);         // how far the envelope can push it
    void set_sensitivity(float s);       // -2 .. 4; negative inverts the sweep
    void set_resonance(float q);         // 0.5 .. 12, the vocal "quack"
    void set_attack_ms(float ms);
    void set_release_ms(float ms);
    void set_mix(float mix);

    float base_hz() const { return base_hz_.load(std::memory_order_relaxed); }
    float range_hz() const { return range_hz_.load(std::memory_order_relaxed); }
    float sensitivity() const { return sensitivity_.load(std::memory_order_relaxed); }
    float resonance() const { return resonance_.load(std::memory_order_relaxed); }
    float attack_ms() const { return attack_ms_.load(std::memory_order_relaxed); }
    float release_ms() const { return release_ms_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    // Exposed for the UI, the same way the compressor publishes its gain
    // reduction: seeing the filter move explains the effect faster than any
    // amount of text.
    float envelope() const { return published_env_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    float sample_rate_;
    Biquad filter_;

    std::atomic<float> base_hz_{300.0f};
    std::atomic<float> range_hz_{2200.0f};
    std::atomic<float> sensitivity_{2.0f};
    std::atomic<float> resonance_{4.0f};
    std::atomic<float> attack_ms_{8.0f};
    std::atomic<float> release_ms_{160.0f};
    std::atomic<float> mix_{1.0f};

    // Audio thread only.
    float env_ = 0.0f;
    float last_cutoff_ = 0.0f;

    std::atomic<float> published_env_{0.0f};
};
