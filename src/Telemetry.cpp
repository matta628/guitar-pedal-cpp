#include "Telemetry.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// Meter ballistics. Peak falls back at roughly -20 dB in 300 ms so a plucked
// note stays visible between UI frames instead of flashing for one buffer; RMS
// averages over 150 ms, which is about how the ear integrates loudness.
constexpr float kPeakReleaseSeconds = 0.30f;
constexpr float kRmsAverageSeconds = 0.15f;

// Anything at or above this on the output is counted as clipped. Not 1.0f
// exactly: a float that rounds up to full scale has already lost its peak.
constexpr float kClipThreshold = 0.999f;

}  // namespace

void Telemetry::configure(float sample_rate, unsigned int buffer_frames) {
    const float dt = (sample_rate > 0.0f) ? static_cast<float>(buffer_frames) / sample_rate : 0.005f;
    peak_decay_ = std::exp(-dt / kPeakReleaseSeconds);
    rms_alpha_ = 1.0f - std::exp(-dt / kRmsAverageSeconds);
    budget_us_.store(dt * 1e6f, std::memory_order_relaxed);
}

void Telemetry::record_block(const float* in, const float* out, std::size_t n_frames,
                             std::chrono::nanoseconds elapsed) {
    float in_peak = 0.0f;
    float in_sq = 0.0f;
    if (in != nullptr) {
        for (std::size_t i = 0; i < n_frames; ++i) {
            const float a = std::fabs(in[i]);
            in_peak = std::max(in_peak, a);
            in_sq += in[i] * in[i];
        }
    }

    float out_peak = 0.0f;
    float out_sq = 0.0f;
    std::uint32_t clipped = 0;
    for (std::size_t i = 0; i < n_frames; ++i) {
        const float a = std::fabs(out[i]);
        out_peak = std::max(out_peak, a);
        out_sq += out[i] * out[i];
        if (a >= kClipThreshold) ++clipped;
    }

    const float inv_n = (n_frames > 0) ? 1.0f / static_cast<float>(n_frames) : 0.0f;

    // Instant attack, exponential release: the hold only ever falls by decay,
    // so a transient shorter than one UI frame still registers.
    in_peak_hold_ = std::max(in_peak, in_peak_hold_ * peak_decay_);
    out_peak_hold_ = std::max(out_peak, out_peak_hold_ * peak_decay_);
    in_rms_ += rms_alpha_ * (std::sqrt(in_sq * inv_n) - in_rms_);
    out_rms_ += rms_alpha_ * (std::sqrt(out_sq * inv_n) - out_rms_);

    in_peak_.store(in_peak_hold_, std::memory_order_relaxed);
    out_peak_.store(out_peak_hold_, std::memory_order_relaxed);
    in_rms_pub_.store(in_rms_, std::memory_order_relaxed);
    out_rms_pub_.store(out_rms_, std::memory_order_relaxed);
    if (clipped != 0) clips_.fetch_add(clipped, std::memory_order_relaxed);
    blocks_.fetch_add(1, std::memory_order_relaxed);

    const float us = static_cast<float>(elapsed.count()) * 1e-3f;
    block_us_last_.store(us, std::memory_order_relaxed);
    const float avg = block_us_avg_.load(std::memory_order_relaxed);
    block_us_avg_.store(avg + 0.05f * (us - avg), std::memory_order_relaxed);
    if (us > block_us_max_.load(std::memory_order_relaxed)) {
        block_us_max_.store(us, std::memory_order_relaxed);
    }

    // Fill the staging window and publish it whole. A partial window is never
    // visible to a reader, so the scope never shows a seam.
    std::size_t consumed = 0;
    while (consumed < n_frames) {
        const std::size_t room = kScopeSamples - stage_fill_;
        const std::size_t take = std::min(room, n_frames - consumed);
        if (in != nullptr) {
            std::memcpy(&stage_in_[stage_fill_], in + consumed, take * sizeof(float));
        } else {
            std::memset(&stage_in_[stage_fill_], 0, take * sizeof(float));
        }
        std::memcpy(&stage_out_[stage_fill_], out + consumed, take * sizeof(float));
        stage_fill_ += take;
        consumed += take;

        if (stage_fill_ == kScopeSamples) {
            const std::uint32_t seq = scope_seq_.load(std::memory_order_relaxed);
            scope_seq_.store(seq + 1, std::memory_order_relaxed);  // odd: write in progress
            std::atomic_thread_fence(std::memory_order_release);
            std::memcpy(scope_in_.data(), stage_in_.data(), sizeof(scope_in_));
            std::memcpy(scope_out_.data(), stage_out_.data(), sizeof(scope_out_));
            scope_seq_.store(seq + 2, std::memory_order_release);  // even: readable again
            stage_fill_ = 0;
        }
    }
}

void Telemetry::reset_peaks() {
    block_us_max_.store(0.0f, std::memory_order_relaxed);
    clips_.store(0, std::memory_order_relaxed);
    xruns_.store(0, std::memory_order_relaxed);
}

Telemetry::Snapshot Telemetry::snapshot() const {
    Snapshot s;
    s.input.peak = in_peak_.load(std::memory_order_relaxed);
    s.input.rms = in_rms_pub_.load(std::memory_order_relaxed);
    s.output.peak = out_peak_.load(std::memory_order_relaxed);
    s.output.rms = out_rms_pub_.load(std::memory_order_relaxed);
    s.blocks = blocks_.load(std::memory_order_relaxed);
    s.xruns = xruns_.load(std::memory_order_relaxed);
    s.clips = clips_.load(std::memory_order_relaxed);
    s.block_us_last = block_us_last_.load(std::memory_order_relaxed);
    s.block_us_avg = block_us_avg_.load(std::memory_order_relaxed);
    s.block_us_max = block_us_max_.load(std::memory_order_relaxed);
    s.budget_us = budget_us_.load(std::memory_order_relaxed);
    return s;
}

bool Telemetry::read_scope(float* input, float* output) const {
    // Four attempts, then give up: at ~43 ms between publishes and a copy that
    // takes microseconds, losing four in a row means something is badly wrong,
    // and a dropped UI frame is the right way to lose that argument.
    //
    // The data reads below race with the writer by construction — that is what
    // the sequence counter exists to detect. It is the standard seqlock caveat:
    // formally a data race on the payload, made safe in practice by the fences
    // and by discarding any read the counter says was torn.
    for (int attempt = 0; attempt < 4; ++attempt) {
        const std::uint32_t before = scope_seq_.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) continue;  // writer mid-copy
        std::memcpy(input, scope_in_.data(), sizeof(scope_in_));
        std::memcpy(output, scope_out_.data(), sizeof(scope_out_));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (scope_seq_.load(std::memory_order_relaxed) == before) return true;
    }
    return false;
}
