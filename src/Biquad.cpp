#include "Biquad.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kTwoPi = 6.28318530718f;

// Nyquist guard. A shelf or peak asked for above half the sample rate produces
// coefficients that are not merely wrong but unstable, and on a device that
// accepts a sample rate from the sound card that is a reachable state, not a
// hypothetical one.
float safe_hz(float hz, float sample_rate) {
    return std::clamp(hz, 10.0f, sample_rate * 0.45f);
}
}  // namespace

void Biquad::set_low_shelf(float sample_rate, float hz, float gain_db, float slope) {
    const float A = std::pow(10.0f, gain_db / 40.0f);
    const float w0 = kTwoPi * safe_hz(hz, sample_rate) / sample_rate;
    const float cs = std::cos(w0), sn = std::sin(w0);
    const float alpha = sn / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / slope - 1.0f) + 2.0f);
    const float beta = 2.0f * std::sqrt(A) * alpha;
    const float a0 = (A + 1.0f) + (A - 1.0f) * cs + beta;

    b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + beta) / a0;
    b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs) / a0;
    b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - beta) / a0;
    a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs) / a0;
    a2 = ((A + 1.0f) + (A - 1.0f) * cs - beta) / a0;
}

void Biquad::set_peaking(float sample_rate, float hz, float gain_db, float q) {
    const float A = std::pow(10.0f, gain_db / 40.0f);
    const float w0 = kTwoPi * safe_hz(hz, sample_rate) / sample_rate;
    const float cs = std::cos(w0), sn = std::sin(w0);
    const float alpha = sn / (2.0f * std::max(q, 0.05f));
    const float a0 = 1.0f + alpha / A;

    b0 = (1.0f + alpha * A) / a0;
    b1 = -2.0f * cs / a0;
    b2 = (1.0f - alpha * A) / a0;
    a1 = -2.0f * cs / a0;
    a2 = (1.0f - alpha / A) / a0;
}

void Biquad::set_high_shelf(float sample_rate, float hz, float gain_db, float slope) {
    const float A = std::pow(10.0f, gain_db / 40.0f);
    const float w0 = kTwoPi * safe_hz(hz, sample_rate) / sample_rate;
    const float cs = std::cos(w0), sn = std::sin(w0);
    const float alpha = sn / 2.0f * std::sqrt((A + 1.0f / A) * (1.0f / slope - 1.0f) + 2.0f);
    const float beta = 2.0f * std::sqrt(A) * alpha;
    const float a0 = (A + 1.0f) - (A - 1.0f) * cs + beta;

    b0 = A * ((A + 1.0f) + (A - 1.0f) * cs + beta) / a0;
    b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs) / a0;
    b2 = A * ((A + 1.0f) + (A - 1.0f) * cs - beta) / a0;
    a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs) / a0;
    a2 = ((A + 1.0f) - (A - 1.0f) * cs - beta) / a0;
}

void Biquad::set_lowpass(float sample_rate, float hz, float q) {
    const float w0 = kTwoPi * safe_hz(hz, sample_rate) / sample_rate;
    const float cs = std::cos(w0), sn = std::sin(w0);
    const float alpha = sn / (2.0f * std::max(q, 0.05f));
    const float a0 = 1.0f + alpha;

    b0 = (1.0f - cs) / 2.0f / a0;
    b1 = (1.0f - cs) / a0;
    b2 = b0;
    a1 = -2.0f * cs / a0;
    a2 = (1.0f - alpha) / a0;
}
