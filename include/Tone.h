#pragma once

#include <atomic>

#include "Biquad.h"
#include "Effect.h"

// Three-band tone shaping: low shelf, sweepable mid peak, high shelf.
//
// Nearly every band preset needs this and none of the existing stages provide
// it. "Scooped mids" is what makes a Deftones rhythm tone sit under a vocal;
// a mid-forward push is most of why The Strokes sound like a transistor radio;
// and a narrow, *fixed* mid peak is how a wah pedal is used when it is parked
// rather than swept, which is exactly how Wednesday's lap steel and Jimmy
// Page's V846 are used.
//
// Three RBJ biquads in series. Coefficients are recomputed only when a knob
// actually moves — see the cached_ members — because the trig involved costs
// far more than the filtering does.
class Tone : public Effect {
public:
    void set_bass_db(float db);      // low shelf at 200 Hz
    void set_mid_db(float db);       // peaking
    void set_mid_hz(float hz);       // 200 .. 3000
    void set_mid_q(float q);         // 0.3 (broad) .. 8 (parked-wah narrow)
    void set_treble_db(float db);    // high shelf at 3 kHz

    float bass_db() const { return bass_db_.load(std::memory_order_relaxed); }
    float mid_db() const { return mid_db_.load(std::memory_order_relaxed); }
    float mid_hz() const { return mid_hz_.load(std::memory_order_relaxed); }
    float mid_q() const { return mid_q_.load(std::memory_order_relaxed); }
    float treble_db() const { return treble_db_.load(std::memory_order_relaxed); }

    void set_sample_rate(float sample_rate) { sample_rate_ = sample_rate; }

    void process(float* buffer, std::size_t n_frames) override;

private:
    void recompute();

    std::atomic<float> bass_db_{0.0f};
    std::atomic<float> mid_db_{0.0f};
    std::atomic<float> mid_hz_{800.0f};
    std::atomic<float> mid_q_{0.9f};
    std::atomic<float> treble_db_{0.0f};

    float sample_rate_ = 48000.0f;
    Biquad low_, mid_, high_;

    // What the coefficients were last built from. Recomputing is skipped when
    // these still match, which is every buffer but the one after a knob moves.
    float cached_bass_ = 1e9f;
    float cached_mid_ = 1e9f;
    float cached_mid_hz_ = 1e9f;
    float cached_mid_q_ = 1e9f;
    float cached_treble_ = 1e9f;
    float cached_rate_ = 0.0f;
};
