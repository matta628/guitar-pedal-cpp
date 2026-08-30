#pragma once

// A single second-order IIR section, plus the RBJ cookbook coefficient
// formulas for the shapes this project needs.
//
// Shared by Tone and Amp rather than written twice: the tone stack of a guitar
// amp and a standalone EQ pedal are the same three filters with different
// centre frequencies, and the only thing worse than getting the cookbook
// algebra wrong once is getting it wrong twice, differently.
//
// Coefficients are set from a control path, never per sample — every set_*
// call here involves several transcendentals.
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

    // Direct Form I: keeps the input and output histories separate, which
    // matters here because coefficients change while the filter is running
    // (a knob moves) and DF1 is far better behaved about that than DF2.
    float process(float x) {
        const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return y;
    }

    void reset() { x1 = x2 = y1 = y2 = 0.0f; }

    void set_low_shelf(float sample_rate, float hz, float gain_db, float slope = 0.7f);
    void set_peaking(float sample_rate, float hz, float gain_db, float q);
    void set_high_shelf(float sample_rate, float hz, float gain_db, float slope = 0.7f);
    void set_lowpass(float sample_rate, float hz, float q = 0.707f);
};
