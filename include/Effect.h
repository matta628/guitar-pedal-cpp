#pragma once

#include <cstddef>

class Effect {
public:
    virtual ~Effect() = default;
    virtual void process(float* buffer, std::size_t n_frames) = 0;
};
