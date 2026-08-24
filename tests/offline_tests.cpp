#include <cmath>
#include <cstdio>
#include <vector>

#include "Chorus.h"
#include "Delay.h"
#include "Distortion.h"
#include "Looper.h"
#include "Reverb.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::printf("FAIL: %s\n", name);
        ++g_failures;
    } else {
        std::printf("PASS: %s\n", name);
    }
}

bool approx(float a, float b, float epsilon = 1e-3f) { return std::fabs(a - b) < epsilon; }

void test_distortion_bypass() {
    Distortion d;
    d.set_mix(0.0f);
    std::vector<float> buf = {0.3f, -0.5f, 0.7f};
    const std::vector<float> original = buf;
    d.process(buf.data(), buf.size());
    check(buf == original, "Distortion: mix=0 passes signal through unchanged");
}

void test_distortion_bounds_overdriven_input() {
    Distortion d;
    d.set_drive(4.0f);
    d.set_mix(1.0f);
    std::vector<float> buf = {5.0f, -5.0f};
    d.process(buf.data(), buf.size());
    check(std::fabs(buf[0]) < 1.05f && std::fabs(buf[1]) < 1.05f,
          "Distortion: heavily overdriven input stays bounded near +-1");
}

void test_delay_impulse_lands_at_expected_offset() {
    constexpr float kSampleRate = 48000.0f;
    Delay delay(kSampleRate);
    delay.set_delay_seconds(48.0f / kSampleRate);  // exactly 48 samples
    delay.set_feedback(0.0f);
    delay.set_mix(0.35f);

    std::vector<float> buf(200, 0.0f);
    buf[0] = 1.0f;
    delay.process(buf.data(), buf.size());

    check(approx(buf[0], 0.65f), "Delay: dry sample at t=0 scaled by (1-mix)");
    check(approx(buf[48], 0.35f), "Delay: impulse echo appears at t=48 scaled by mix");
}

void test_chorus_silence_stays_silent() {
    Chorus chorus(48000.0f);
    std::vector<float> buf(64, 0.0f);
    chorus.process(buf.data(), buf.size());
    bool all_zero = true;
    for (float v : buf) {
        if (v != 0.0f) all_zero = false;
    }
    check(all_zero, "Chorus: silence in produces silence out");
}

void test_chorus_stays_finite_and_bounded() {
    Chorus chorus(48000.0f);
    chorus.set_depth_ms(8.0f);
    chorus.set_rate_hz(2.0f);
    chorus.set_mix(0.7f);

    std::vector<float> buf(4800);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / 48000.0f);
    }
    chorus.process(buf.data(), buf.size());

    bool ok = true;
    for (float v : buf) {
        if (!std::isfinite(v) || std::fabs(v) > 2.0f) ok = false;
    }
    check(ok, "Chorus: sine wave through chorus stays finite and bounded");
}

void test_reverb_silence_stays_silent() {
    Reverb reverb(48000.0f);
    std::vector<float> buf(64, 0.0f);
    reverb.process(buf.data(), buf.size());
    bool all_zero = true;
    for (float v : buf) {
        if (v != 0.0f) all_zero = false;
    }
    check(all_zero, "Reverb: silence in produces silence out");
}

void test_reverb_impulse_produces_bounded_decaying_tail() {
    Reverb reverb(48000.0f);
    reverb.set_mix(0.5f);
    std::vector<float> buf(5000, 0.0f);
    buf[0] = 1.0f;
    reverb.process(buf.data(), buf.size());

    check(approx(buf[0], 0.5f),
          "Reverb: dry impulse at t=0 scaled by (1-mix) before any reflections return");

    bool finite_and_bounded = true;
    for (float v : buf) {
        if (!std::isfinite(v) || std::fabs(v) > 10.0f) finite_and_bounded = false;
    }
    check(finite_and_bounded, "Reverb: impulse response stays finite and bounded (feedback network is stable)");

    bool has_tail = false;
    for (std::size_t i = 1500; i < buf.size(); ++i) {
        if (buf[i] != 0.0f) has_tail = true;
    }
    check(has_tail, "Reverb: impulse produces a decaying tail well after t=0");
}

void test_looper_record_and_play_back() {
    Looper looper(48000.0f, 1.0f);

    std::vector<float> silence(10, 0.0f);
    looper.process(silence.data(), silence.size());
    check(looper.state() == Looper::State::Empty, "Looper: starts in Empty state (no-op passthrough)");

    looper.on_trigger();
    std::vector<float> recorded_input = {1.0f, 2.0f, 3.0f, 4.0f};
    looper.process(recorded_input.data(), recorded_input.size());
    check(looper.state() == Looper::State::Recording, "Looper: on_trigger from Empty enters Recording");

    looper.on_trigger();
    std::vector<float> playback1(4, 0.0f);
    looper.process(playback1.data(), playback1.size());
    check(looper.state() == Looper::State::Playing, "Looper: on_trigger from Recording enters Playing");
    check(playback1[0] == 1.0f && playback1[1] == 2.0f && playback1[2] == 3.0f && playback1[3] == 4.0f,
          "Looper: playback reproduces exactly what was recorded");

    std::vector<float> playback2(4, 0.0f);
    looper.process(playback2.data(), playback2.size());
    check(playback2[0] == 1.0f && playback2[3] == 4.0f,
          "Looper: loop wraps around and repeats after loop_length samples");

    std::vector<float> live_over_loop = {10.0f, 0.0f, 0.0f, 0.0f};
    looper.process(live_over_loop.data(), live_over_loop.size());
    check(approx(live_over_loop[0], 11.0f), "Looper: live input mixes additively on top of the loop");

    looper.on_trigger();
    std::vector<float> after_clear = {5.0f, 6.0f};
    looper.process(after_clear.data(), after_clear.size());
    check(looper.state() == Looper::State::Empty, "Looper: on_trigger from Playing clears back to Empty");
    check(after_clear[0] == 5.0f && after_clear[1] == 6.0f, "Looper: Empty state is a no-op passthrough again");
}

}  // namespace

int main() {
    test_distortion_bypass();
    test_distortion_bounds_overdriven_input();
    test_delay_impulse_lands_at_expected_offset();
    test_chorus_silence_stays_silent();
    test_chorus_stays_finite_and_bounded();
    test_reverb_silence_stays_silent();
    test_reverb_impulse_produces_bounded_decaying_tail();
    test_looper_record_and_play_back();

    if (g_failures > 0) {
        std::printf("\n%d test(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll tests passed\n");
    return 0;
}
