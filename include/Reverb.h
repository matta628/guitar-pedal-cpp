#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include "Effect.h"

// Freeverb-style feedback delay network: parallel comb filters build the
// density, series allpasses smear what comes out of them.
//
// The four modes are not cosmetic. A room, a plate, a hall and a spring differ
// in how long the early reflections take to arrive and in what the space does
// to the top end, so a mode sets both the comb lengths (the size of the space)
// and a bandpass on the wet signal (what the space is made of). A spring tank
// is a narrow, mid-forward band; a plate is bright and dense; a hall is long
// and rolls the treble off.
//
// The comb buffers are allocated once, at the largest size any mode needs, and
// each filter carries a separate *active* length. Changing mode therefore
// changes an index bound, not a vector size — no allocation on the audio
// thread, which is the whole reason mode switching is safe to expose to a
// footswitch and a browser.
class Reverb : public Effect {
public:
    enum class Mode { Room = 0, Plate, Hall, Spring };

    explicit Reverb(float sample_rate);

    void set_mode(Mode mode);
    void set_room_size(float room_size);
    void set_damping(float damping);
    void set_mix(float mix);

    Mode mode() const { return mode_.load(std::memory_order_relaxed); }
    float mode_index() const { return static_cast<float>(mode_.load(std::memory_order_relaxed)); }
    float room_size() const { return room_size_.load(std::memory_order_relaxed); }
    float damping() const { return damping_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    void process(float* buffer, std::size_t n_frames) override;

private:
    struct CombFilter {
        std::vector<float> buffer;
        std::size_t length = 0;   // active length; never exceeds buffer.size()
        std::size_t pos = 0;
        float filterstore = 0.0f;
        float feedback = 0.5f;
        float damping = 0.5f;

        float process(float input);
    };

    struct AllpassFilter {
        std::vector<float> buffer;
        std::size_t length = 0;
        std::size_t pos = 0;
        static constexpr float kFeedback = 0.5f;

        float process(float input);
    };

    void apply_mode(Mode mode);

    std::atomic<Mode> mode_{Mode::Hall};
    std::atomic<float> room_size_{0.5f};
    std::atomic<float> damping_{0.5f};
    std::atomic<float> mix_{0.3f};

    float sample_rate_;
    std::vector<CombFilter> combs_;
    std::vector<AllpassFilter> allpasses_;

    // Wet-path bandpass, set by the mode. One pole each way is enough to
    // separate a spring from a plate by ear.
    float lowpass_state_ = 0.0f;
    float highpass_state_ = 0.0f;
    float lowpass_coeff_ = 1.0f;
    float highpass_coeff_ = 0.0f;

    Mode applied_mode_ = Mode::Room;
    bool mode_applied_ = false;
};
