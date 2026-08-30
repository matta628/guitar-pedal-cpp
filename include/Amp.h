#pragma once

#include <atomic>

#include "Biquad.h"
#include "Effect.h"

// A simple guitar amplifier: the knobs on the front panel, in the order the
// signal actually meets them.
//
//   preamp drive -> tone stack -> power amp drive -> presence -> speaker
//
// The order is the whole point, and it is why this is one stage rather than a
// Distortion followed by a Tone. Real amps put the tone stack *between* the two
// gain stages, so the EQ shapes what the power amp is asked to distort. Turning
// the bass up on an amp does not just add bass — it pushes the power stage
// harder in the low end and changes how the whole thing breaks up. Wiring an
// EQ pedal after a distortion pedal cannot do that, because by then the
// distortion has already happened.
//
//   gain     — preamp drive. Where the character comes from at low volume.
//   bass/mid/treble — the tone stack, sitting between the two gain stages.
//   master   — how hard the power amp is driven. Distinct from volume: this is
//              the knob that makes an amp sound big rather than loud.
//   presence — a high shelf in the power amp's feedback path on a real amp;
//              here, a shelf after it. Adds bite without adding fizz.
//   volume   — output trim, after everything. Changes level, not tone.
//   cab      — speaker rolloff. A guitar speaker has almost nothing above
//              5 kHz, and skipping this is the single biggest reason naive
//              amp simulations sound like a wasp in a tin.
class Amp : public Effect {
public:
    void set_gain(float gain);          // 1 .. 50
    void set_bass_db(float db);         // -12 .. 12
    void set_mid_db(float db);
    void set_treble_db(float db);
    void set_master(float master);      // 1 .. 20, power amp drive
    void set_presence_db(float db);     // -6 .. 12
    void set_volume(float volume);      // 0 .. 1.5
    void set_cab(float cab);            // 0 = full range, 1 = dark speaker

    float gain() const { return gain_.load(std::memory_order_relaxed); }
    float bass_db() const { return bass_db_.load(std::memory_order_relaxed); }
    float mid_db() const { return mid_db_.load(std::memory_order_relaxed); }
    float treble_db() const { return treble_db_.load(std::memory_order_relaxed); }
    float master() const { return master_.load(std::memory_order_relaxed); }
    float presence_db() const { return presence_db_.load(std::memory_order_relaxed); }
    float volume() const { return volume_.load(std::memory_order_relaxed); }
    float cab() const { return cab_.load(std::memory_order_relaxed); }

    void set_sample_rate(float sample_rate) { sample_rate_ = sample_rate; }

    void process(float* buffer, std::size_t n_frames) override;

private:
    void recompute();

    std::atomic<float> gain_{4.0f};
    std::atomic<float> bass_db_{0.0f};
    std::atomic<float> mid_db_{0.0f};
    std::atomic<float> treble_db_{0.0f};
    std::atomic<float> master_{2.0f};
    std::atomic<float> presence_db_{0.0f};
    std::atomic<float> volume_{0.7f};
    std::atomic<float> cab_{0.7f};

    float sample_rate_ = 48000.0f;
    Biquad low_, mid_, high_, presence_, speaker_;

    float cached_bass_ = 1e9f;
    float cached_mid_ = 1e9f;
    float cached_treble_ = 1e9f;
    float cached_presence_ = 1e9f;
    float cached_cab_ = 1e9f;
    float cached_rate_ = 0.0f;
};
