#include "BitCrusher.h"

#include <algorithm>
#include <cmath>

void BitCrusher::set_bits(float bits) {
    bits_.store(std::clamp(bits, 1.0f, 16.0f), std::memory_order_relaxed);
}
void BitCrusher::set_downsample(float factor) {
    downsample_.store(std::clamp(factor, 1.0f, 64.0f), std::memory_order_relaxed);
}
void BitCrusher::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void BitCrusher::process(float* buffer, std::size_t n_frames) {
    const float bits = bits_.load(std::memory_order_relaxed);
    const int hold = static_cast<int>(downsample_.load(std::memory_order_relaxed));
    const float mix = mix_.load(std::memory_order_relaxed);

    // Number of quantisation steps for the requested bit depth, computed once
    // per buffer. Fractional bit depths are allowed on purpose — the knob then
    // sweeps continuously instead of stepping through 15 audible jumps.
    const float steps = std::pow(2.0f, bits) - 1.0f;

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float dry = buffer[i];

        if (hold_counter_ <= 0) {
            held_ = dry;
            hold_counter_ = hold;
        }
        --hold_counter_;

        // Round to the grid. std::round rather than truncation so the error is
        // centred on zero: truncating would introduce a DC offset that grows
        // as the bit depth falls.
        const float quantised = std::round(held_ * steps) / steps;

        buffer[i] = dry * (1.0f - mix) + quantised * mix;
    }
}
