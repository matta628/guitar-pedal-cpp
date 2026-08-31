#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Amp.h"
#include "Biquad.h"
#include "BitCrusher.h"
#include "Chorus.h"
#include "ClickDetector.h"
#include "EnvFilter.h"
#include "Freeze.h"
#include "WaveFolder.h"
#include "Compressor.h"
#include "Delay.h"
#include "Distortion.h"
#include "Flanger.h"
#include "Fuzz.h"
#include "LedPattern.h"
#include "Looper.h"
#include "Pedalboard.h"
#include "Phaser.h"
#include "PitchShifter.h"
#include "Reverb.h"
#include "RingMod.h"
#include "Telemetry.h"
#include "Tone.h"
#include "Tremolo.h"

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

    looper.clear();
    std::vector<float> after_clear = {5.0f, 6.0f};
    looper.process(after_clear.data(), after_clear.size());
    check(looper.state() == Looper::State::Empty, "Looper: clear() resets back to Empty from Playing");
    check(after_clear[0] == 5.0f && after_clear[1] == 6.0f, "Looper: Empty state is a no-op passthrough again");
}

void test_looper_overdub_layers_and_decays() {
    Looper looper(48000.0f, 1.0f);

    looper.on_trigger();
    std::vector<float> recorded_input = {1.0f, 2.0f, 3.0f, 4.0f};
    looper.process(recorded_input.data(), recorded_input.size());

    looper.on_trigger();
    std::vector<float> settle(4, 0.0f);
    looper.process(settle.data(), settle.size());
    check(looper.state() == Looper::State::Playing, "Looper overdub: enters Playing after recording");

    looper.on_trigger();
    std::vector<float> overdub_input = {10.0f, 0.0f, 0.0f, 0.0f};
    looper.process(overdub_input.data(), overdub_input.size());
    check(looper.state() == Looper::State::Overdubbing,
          "Looper overdub: on_trigger from Playing enters Overdubbing");
    check(approx(overdub_input[0], 11.0f) && approx(overdub_input[1], 2.0f) &&
              approx(overdub_input[2], 3.0f) && approx(overdub_input[3], 4.0f),
          "Looper overdub: output during overdub is new input plus existing loop content");

    looper.on_trigger();
    std::vector<float> playback(4, 0.0f);
    looper.process(playback.data(), playback.size());
    check(looper.state() == Looper::State::Playing,
          "Looper overdub: on_trigger from Overdubbing returns to Playing");
    check(approx(playback[0], 10.98f) && approx(playback[1], 1.96f) && approx(playback[2], 2.94f) &&
              approx(playback[3], 3.92f),
          "Looper overdub: overdubbed layer persists in the loop, decayed to bound gain buildup");

    looper.clear();
    std::vector<float> after_clear = {7.0f, 8.0f};
    looper.process(after_clear.data(), after_clear.size());
    check(looper.state() == Looper::State::Empty, "Looper overdub: clear() resets to Empty from any state");
    check(after_clear[0] == 7.0f && after_clear[1] == 8.0f,
          "Looper overdub: Empty is a no-op passthrough after clear()");
}


void test_looper_level_scales_playback_not_storage() {
    Looper looper(48000.0f);
    std::vector<float> buf(4, 0.0f);

    // Record a loop of 1.0 ...
    looper.on_trigger();
    std::fill(buf.begin(), buf.end(), 1.0f);
    looper.process(buf.data(), buf.size());
    looper.on_trigger();  // -> Playing

    // ... then play it back at half volume against silence.
    looper.set_level(0.5f);
    std::fill(buf.begin(), buf.end(), 0.0f);
    looper.process(buf.data(), buf.size());
    check(std::fabs(buf[0] - 0.5f) < 1e-6f, "Looper: level scales playback");

    // Silencing the loop must not erase it: overdub a pass at level 0, then
    // restore level and the original material has to still be there.
    looper.set_level(0.0f);
    looper.on_trigger();  // -> Overdubbing
    std::fill(buf.begin(), buf.end(), 0.0f);
    looper.process(buf.data(), buf.size());
    check(std::fabs(buf[0]) < 1e-6f, "Looper: level 0 is silent");

    looper.on_trigger();  // -> Playing
    looper.set_level(1.0f);
    std::fill(buf.begin(), buf.end(), 0.0f);
    looper.process(buf.data(), buf.size());
    check(buf[0] > 0.9f, "Looper: a pass at level 0 did not erase the loop");
}

// peak_of is defined further down; these tests need their own.
float local_peak(const std::vector<float>& v) {
    float p = 0.0f;
    for (float x : v) p = std::max(p, std::fabs(x));
    return p;
}

void test_wave_folder_folds_rather_than_clips() {
    WaveFolder f;
    f.set_drive(4.0f);
    f.set_symmetry(0.0f);
    f.set_level(1.0f);
    f.set_mix(1.0f);

    // The defining property: a saturator is monotonic -- more in never means
    // less out. A folder is not. Somewhere past the fold, raising the input
    // has to LOWER the output, and that non-monotonicity is the whole effect.
    bool went_back_down = false;
    float previous = 0.0f;
    for (int i = 1; i <= 100; ++i) {
        float x = static_cast<float>(i) / 100.0f;
        f.process(&x, 1);
        if (i > 1 && x < previous - 1e-4f) went_back_down = true;
        previous = x;
    }
    check(went_back_down, "WaveFolder: output falls as input rises past a fold");

    // And it must stay bounded however hard it is hit -- the closed-form
    // triangle cannot run away the way an unbounded reflect loop could.
    float hot = 50.0f;
    f.process(&hot, 1);
    check(std::fabs(hot) <= 1.001f, "WaveFolder: a wildly hot input stays bounded");
}

void test_env_filter_opens_with_level() {
    // A quiet signal leaves the filter near its base cutoff; a loud one pushes
    // it up. Measure by how much of a bright input survives.
    const float sr = 48000.0f;
    auto brightness = [&](float amplitude) {
        EnvFilter f(sr);
        f.set_base_hz(200.0f);
        f.set_range_hz(4000.0f);
        f.set_sensitivity(3.0f);
        f.set_resonance(2.0f);
        f.set_attack_ms(1.0f);
        f.set_release_ms(50.0f);
        f.set_mix(1.0f);

        std::vector<float> buf(4096);
        // 3 kHz: well above the resting cutoff, so it only passes once the
        // envelope has opened the filter.
        for (std::size_t i = 0; i < buf.size(); ++i) {
            buf[i] = amplitude * std::sin(2.0f * 3.14159265f * 3000.0f *
                                          static_cast<float>(i) / sr);
        }
        f.process(buf.data(), buf.size());
        return local_peak(std::vector<float>(buf.end() - 1024, buf.end())) / amplitude;
    };

    const float quiet = brightness(0.05f);
    const float loud = brightness(0.9f);
    check(loud > quiet * 1.5f, "EnvFilter: a loud input passes more highs than a quiet one");
}

void test_freeze_holds_after_input_stops() {
    const float sr = 48000.0f;
    Freeze f(sr);
    f.set_grain_ms(50.0f);
    f.set_level(1.0f);
    f.set_decay(1.0f);

    // Fill the history with signal, then capture it.
    std::vector<float> buf(4096);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = 0.7f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(i) / sr);
    }
    f.process(buf.data(), buf.size());
    f.capture();

    // Now feed silence. Anything that comes out is the pad.
    std::fill(buf.begin(), buf.end(), 0.0f);
    f.process(buf.data(), buf.size());
    check(local_peak(buf) > 0.05f, "Freeze: audio continues after the input stops");
    check(f.frozen(), "Freeze: reports itself as frozen");

    // Releasing ends it.
    f.release();
    std::fill(buf.begin(), buf.end(), 0.0f);
    f.process(buf.data(), buf.size());
    check(local_peak(buf) < 1e-6f, "Freeze: release silences the pad");
    check(!f.frozen(), "Freeze: reports itself as released");
}

void test_click_detector_single_and_double() {
    using Clock = ClickDetector::Clock;
    using std::chrono::milliseconds;

    int singles = 0;
    int doubles = 0;
    ClickDetector detector(milliseconds(350));
    detector.set_handlers([&singles]() { ++singles; }, [&doubles]() { ++doubles; });

    const Clock::time_point t0 = Clock::time_point{} + milliseconds(10000);

    detector.on_press(t0);
    detector.poll(t0 + milliseconds(100));
    check(singles == 0 && doubles == 0,
          "ClickDetector: nothing fires while the double-click window is still open");

    detector.poll(t0 + milliseconds(400));
    check(singles == 1 && doubles == 0,
          "ClickDetector: a lone press reports a single click once the window closes");

    singles = doubles = 0;
    const Clock::time_point t1 = t0 + milliseconds(2000);
    detector.on_press(t1);
    detector.on_press(t1 + milliseconds(200));
    check(singles == 0 && doubles == 1,
          "ClickDetector: a second press inside the window reports a double click immediately");

    detector.poll(t1 + milliseconds(1000));
    check(singles == 0 && doubles == 1,
          "ClickDetector: a consumed double click leaves nothing pending to flush as a single");

    singles = doubles = 0;
    const Clock::time_point t2 = t1 + milliseconds(5000);
    detector.on_press(t2);
    detector.on_press(t2 + milliseconds(600));
    check(singles == 1 && doubles == 0,
          "ClickDetector: a second press past the window is a new gesture, not a double click");
}

void test_led_pattern_base_modes() {
    using Clock = LedPattern::Clock;
    using std::chrono::milliseconds;
    const Clock::time_point t0 = Clock::time_point{} + milliseconds(10000);

    LedPattern pattern;
    check(pattern.value(t0) == false, "LedPattern: default base is off");

    pattern.set_solid();
    check(pattern.value(t0) && pattern.value(t0 + milliseconds(5000)),
          "LedPattern: solid stays lit regardless of time");

    pattern.set_blink(milliseconds(200), t0);
    check(pattern.value(t0) && pattern.value(t0 + milliseconds(99)),
          "LedPattern: blink is lit for the first half of its period");
    check(!pattern.value(t0 + milliseconds(150)),
          "LedPattern: blink is dark for the second half of its period");
    check(pattern.value(t0 + milliseconds(1000)),
          "LedPattern: blink phase keeps cycling on the same period");

    // Re-asserting the same blink must not restart the phase, or the 20 ms
    // indicator loop would pin the LED to the top of its cycle forever.
    pattern.set_blink(milliseconds(200), t0 + milliseconds(150));
    check(!pattern.value(t0 + milliseconds(150)),
          "LedPattern: re-asserting an unchanged blink preserves the existing phase");
}

void test_led_pattern_burst_overrides_then_releases() {
    using Clock = LedPattern::Clock;
    using std::chrono::milliseconds;
    const Clock::time_point t0 = Clock::time_point{} + milliseconds(10000);

    LedPattern pattern;
    pattern.set_off();
    pattern.flash_burst(3, milliseconds(100), milliseconds(100), t0);

    check(pattern.value(t0), "LedPattern: burst lights immediately on its first flash");
    check(!pattern.value(t0 + milliseconds(150)), "LedPattern: burst goes dark between flashes");
    check(pattern.value(t0 + milliseconds(200)), "LedPattern: burst lights again on flash two");
    check(pattern.value(t0 + milliseconds(400)), "LedPattern: burst lights again on flash three");
    check(!pattern.value(t0 + milliseconds(700)),
          "LedPattern: after 3 flashes the burst ends and the off base takes over");

    pattern.set_solid();
    pattern.flash_burst(1, milliseconds(100), milliseconds(100), t0);
    check(!pattern.value(t0 + milliseconds(150)),
          "LedPattern: a burst overrides a solid base while it runs");
    check(pattern.value(t0 + milliseconds(500)),
          "LedPattern: the solid base returns once the burst finishes");
}

// ------------------------------------------------------------------ telemetry

void test_telemetry_levels_and_timing() {
    Telemetry t;
    t.configure(48000.0f, 256);

    std::vector<float> in(256, 0.5f);
    std::vector<float> out(256, 0.25f);
    // One block is not enough for the RMS average to settle, so drive it for
    // about a second of audio and check it converges on the true value.
    for (int i = 0; i < 200; ++i) {
        t.record_block(in.data(), out.data(), in.size(), std::chrono::microseconds(400));
    }

    const Telemetry::Snapshot s = t.snapshot();
    check(approx(s.input.peak, 0.5f, 1e-2f), "Telemetry: input peak tracks the signal");
    check(approx(s.input.rms, 0.5f, 1e-2f), "Telemetry: RMS of a constant converges on it");
    check(approx(s.output.peak, 0.25f, 1e-2f), "Telemetry: input and output are metered apart");
    check(s.blocks == 200, "Telemetry: every block is counted");
    check(approx(s.budget_us, 5333.3f, 1.0f), "Telemetry: budget is frames/rate, not a constant");
    check(approx(s.block_us_max, 400.0f, 1.0f), "Telemetry: worst-case block time is held");
}

void test_telemetry_peak_holds_then_decays() {
    Telemetry t;
    t.configure(48000.0f, 256);
    std::vector<float> loud(256, 0.9f);
    std::vector<float> quiet(256, 0.0f);

    t.record_block(loud.data(), loud.data(), loud.size(), std::chrono::microseconds(100));
    const float immediately = t.snapshot().input.peak;

    // One block later the transient is long gone from the signal, but the meter
    // must still show it — otherwise a plucked note lands between UI frames and
    // is never seen.
    t.record_block(quiet.data(), quiet.data(), quiet.size(), std::chrono::microseconds(100));
    const float one_block_later = t.snapshot().input.peak;

    for (int i = 0; i < 400; ++i) {  // ~2 s of silence
        t.record_block(quiet.data(), quiet.data(), quiet.size(), std::chrono::microseconds(100));
    }
    const float much_later = t.snapshot().input.peak;

    check(approx(immediately, 0.9f, 1e-3f), "Telemetry: peak attack is instant");
    check(one_block_later > 0.85f, "Telemetry: peak survives the block after the transient");
    check(much_later < 0.01f, "Telemetry: peak eventually releases to silence");
}

void test_telemetry_counts_clipping_on_the_output_only() {
    Telemetry t;
    t.configure(48000.0f, 4);
    const std::vector<float> hot = {1.0f, -1.0f, 0.5f, 2.0f};
    const std::vector<float> cold(4, 0.1f);

    t.record_block(hot.data(), cold.data(), 4, std::chrono::microseconds(10));
    check(t.snapshot().clips == 0, "Telemetry: a hot input alone is not a clip");

    t.record_block(cold.data(), hot.data(), 4, std::chrono::microseconds(10));
    check(t.snapshot().clips == 3, "Telemetry: every full-scale output sample is counted");
}

void test_telemetry_scope_is_never_torn() {
    Telemetry t;
    t.configure(48000.0f, 256);

    // The reader must only ever see a window the writer finished. A torn read
    // would mix two different constants into one buffer, so filling each window
    // with a single value makes tearing detectable rather than merely unlikely.
    std::atomic<bool> stop{false};
    std::atomic<int> torn{0};
    std::atomic<int> reads{0};

    std::thread reader([&]() {
        std::vector<float> in(Telemetry::kScopeSamples);
        std::vector<float> out(Telemetry::kScopeSamples);
        while (!stop.load(std::memory_order_relaxed)) {
            if (!t.read_scope(in.data(), out.data())) continue;
            reads.fetch_add(1, std::memory_order_relaxed);
            for (std::size_t i = 1; i < in.size(); ++i) {
                if (in[i] != in[0] || out[i] != out[0]) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }
    });

    std::vector<float> block(256);
    for (int window = 0; window < 400; ++window) {
        const float value = static_cast<float>(window);
        std::fill(block.begin(), block.end(), value);
        // Exactly one full window per iteration: 8 blocks of 256 = 2048.
        for (int b = 0; b < 8; ++b) {
            t.record_block(block.data(), block.data(), block.size(), std::chrono::microseconds(50));
        }
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    check(reads.load() > 0, "Telemetry: the scope reader saw at least one window");
    check(torn.load() == 0, "Telemetry: the seqlock never hands out a torn scope window");
}

// ---------------------------------------------------------------- helpers

std::vector<float> sine(float hz, float amplitude, std::size_t n, float sample_rate = 48000.0f) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = amplitude * std::sin(2.0f * 3.14159265358979f * hz *
                                      static_cast<float>(i) / sample_rate);
    }
    return out;
}

float peak_of(const std::vector<float>& v) {
    float peak = 0.0f;
    for (float s : v) peak = std::max(peak, std::fabs(s));
    return peak;
}

bool all_finite(const std::vector<float>& v) {
    for (float s : v) {
        if (!std::isfinite(s)) return false;
    }
    return true;
}

// Counts sign changes after `from`, which is proportional to frequency for a
// signal that crosses zero twice a cycle. Used to check pitch shifting without
// pulling in an FFT.
int zero_crossings(const std::vector<float>& v, std::size_t from) {
    int n = 0;
    for (std::size_t i = from + 1; i < v.size(); ++i) {
        if ((v[i - 1] < 0.0f) != (v[i] < 0.0f)) ++n;
    }
    return n;
}

// A stage that is mixed out, or set to its neutral value, must return the
// buffer bit-for-bit. Anything else means the dry path is not actually dry.
bool unchanged_by(Effect& effect, std::vector<float> buf) {
    const std::vector<float> original = buf;
    effect.process(buf.data(), buf.size());
    return buf == original;
}

// ---------------------------------------------------------------- Biquad

void test_biquad_defaults_to_identity() {
    Biquad b;
    bool identical = true;
    for (float x : {0.5f, -0.25f, 1.0f, 0.0f, -0.75f}) {
        if (!approx(b.process(x), x, 1e-6f)) identical = false;
    }
    check(identical, "Biquad: a default-constructed section is an identity filter");
}

void test_biquad_zero_db_shelves_are_transparent() {
    constexpr float kSampleRate = 48000.0f;
    Biquad low, high, peak;
    low.set_low_shelf(kSampleRate, 200.0f, 0.0f);
    high.set_high_shelf(kSampleRate, 3000.0f, 0.0f);
    peak.set_peaking(kSampleRate, 800.0f, 0.0f, 1.0f);

    const std::vector<float> input = sine(440.0f, 0.5f, 512);
    bool transparent = true;
    for (float x : input) {
        if (!approx(low.process(x), x, 1e-4f)) transparent = false;
        if (!approx(high.process(x), x, 1e-4f)) transparent = false;
        if (!approx(peak.process(x), x, 1e-4f)) transparent = false;
    }
    check(transparent, "Biquad: 0 dB shelf and peaking sections pass signal through unchanged");
}

void test_biquad_lowpass_attenuates_treble_more_than_bass() {
    constexpr float kSampleRate = 48000.0f;
    const auto run = [&](float hz) {
        Biquad b;
        b.set_lowpass(kSampleRate, 500.0f);
        std::vector<float> buf = sine(hz, 0.5f, 4096, kSampleRate);
        for (float& s : buf) s = b.process(s);
        // Skip the first cycle or so, which is the filter settling.
        return peak_of({buf.begin() + 2048, buf.end()});
    };
    const float bass = run(100.0f);
    const float treble = run(8000.0f);
    check(bass > 0.45f, "Biquad: a 500 Hz lowpass leaves a 100 Hz tone nearly untouched");
    check(treble < bass * 0.1f, "Biquad: the same lowpass strongly attenuates 8 kHz");
}

// ---------------------------------------------------------------- Fuzz

void test_fuzz_mix_zero_is_transparent() {
    Fuzz f;
    f.set_mix(0.0f);
    f.set_drive(50.0f);
    check(unchanged_by(f, sine(440.0f, 0.4f, 256)),
          "Fuzz: mix=0 passes signal through unchanged even at full drive");
}

void test_fuzz_saturates_rather_than_amplifying() {
    const auto peak_for = [](float amplitude, float level) {
        Fuzz f;
        f.set_drive(80.0f);
        f.set_mix(1.0f);
        f.set_level(level);
        std::vector<float> buf = sine(220.0f, amplitude, 1024);
        f.process(buf.data(), buf.size());
        return all_finite(buf) ? peak_of(buf) : std::numeric_limits<float>::quiet_NaN();
    };

    const float normal = peak_for(0.5f, 1.0f);
    const float hot = peak_for(3.0f, 1.0f);

    // The defining property of a fuzz: past the clipping point, six times the
    // input produces essentially the same output level. If these diverged, the
    // stage would be amplifying rather than saturating.
    check(std::isfinite(hot) && hot < 1.5f,
          "Fuzz: a 6x over-range input stays finite and bounded");
    check(approx(hot, normal, 0.1f),
          "Fuzz: 6x the input yields the same output level — it saturates, it does not amplify");
    // level is an output trim applied after the clipper, so it scales linearly
    // even though the clipper itself does not.
    check(approx(peak_for(3.0f, 0.5f), hot * 0.5f, 0.02f),
          "Fuzz: the level control trims the output linearly");
}

void test_fuzz_gate_silences_a_quiet_signal() {
    const auto run_with_gate = [](float gate) {
        Fuzz f;
        f.set_drive(20.0f);
        f.set_mix(1.0f);
        f.set_gate(gate);
        // Well below any sensible gate threshold: the tail of a decaying note.
        std::vector<float> buf = sine(220.0f, 0.002f, 2048);
        f.process(buf.data(), buf.size());
        return peak_of(buf);
    };
    const float open = run_with_gate(0.0f);
    const float gated = run_with_gate(1.0f);
    check(gated < open, "Fuzz: the gate squashes a signal the open setting passes");
}

// ---------------------------------------------------------------- Compressor

void test_compressor_ratio_one_is_transparent() {
    Compressor c;
    c.set_sample_rate(48000.0f);
    c.set_ratio(1.0f);
    c.set_makeup_db(0.0f);
    c.set_threshold_db(-40.0f);
    std::vector<float> buf = sine(440.0f, 0.9f, 1024);
    const std::vector<float> original = buf;
    c.process(buf.data(), buf.size());
    bool same = true;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        if (!approx(buf[i], original[i], 1e-5f)) same = false;
    }
    // ratio=1 makes the exponent 0, so the gain is pow(x, 0) == 1 everywhere.
    check(same, "Compressor: ratio=1 is unity gain regardless of threshold");
    check(approx(c.reduction_db(), 0.0f, 1e-3f),
          "Compressor: ratio=1 reports no gain reduction");
}

void test_compressor_reduces_only_above_the_threshold() {
    const auto run = [](float amplitude) {
        Compressor c;
        c.set_sample_rate(48000.0f);
        c.set_threshold_db(-20.0f);  // ~0.1 linear
        c.set_ratio(8.0f);
        c.set_attack_ms(1.0f);
        c.set_release_ms(50.0f);
        c.set_makeup_db(0.0f);
        std::vector<float> buf = sine(440.0f, amplitude, 24000);
        c.process(buf.data(), buf.size());
        return std::pair<float, float>{peak_of(buf), c.reduction_db()};
    };

    const auto [loud_peak, loud_reduction] = run(0.9f);
    const auto [quiet_peak, quiet_reduction] = run(0.01f);  // -40 dB, under the threshold

    // reduction_db is 20*log10(gain), so it is negative when the compressor is
    // working and zero when it is not.
    check(loud_reduction < -6.0f,
          "Compressor: a signal well above the threshold is measurably reduced");
    check(loud_peak < 0.9f, "Compressor: the reduced signal is quieter than it went in");
    check(approx(quiet_reduction, 0.0f, 1e-3f),
          "Compressor: a signal below the threshold is left alone");
    check(approx(quiet_peak, 0.01f, 1e-4f),
          "Compressor: the below-threshold signal keeps its original level");
}

// ---------------------------------------------------------------- BitCrusher

void test_bitcrusher_downsample_holds_each_sample() {
    BitCrusher b;
    b.set_bits(16.0f);       // fine enough that quantisation is not what we are seeing
    b.set_downsample(4.0f);
    b.set_mix(1.0f);

    std::vector<float> buf(16);
    for (std::size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<float>(i) / 16.0f;
    b.process(buf.data(), buf.size());

    bool held_in_fours = true;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        // Every group of four holds the value sampled at the group's start.
        if (!approx(buf[i], buf[(i / 4) * 4], 1e-4f)) held_in_fours = false;
    }
    check(held_in_fours, "BitCrusher: downsample=4 holds each sample for four frames");
    check(!approx(buf[0], buf[4], 1e-4f), "BitCrusher: consecutive holds differ on a ramp");
}

void test_bitcrusher_quantises_to_a_coarse_grid() {
    BitCrusher b;
    b.set_bits(2.0f);        // 2^2 - 1 = 3 steps
    b.set_downsample(1.0f);
    b.set_mix(1.0f);

    std::vector<float> buf = sine(440.0f, 1.0f, 2048);
    b.process(buf.data(), buf.size());

    // Every output must land on a multiple of 1/3.
    bool on_grid = true;
    for (float s : buf) {
        if (!approx(s * 3.0f, std::round(s * 3.0f), 1e-4f)) on_grid = false;
    }
    check(on_grid, "BitCrusher: 2-bit output lands only on the 3-step quantisation grid");
    check(all_finite(buf), "BitCrusher: quantised output stays finite");
}

void test_bitcrusher_mix_zero_is_transparent() {
    BitCrusher b;
    b.set_bits(1.0f);
    b.set_downsample(32.0f);
    b.set_mix(0.0f);
    check(unchanged_by(b, sine(440.0f, 0.6f, 256)),
          "BitCrusher: mix=0 is transparent even at 1 bit and 32x downsampling");
}

// ---------------------------------------------------------------- RingMod

void test_ringmod_mix_zero_is_transparent() {
    RingMod r;
    r.set_sample_rate(48000.0f);
    r.set_frequency_hz(500.0f);
    r.set_mix(0.0f);
    check(unchanged_by(r, sine(440.0f, 0.5f, 256)), "RingMod: mix=0 passes signal through");
}

void test_ringmod_multiplies_dc_by_its_carrier() {
    RingMod r;
    r.set_sample_rate(48000.0f);
    r.set_frequency_hz(100.0f);
    r.set_mix(1.0f);

    // DC in means the output *is* the carrier, scaled — the cleanest way to
    // see that this multiplies rather than adds.
    std::vector<float> buf(4800, 1.0f);
    r.process(buf.data(), buf.size());

    double mean = 0.0;
    for (float s : buf) mean += s;
    mean /= static_cast<double>(buf.size());

    check(approx(peak_of(buf), 1.0f, 0.02f), "RingMod: DC input yields a full-scale carrier");
    check(std::fabs(mean) < 0.02, "RingMod: the ring-modulated carrier has no DC offset");
    check(zero_crossings(buf, 0) >= 18,
          "RingMod: a 100 Hz carrier crosses zero about twice per cycle over 100 ms");
}

// ---------------------------------------------------------------- Tremolo

void test_tremolo_depth_zero_is_transparent() {
    Tremolo t;
    t.set_sample_rate(48000.0f);
    t.set_rate_hz(5.0f);
    t.set_depth(0.0f);
    check(unchanged_by(t, sine(440.0f, 0.5f, 512)), "Tremolo: depth=0 leaves the signal alone");
}

void test_tremolo_ducks_to_silence_and_never_boosts() {
    Tremolo t;
    t.set_sample_rate(48000.0f);
    t.set_rate_hz(10.0f);
    t.set_depth(1.0f);
    t.set_shape(0.0f);

    std::vector<float> buf(9600, 1.0f);  // DC, so the output is the gain curve itself
    t.process(buf.data(), buf.size());

    float lowest = 1.0f;
    for (float s : buf) lowest = std::min(lowest, s);

    check(peak_of(buf) <= 1.0f + 1e-4f,
          "Tremolo: full depth never exceeds unity gain — it ducks, it does not boost");
    check(lowest < 0.01f, "Tremolo: full depth reaches silence at the bottom of the sweep");
}

// ---------------------------------------------------------------- Tone / Amp

void test_tone_flat_is_transparent() {
    Tone tone;
    tone.set_sample_rate(48000.0f);
    tone.set_bass_db(0.0f);
    tone.set_mid_db(0.0f);
    tone.set_treble_db(0.0f);

    std::vector<float> buf = sine(440.0f, 0.5f, 2048);
    const std::vector<float> original = buf;
    tone.process(buf.data(), buf.size());

    bool flat = true;
    for (std::size_t i = 0; i < buf.size(); ++i) {
        if (!approx(buf[i], original[i], 1e-3f)) flat = false;
    }
    check(flat, "Tone: every band at 0 dB is a flat response, not merely a quiet one");
}

void test_amp_stays_bounded_at_full_gain() {
    Amp amp;
    amp.set_sample_rate(48000.0f);
    amp.set_gain(50.0f);
    amp.set_master(20.0f);
    amp.set_volume(1.0f);

    std::vector<float> buf = sine(220.0f, 1.0f, 4096);
    amp.process(buf.data(), buf.size());
    check(all_finite(buf), "Amp: a fully cranked amp stays finite");
    check(peak_of(buf) < 2.0f, "Amp: cascaded gain stages stay bounded rather than running away");
}

// ---------------------------------------------------------------- modulation

void test_phaser_and_flanger_mix_zero_is_transparent() {
    Phaser p;
    p.set_sample_rate(48000.0f);
    p.set_mix(0.0f);
    check(unchanged_by(p, sine(440.0f, 0.5f, 512)), "Phaser: mix=0 passes signal through");

    Flanger f(48000.0f);
    f.set_mix(0.0f);
    check(unchanged_by(f, sine(440.0f, 0.5f, 512)), "Flanger: mix=0 passes signal through");
}

void test_phaser_and_flanger_stay_bounded_with_feedback() {
    Phaser p;
    p.set_sample_rate(48000.0f);
    p.set_rate_hz(1.0f);
    p.set_depth(1.0f);
    p.set_feedback(0.95f);
    p.set_stages(8);
    p.set_mix(1.0f);
    std::vector<float> pbuf = sine(440.0f, 0.7f, 48000);
    p.process(pbuf.data(), pbuf.size());
    check(all_finite(pbuf) && peak_of(pbuf) < 8.0f,
          "Phaser: maximum resonance over a full second does not diverge");

    Flanger f(48000.0f);
    f.set_rate_hz(0.5f);
    f.set_depth_ms(4.0f);
    f.set_delay_ms(2.0f);
    f.set_feedback(0.95f);
    f.set_mix(1.0f);
    std::vector<float> fbuf = sine(440.0f, 0.7f, 48000);
    f.process(fbuf.data(), fbuf.size());
    check(all_finite(fbuf) && peak_of(fbuf) < 8.0f,
          "Flanger: near-maximum feedback over a full second does not diverge");
}

void test_pitch_shifter_moves_the_pitch_by_the_requested_interval() {
    constexpr float kSampleRate = 48000.0f;
    const auto ratio_for = [&](float semitones) {
        PitchShifter ps(kSampleRate);
        ps.set_semitones(semitones);
        ps.set_cents(0.0f);
        ps.set_mix(1.0f);
        std::vector<float> buf = sine(200.0f, 0.5f, 48000, kSampleRate);
        const int dry = zero_crossings(buf, 24000);
        ps.process(buf.data(), buf.size());
        // Measure over the second half only: the crossfading read pointer needs
        // a window or two before it is tracking properly.
        const int wet = zero_crossings(buf, 24000);
        return static_cast<float>(wet) / static_cast<float>(dry);
    };

    check(approx(ratio_for(12.0f), 2.0f, 0.1f),
          "PitchShifter: +12 semitones doubles the frequency");
    check(approx(ratio_for(-12.0f), 0.5f, 0.05f),
          "PitchShifter: -12 semitones halves the frequency");
    check(approx(ratio_for(0.0f), 1.0f, 0.02f),
          "PitchShifter: 0 semitones leaves the frequency alone");
}

void test_pitch_shifter_mix_zero_is_transparent() {
    PitchShifter ps(48000.0f);
    ps.set_semitones(12.0f);
    ps.set_mix(0.0f);
    check(unchanged_by(ps, sine(440.0f, 0.5f, 512)), "PitchShifter: mix=0 passes signal through");
}

// ---------------------------------------------------------------- Pedalboard

void test_pedalboard_preset_table_resolves() {
    // Construction throws if any preset names a parameter that does not exist,
    // so simply building one validates the whole table.
    Pedalboard board(48000.0f);
    check(board.preset_count() > 0, "Pedalboard: the preset table builds without an unknown id");
    check(!board.params().empty(), "Pedalboard: stages publish parameters");

    bool ids_unique = true;
    std::set<std::string> seen;
    for (const auto& p : board.params()) {
        if (!seen.insert(p.id).second) ids_unique = false;
    }
    check(ids_unique, "Pedalboard: every parameter id is unique");

    bool chains_fit = true;
    for (const auto& preset : board.presets()) {
        if (preset.chain_length > Pedalboard::kMaxChain) chains_fit = false;
        for (std::size_t i = 0; i < preset.chain_length; ++i) {
            if (preset.chain[i] == nullptr) chains_fit = false;
        }
    }
    check(chains_fit, "Pedalboard: no preset chain exceeds kMaxChain or holds a null stage");
}

// The rule the header states: because presets share one instance of each stage,
// a preset must set *every* parameter of every stage it uses. A preset that
// leaves one out inherits whatever the previously selected preset put there,
// which makes the sound depend on the order presets were visited in. That is a
// property of the table, so it is checked here rather than trusted.
void test_every_preset_fully_specifies_the_stages_it_uses() {
    Pedalboard board(48000.0f);

    std::string missing;
    for (const auto& preset : board.presets()) {
        std::set<std::string> specified;
        for (const auto& [param_index, value] : preset.settings) {
            specified.insert(board.params()[param_index].id);
        }
        for (const std::string& group : preset.groups) {
            for (const auto& param : board.params()) {
                if (param.group == group && specified.count(param.id) == 0) {
                    if (missing.empty()) missing = preset.id + " omits " + param.id;
                }
            }
        }
    }
    check(missing.empty(),
          missing.empty()
              ? "Pedalboard: every preset sets every parameter of every stage it uses"
              : missing.c_str());
}

void test_pedalboard_select_applies_settings_and_cycle_wraps() {
    Pedalboard board(48000.0f);
    const int count = board.preset_count();

    // Selecting a preset must actually push its values into the stages. Find a
    // parameter two presets disagree about and watch it change.
    bool observed_a_change = false;
    for (int i = 0; i < count && !observed_a_change; ++i) {
        for (int j = 0; j < count && !observed_a_change; ++j) {
            if (i == j) continue;
            board.select(i);
            std::vector<float> before;
            for (const auto& p : board.params()) before.push_back(p.get());
            board.select(j);
            for (std::size_t k = 0; k < board.params().size(); ++k) {
                if (!approx(board.params()[k].get(), before[k], 1e-6f)) observed_a_change = true;
            }
        }
    }
    check(observed_a_change, "Pedalboard: select() pushes the preset's values into the stages");

    board.select(count - 1);
    check(board.cycle() == 0, "Pedalboard: cycle() wraps from the last preset back to the first");
    check(board.current() == 0, "Pedalboard: cycle() publishes the index it returns");

    // Out-of-range selections are ignored rather than clamped or fatal.
    board.select(count + 99);
    check(board.current() == 0, "Pedalboard: an out-of-range select() is ignored");
    board.select(-1);
    check(board.current() == 0, "Pedalboard: a negative select() is ignored");
}

void test_pedalboard_processes_every_preset_without_blowing_up() {
    Pedalboard board(48000.0f);
    bool all_ok = true;
    std::string first_bad;
    for (int i = 0; i < board.preset_count(); ++i) {
        board.select(i);
        // A loud sustained tone through the whole chain, long enough for a
        // reverb tail and a feedback loop to build.
        std::vector<float> buf = sine(220.0f, 0.8f, 24000);
        board.process(buf.data(), buf.size());
        if (!all_finite(buf) || peak_of(buf) > 12.0f) {
            all_ok = false;
            if (first_bad.empty()) first_bad = board.presets()[static_cast<std::size_t>(i)].id;
        }
    }
    check(all_ok, all_ok ? "Pedalboard: every preset stays finite and bounded on a hot input"
                         : ("Pedalboard: preset diverged: " + first_bad).c_str());
}

void test_pedalboard_saves_and_resets_user_edits() {
    const std::string path =
        (std::filesystem::temp_directory_path() / "guitar-pedal-test-presets.conf").string();
    std::filesystem::remove(path);

    // Not preset 0: "clean" has an empty chain and controls no parameters, so
    // there would be nothing to save and the round trip would prove nothing.
    // Pick the first preset that actually owns some knobs.
    const auto first_preset_with_settings = [](const Pedalboard& b) {
        for (int i = 0; i < b.preset_count(); ++i) {
            if (!b.presets()[static_cast<std::size_t>(i)].settings.empty()) return i;
        }
        return -1;
    };

    float saved_value = 0.0f;
    std::size_t index = 0;
    int target = 0;
    {
        Pedalboard board(48000.0f);
        board.set_storage_path(path);
        target = first_preset_with_settings(board);
        check(target >= 0, "Pedalboard: at least one preset controls parameters");
        board.select(target);
        index = board.presets()[static_cast<std::size_t>(target)].settings.front().first;

        const auto& param = board.params()[index];
        // Move it somewhere it certainly was not.
        saved_value = (param.get() == param.max) ? param.min
                                                 : (param.get() + param.max) * 0.5f;
        param.set(saved_value);
        saved_value = param.get();  // whatever the setter clamped it to

        std::string error;
        check(board.save_user_preset(target, &error),
              "Pedalboard: saving a preset's edits succeeds");
        check(board.has_override(target), "Pedalboard: the saved preset is marked as modified");
    }

    check(std::filesystem::exists(path), "Pedalboard: saving writes the settings file");

    {
        // A fresh board must come back with the edit, not the table value.
        Pedalboard board(48000.0f);
        board.set_storage_path(path);
        board.load_user_presets();
        board.select(target);
        check(approx(board.params()[index].get(), saved_value, 1e-3f),
              "Pedalboard: a saved edit survives a reload");
        check(board.has_override(target),
              "Pedalboard: the reloaded preset is still marked as modified");

        std::string error;
        check(board.reset_preset(target, &error), "Pedalboard: resetting a preset succeeds");
        check(!board.has_override(target),
              "Pedalboard: a reset preset is no longer marked as modified");
    }

    {
        // And the reset must have been persisted, not just applied in memory.
        Pedalboard board(48000.0f);
        board.set_storage_path(path);
        board.load_user_presets();
        check(!board.has_override(target),
              "Pedalboard: the reset is persisted, not only in memory");
    }

    std::filesystem::remove(path);
}

void test_pedalboard_ignores_a_settings_file_it_cannot_understand() {
    const std::string path =
        (std::filesystem::temp_directory_path() / "guitar-pedal-test-junk.conf").string();
    {
        std::ofstream out(path, std::ios::trunc);
        out << "# a file written by a much older build\n"
               "[no-such-preset]\n"
               "no.such.param 0.5\n"
               "\n"
               "[clean]\n"
               "also.not.a.param 1.0\n"
               "this line has no value\n";
    }

    // Unknown ids are skipped rather than fatal: a stale settings file must not
    // stop the pedal from starting. This is the opposite of the shipped preset
    // table, where an unknown id throws.
    Pedalboard board(48000.0f);
    board.set_storage_path(path);
    board.load_user_presets();
    check(board.preset_count() > 0,
          "Pedalboard: an unreadable settings file is skipped, not fatal");

    bool any_override = false;
    for (int i = 0; i < board.preset_count(); ++i) {
        if (board.has_override(i)) any_override = true;
    }
    check(!any_override,
          "Pedalboard: a settings block whose parameters all failed to resolve is not an override");

    std::filesystem::remove(path);
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
    test_looper_overdub_layers_and_decays();
    test_wave_folder_folds_rather_than_clips();
    test_env_filter_opens_with_level();
    test_freeze_holds_after_input_stops();
    test_click_detector_single_and_double();
    test_looper_level_scales_playback_not_storage();
    test_led_pattern_base_modes();
    test_led_pattern_burst_overrides_then_releases();
    test_telemetry_levels_and_timing();
    test_telemetry_peak_holds_then_decays();
    test_telemetry_counts_clipping_on_the_output_only();
    test_telemetry_scope_is_never_torn();

    test_biquad_defaults_to_identity();
    test_biquad_zero_db_shelves_are_transparent();
    test_biquad_lowpass_attenuates_treble_more_than_bass();
    test_fuzz_mix_zero_is_transparent();
    test_fuzz_saturates_rather_than_amplifying();
    test_fuzz_gate_silences_a_quiet_signal();
    test_compressor_ratio_one_is_transparent();
    test_compressor_reduces_only_above_the_threshold();
    test_bitcrusher_downsample_holds_each_sample();
    test_bitcrusher_quantises_to_a_coarse_grid();
    test_bitcrusher_mix_zero_is_transparent();
    test_ringmod_mix_zero_is_transparent();
    test_ringmod_multiplies_dc_by_its_carrier();
    test_tremolo_depth_zero_is_transparent();
    test_tremolo_ducks_to_silence_and_never_boosts();
    test_tone_flat_is_transparent();
    test_amp_stays_bounded_at_full_gain();
    test_phaser_and_flanger_mix_zero_is_transparent();
    test_phaser_and_flanger_stay_bounded_with_feedback();
    test_pitch_shifter_moves_the_pitch_by_the_requested_interval();
    test_pitch_shifter_mix_zero_is_transparent();
    test_pedalboard_preset_table_resolves();
    test_every_preset_fully_specifies_the_stages_it_uses();
    test_pedalboard_select_applies_settings_and_cycle_wraps();
    test_pedalboard_processes_every_preset_without_blowing_up();
    test_pedalboard_saves_and_resets_user_edits();
    test_pedalboard_ignores_a_settings_file_it_cannot_understand();

    if (g_failures > 0) {
        std::printf("\n%d test(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll tests passed\n");
    return 0;
}
