#pragma once

#include <atomic>

#include "Effect.h"

// Feed-forward peak compressor with a hard knee.
//
// Two presets need it for different reasons. Elliott Smith's records are built
// on double-tracked parts sitting still in the mix, which needs the dynamic
// range squashed before anything else touches it; The Strokes' rhythm tone is
// audibly limited before it hits the distortion, which is part of why it stays
// so even. Putting it first in the chain matters — compressing *after*
// distortion does almost nothing, because the distortion has already flattened
// the dynamics.
class Compressor : public Effect {
public:
    void set_threshold_db(float db);   // -60 .. 0
    void set_ratio(float ratio);       // 1 (off) .. 20 (limiting)
    void set_attack_ms(float ms);
    void set_release_ms(float ms);
    void set_makeup_db(float db);

    float threshold_db() const { return threshold_db_.load(std::memory_order_relaxed); }
    float ratio() const { return ratio_.load(std::memory_order_relaxed); }
    float attack_ms() const { return attack_ms_.load(std::memory_order_relaxed); }
    float release_ms() const { return release_ms_.load(std::memory_order_relaxed); }
    float makeup_db() const { return makeup_db_.load(std::memory_order_relaxed); }

    void set_sample_rate(float sample_rate) { sample_rate_ = sample_rate; }

    // Gain currently being applied, in dB (negative = compressing). Published
    // for the UI's gain-reduction meter; audio thread writes, UI reads.
    float reduction_db() const { return reduction_db_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::atomic<float> threshold_db_{-18.0f};
    std::atomic<float> ratio_{4.0f};
    std::atomic<float> attack_ms_{5.0f};
    std::atomic<float> release_ms_{120.0f};
    std::atomic<float> makeup_db_{0.0f};
    std::atomic<float> reduction_db_{0.0f};

    float sample_rate_ = 48000.0f;
    float envelope_ = 0.0f;
};
