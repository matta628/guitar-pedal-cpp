#include "WaveFolder.h"

#include <algorithm>
#include <cmath>

void WaveFolder::set_drive(float drive) {
    drive_.store(std::clamp(drive, 1.0f, 20.0f), std::memory_order_relaxed);
}
void WaveFolder::set_symmetry(float offset) {
    symmetry_.store(std::clamp(offset, -0.5f, 0.5f), std::memory_order_relaxed);
}
void WaveFolder::set_level(float level) {
    level_.store(std::clamp(level, 0.0f, 2.0f), std::memory_order_relaxed);
}
void WaveFolder::set_mix(float mix) {
    mix_.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
}

void WaveFolder::process(float* buffer, std::size_t n_frames) {
    const float drive = drive_.load(std::memory_order_relaxed);
    const float symmetry = symmetry_.load(std::memory_order_relaxed);
    const float level = level_.load(std::memory_order_relaxed);
    const float mix = mix_.load(std::memory_order_relaxed);

    for (std::size_t i = 0; i < n_frames; ++i) {
        const float dry = buffer[i];
        float x = dry * drive + symmetry;

        // Triangle fold. Reflecting x about +/-1 repeatedly is the same shape
        // as a triangle wave of x, and computing it closed-form avoids an
        // unbounded `while (x > 1) x = 2 - x` loop -- which on a hot signal
        // would run a data-dependent number of iterations on the audio thread.
        // Bounded work per sample is not a detail here; it is the requirement.
        //
        // asin(sin(pi/2 * x)) * 2/pi is that triangle, exactly: it rises
        // linearly to 1, turns, falls to -1, and repeats.
        x = std::asin(std::sin(x * 1.57079633f)) * 0.63661977f;

        buffer[i] = dry * (1.0f - mix) + x * level * mix;
    }
}
