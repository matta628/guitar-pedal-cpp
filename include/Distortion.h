#pragma once

#include <atomic>

#include "Effect.h"

class Distortion : public Effect {
public:
    void set_drive(float drive);
    void set_mix(float mix);

    void process(float* buffer, std::size_t n_frames) override;

private:
    std::atomic<float> drive_{4.0f};
    std::atomic<float> mix_{1.0f};
};
