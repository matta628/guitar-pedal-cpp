#pragma once

#include <atomic>

#include "Effect.h"

class Distortion : public Effect {
public:
    void set_drive(float drive);
    void set_mix(float mix);

    // Readable from any thread, same as the setters. The web UI needs the
    // current value to draw a slider in the right place at page load.
    float drive() const { return drive_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::atomic<float> drive_{4.0f};
    std::atomic<float> mix_{1.0f};
};
