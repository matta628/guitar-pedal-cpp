#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

// What the audio thread knows, published for observers to read (the web dev UI
// today; the LED/LCD loop could read it too).
//
// Everything the callback writes here is a relaxed atomic store into a
// preallocated member: no locks, no allocation, no syscalls, bounded work. That
// is the whole reason this class exists as its own thing rather than the web
// server reaching into the DSP objects — an observer must never be able to make
// the audio thread miss its deadline.
//
// The waveform window is the one item too large to publish as a single atomic,
// so it uses a seqlock: the writer bumps a counter to odd, copies, bumps it to
// even, and never waits; a reader that sees an odd or changed counter retries.
// The asymmetry is deliberate. A stalled reader costs a dropped frame of UI; a
// stalled writer costs an audible glitch.
class Telemetry {
public:
    // 2048 frames is ~43 ms at 48 kHz — a few cycles of a low E (82 Hz), so the
    // scope shows a recognisable waveform rather than a fragment of one.
    static constexpr std::size_t kScopeSamples = 2048;

    struct Levels {
        float peak = 0.0f;  // linear, with meter ballistics (instant attack, ~300 ms release)
        float rms = 0.0f;   // linear, EWMA over ~150 ms
    };

    struct Snapshot {
        Levels input;
        Levels output;
        std::uint64_t blocks = 0;
        std::uint32_t xruns = 0;
        std::uint32_t clips = 0;       // output samples that hit full scale
        float block_us_last = 0.0f;
        float block_us_avg = 0.0f;     // EWMA
        float block_us_max = 0.0f;     // since the last reset
        float budget_us = 0.0f;        // n_frames / sample_rate, the deadline
    };

    // Call once before the stream starts. Sets the meter time constants and the
    // per-buffer deadline the UI compares against.
    void configure(float sample_rate, unsigned int buffer_frames);

    // What one buffer contained. Returned by record_block so a caller that
    // wants to attribute the block to something (which preset was live, say)
    // does not have to scan the buffer a second time on the audio thread.
    struct BlockStats {
        float in_peak = 0.0f;
        float out_peak = 0.0f;
        std::uint32_t clips = 0;
    };

    // ---- audio thread only ----
    // `in` may be null (output-only streams); `out` may not.
    BlockStats record_block(const float* in, const float* out, std::size_t n_frames,
                            std::chrono::nanoseconds elapsed);
    void note_xrun() { xruns_.fetch_add(1, std::memory_order_relaxed); }

    // ---- any thread ----
    void reset_peaks();
    Snapshot snapshot() const;

    // Copies the most recent complete waveform window into `input` and `output`
    // (kScopeSamples floats each). Returns false if the writer won every retry,
    // which means the caller should just skip this frame.
    bool read_scope(float* input, float* output) const;

private:
    // Written and read only by the audio thread — never shared, so plain.
    float peak_decay_ = 0.98f;  // per-block multiplier for the peak hold
    float rms_alpha_ = 0.05f;   // per-block EWMA weight for the new value
    float in_peak_hold_ = 0.0f;
    float out_peak_hold_ = 0.0f;
    float in_rms_ = 0.0f;
    float out_rms_ = 0.0f;
    std::size_t stage_fill_ = 0;
    std::array<float, kScopeSamples> stage_in_{};
    std::array<float, kScopeSamples> stage_out_{};

    std::atomic<float> in_peak_{0.0f};
    std::atomic<float> in_rms_pub_{0.0f};
    std::atomic<float> out_peak_{0.0f};
    std::atomic<float> out_rms_pub_{0.0f};
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint32_t> xruns_{0};
    std::atomic<std::uint32_t> clips_{0};
    std::atomic<float> block_us_last_{0.0f};
    std::atomic<float> block_us_avg_{0.0f};
    std::atomic<float> block_us_max_{0.0f};
    std::atomic<float> budget_us_{0.0f};

    // Seqlock-protected waveform window.
    std::atomic<std::uint32_t> scope_seq_{0};
    std::array<float, kScopeSamples> scope_in_{};
    std::array<float, kScopeSamples> scope_out_{};
};
