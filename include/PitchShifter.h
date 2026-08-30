#pragma once

#include <atomic>
#include <vector>

#include "Effect.h"

// Time-domain pitch shifting by two crossfaded delay-line taps.
//
// The idea: read out of a delay line at a rate other than the rate it is
// written at. Reading faster raises the pitch, slower lowers it. The problem
// is that the read pointer then drifts towards (or away from) the write
// pointer forever, so it has to be wrapped back periodically — and a bare wrap
// is a discontinuity, which is a click. The fix is two taps half a window
// apart, crossfaded so that whichever one is about to wrap is already at zero
// gain when it does.
//
// This is the cheap family of algorithm, not a phase vocoder: it costs one
// interpolation and one crossfade per sample and needs no FFT, no lookahead
// and no allocation. It pays for that with a characteristic warble on
// sustained notes, because a 50 ms window that does not line up with the
// note's period splices the waveform mid-cycle. That artefact is not being
// apologised for here — it is what a DigiTech Whammy sounds like, and the
// Radiohead preset wants it.
//
// Used two ways in this project: full-wet at +12/-12 for a Whammy octave, and
// low-mix at +12 in front of the reverb for a shimmer.
class PitchShifter : public Effect {
public:
    explicit PitchShifter(float sample_rate);

    void set_semitones(float semitones);   // -24 .. +24
    // Fine detune, separate from semitones because the two are used for
    // completely different things: whole semitones are a Whammy, a few cents
    // are the Lexicon-style stereo spread Robin Guthrie built the Cocteau
    // Twins' width out of. Ten cents is not reachable on a semitone slider.
    void set_cents(float cents);           // -50 .. +50
    void set_mix(float mix);

    float semitones() const { return semitones_.load(std::memory_order_relaxed); }
    float cents() const { return cents_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::vector<float> buffer_;
    std::size_t window_ = 0;   // crossfade window, in samples
    std::size_t pos_ = 0;
    float offset_ = 0.0f;      // how far the read tap trails the write pointer

    std::atomic<float> semitones_{0.0f};
    std::atomic<float> cents_{0.0f};
    std::atomic<float> mix_{1.0f};
};
