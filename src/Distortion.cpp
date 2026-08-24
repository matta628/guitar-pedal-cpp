#include "Distortion.h"

#include <algorithm>
#include <cmath>

void Distortion::set_drive(float drive) {
    drive_.store(std::max(drive, 1.0f), std::memory_order_relaxed);
}

void Distortion::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void Distortion::process(float* buffer, std::size_t n_frames) {
    const float drive = drive_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);
    const float normalize = 1.0f / std::tanh(drive);

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float dry = buffer[i];
        const float wet = std::tanh(drive * dry) * normalize;
        buffer[i] = mix * wet + (1.0f - mix) * dry;
    }
}
