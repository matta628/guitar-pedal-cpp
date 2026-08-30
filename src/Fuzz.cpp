#include "Fuzz.h"

#include <algorithm>
#include <cmath>

namespace {

// A one-pole DC blocker, the software equivalent of the output coupling
// capacitor every fuzz circuit has. Without it, `bias` would push the whole
// waveform off centre and eat headroom in everything downstream — the reverb
// would decay towards a DC offset rather than towards silence.
constexpr float kDcPole = 0.995f;

// Envelope follower for the gate. Fast enough to open on a pick attack,
// slow enough not to chop the note into fragments.
constexpr float kGateAttack = 0.01f;
constexpr float kGateRelease = 0.0005f;

}  // namespace

void Fuzz::set_drive(float drive) {
    drive_.store(std::clamp(drive, 1.0f, 100.0f), std::memory_order_relaxed);
}
void Fuzz::set_bias(float bias) {
    bias_.store(std::clamp(bias, -0.5f, 0.5f), std::memory_order_relaxed);
}
void Fuzz::set_gate(float gate) {
    gate_.store(std::clamp(gate, 0.0f, 1.0f), std::memory_order_relaxed);
}
void Fuzz::set_tone(float tone) {
    tone_.store(std::clamp(tone, 0.0f, 1.0f), std::memory_order_relaxed);
}
void Fuzz::set_level(float level) {
    level_.store(std::clamp(level, 0.0f, 2.0f), std::memory_order_relaxed);
}
void Fuzz::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Fuzz::process(float* buffer, std::size_t n_frames) {
    const float drive = drive_.load(std::memory_order_relaxed);
    const float bias = bias_.load(std::memory_order_relaxed);
    const float gate = gate_.load(std::memory_order_relaxed);
    const float tone = tone_.load(std::memory_order_relaxed);
    const float level = level_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);

    // Tone is one pole, swept 700 Hz (dark) to 6 kHz (bright). The coefficient
    // is computed once per buffer, not per sample — the knob cannot move
    // mid-buffer, so there is nothing to gain from recomputing 256 times.
    const float cutoff = 0.02f + tone * 0.45f;
    // A gate threshold of 1.0 would mute everything; the useful range tops out
    // well below that, so the knob is scaled into it rather than mapped 1:1.
    const float gate_threshold = gate * 0.08f;

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float dry = buffer[i];

        const float rectified = std::fabs(dry);
        envelope_ += (rectified > envelope_ ? kGateAttack : kGateRelease) * (rectified - envelope_);

        // Two stages, because one is not enough to square the wave off: tanh
        // saturates, then the result is amplified into a hard clip. At high
        // drive almost every sample lands on a rail, which is the sound.
        float x = std::tanh(drive * dry + bias);
        x = std::clamp(x * 1.6f, -1.0f, 1.0f);

        const float dc_out = x - dc_x1_ + kDcPole * dc_y1_;
        dc_x1_ = x;
        dc_y1_ = dc_out;

        lowpass_ += cutoff * (dc_out - lowpass_);

        float wet = lowpass_ * level;
        if (gate_threshold > 0.0f && envelope_ < gate_threshold) {
            // Not a hard mute: scaling by the shortfall makes it sputter and
            // crackle on the way out, which is what a starved fuzz does.
            wet *= envelope_ / gate_threshold;
        }

        buffer[i] = dry * (1.0f - mix) + wet * mix;
    }
}
