#include "Freeze.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kPi = 3.14159265358979323846f;

// Hann applied twice -- once on analysis, once on synthesis -- sums to 1.5 at
// 4x overlap. Dividing by that is what makes the output level independent of
// the overlap factor.
constexpr float kOverlapGain = 1.0f / 1.5f;
}  // namespace

Freeze::Freeze(float sample_rate)
    : sample_rate_(sample_rate),
      history_(kFftSize + kHop, 0.0f),
      window_(kFftSize, 0.0f),
      magnitude_(kFftSize / 2 + 1, 0.0f),
      phase_(kFftSize / 2 + 1, 0.0f),
      advance_(kFftSize / 2 + 1, 0.0f),
      peak_of_(kFftSize / 2 + 1, 0),
      offset_(kFftSize / 2 + 1, 0.0f),
      re_(kFftSize, 0.0f),
      im_(kFftSize, 0.0f),
      bitrev_(kFftSize, 0),
      tw_cos_(kFftSize / 2, 0.0f),
      tw_sin_(kFftSize / 2, 0.0f),
      out_(kFftSize, 0.0f) {
    for (std::size_t i = 0; i < kFftSize; ++i) {
        window_[i] = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(i) /
                                             static_cast<float>(kFftSize - 1)));
    }

    // Bit-reversal permutation and twiddle factors, both precomputed so
    // process() only ever does arithmetic on memory it already owns.
    std::size_t bits = 0;
    while ((std::size_t{1} << bits) < kFftSize) ++bits;
    for (std::size_t i = 0; i < kFftSize; ++i) {
        std::size_t r = 0;
        for (std::size_t b = 0; b < bits; ++b) {
            if (i & (std::size_t{1} << b)) r |= std::size_t{1} << (bits - 1 - b);
        }
        bitrev_[i] = r;
    }
    for (std::size_t i = 0; i < kFftSize / 2; ++i) {
        const float a = -2.0f * kPi * static_cast<float>(i) / static_cast<float>(kFftSize);
        tw_cos_[i] = std::cos(a);
        tw_sin_[i] = std::sin(a);
    }
}

void Freeze::capture() { capture_pending_.store(true, std::memory_order_relaxed); }
void Freeze::release() { release_pending_.store(true, std::memory_order_relaxed); }

void Freeze::toggle() {
    if (published_frozen_.load(std::memory_order_relaxed)) {
        release();
    } else {
        capture();
    }
}

void Freeze::set_level(float level) {
    level_.store(level < 0.0f ? 0.0f : level, std::memory_order_relaxed);
}

void Freeze::set_decay(float decay) {
    decay_.store(std::clamp(decay, 0.9f, 1.0f), std::memory_order_relaxed);
}

void Freeze::set_shimmer(float shimmer) {
    shimmer_.store(std::clamp(shimmer, 0.0f, 1.0f), std::memory_order_relaxed);
}

// Iterative radix-2 Cooley-Tukey, in place, on separate real and imaginary
// arrays. Hand-rolled rather than pulled in: it is forty lines, it allocates
// nothing, and a dependency for one transform is a dependency to build, pin and
// cross-compile for the Pi.
void Freeze::fft(std::vector<float>& re, std::vector<float>& im, bool inverse) {
    const std::size_t n = kFftSize;

    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = bitrev_[i];
        if (j > i) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    for (std::size_t len = 2; len <= n; len <<= 1) {
        const std::size_t half = len / 2;
        const std::size_t step = n / len;
        for (std::size_t i = 0; i < n; i += len) {
            for (std::size_t k = 0; k < half; ++k) {
                const std::size_t t = k * step;
                const float wr = tw_cos_[t];
                const float wi = inverse ? -tw_sin_[t] : tw_sin_[t];
                const std::size_t a = i + k;
                const std::size_t b = a + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }

    if (inverse) {
        const float inv = 1.0f / static_cast<float>(n);
        for (std::size_t i = 0; i < n; ++i) {
            re[i] *= inv;
            im[i] *= inv;
        }
    }
}

void Freeze::analyse() {
    // A magnitude spectrum alone is not enough to hold a chord in tune.
    //
    // Resynthesising each bin at its *bin-centre* frequency is the obvious
    // thing and it is wrong: a real note almost never lands on a bin centre.
    // At 48 kHz with a 4096-point transform the bins are 11.7 Hz apart, so a
    // 220 Hz string sits between bins 18 and 19 and its energy is split across
    // them. Advance both at their centre frequencies and they drift apart and
    // beat -- audibly, at the bin spacing, which is a wobble once every 4096
    // samples.
    //
    // So measure the truth instead. Analyse two frames one hop apart and look
    // at how far each bin's phase actually moved. The part of that movement
    // beyond what the bin centre predicts is the bin's frequency error, and
    // adding it back gives the real advance per hop.
    const std::size_t len = history_.size();

    // Frame A: the transform-length window ending one hop ago. Because the
    // history is exactly kFftSize + kHop long, that window begins at the
    // oldest sample in the ring.
    for (std::size_t i = 0; i < kFftSize; ++i) {
        re_[i] = history_[(history_pos_ + i) % len] * window_[i];
        im_[i] = 0.0f;
    }
    fft(re_, im_, false);
    for (std::size_t k = 0; k <= kFftSize / 2; ++k) {
        phase_[k] = std::atan2(im_[k], re_[k]);  // reused below as "previous phase"
    }

    // Frame B: the most recent transform-length window.
    for (std::size_t i = 0; i < kFftSize; ++i) {
        re_[i] = history_[(history_pos_ + kHop + i) % len] * window_[i];
        im_[i] = 0.0f;
    }
    fft(re_, im_, false);

    for (std::size_t k = 0; k <= kFftSize / 2; ++k) {
        magnitude_[k] = std::sqrt(re_[k] * re_[k] + im_[k] * im_[k]);
        const float now = std::atan2(im_[k], re_[k]);
        const float expected = 2.0f * kPi * static_cast<float>(k) *
                               static_cast<float>(kHop) / static_cast<float>(kFftSize);

        // Wrap the surplus into (-pi, pi]: phase is only known modulo a turn,
        // so the smallest consistent movement is the right interpretation.
        float deviation = (now - phase_[k]) - expected;
        deviation -= 2.0f * kPi * std::floor((deviation + kPi) / (2.0f * kPi));

        advance_[k] = expected + deviation;
        phase_[k] = now;
    }

    // Even with each bin's true frequency measured, a single string still
    // occupies several bins -- its energy is smeared across them by the window.
    // Advancing those bins independently lets them drift apart, and a partial
    // whose own bins fall out of step with each other fades in and out. That is
    // the slow wobble left after the bin-centre error is fixed.
    //
    // So bind them. Find the local maxima, give every bin the phase offset it
    // has *now* relative to whichever peak it belongs to, and from here on move
    // only the peaks: each partial then travels as one rigid object and cannot
    // beat against itself.
    const std::size_t bins = kFftSize / 2;
    std::size_t last_peak = 0;
    for (std::size_t k = 0; k <= bins; ++k) {
        const bool is_peak = (k > 0 && k < bins) && magnitude_[k] > magnitude_[k - 1] &&
                             magnitude_[k] >= magnitude_[k + 1];
        if (is_peak) last_peak = k;
        peak_of_[k] = last_peak;  // nearest peak at or before k; refined below
    }
    // Second pass, backwards: a bin closer to the next peak belongs to that one.
    std::size_t next_peak = bins;
    for (std::size_t i = 0; i <= bins; ++i) {
        const std::size_t k = bins - i;
        const bool is_peak = (k > 0 && k < bins) && magnitude_[k] > magnitude_[k - 1] &&
                             magnitude_[k] >= magnitude_[k + 1];
        if (is_peak) next_peak = k;
        const std::size_t before = peak_of_[k];
        if (next_peak >= k && (next_peak - k) < (k - before)) peak_of_[k] = next_peak;
    }
    for (std::size_t k = 0; k <= bins; ++k) {
        offset_[k] = phase_[k] - phase_[peak_of_[k]];
    }
}

void Freeze::synthesise() {
    const float shimmer = shimmer_.load(std::memory_order_relaxed);
    const std::size_t bins = kFftSize / 2;

    // Move the peaks first: only they carry a phase of their own.
    for (std::size_t k = 0; k <= bins; ++k) {
        if (peak_of_[k] != k) continue;
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        const float r = (static_cast<float>(rng_ & 0xFFFFFF) / 16777215.0f) * 2.0f - 1.0f;

        // Jitter *around* the measured advance rather than replacing it. Fully
        // random phase decorrelates successive frames, and overlap-adding
        // decorrelated frames makes their sum wander -- the very warble this is
        // built to remove. Bounded jitter keeps frames adding constructively
        // while still drifting slowly enough that the pad is not a held sample.
        phase_[k] += advance_[k] + r * shimmer * (kPi * 0.25f);
        while (phase_[k] > 2.0f * kPi) phase_[k] -= 2.0f * kPi;
        while (phase_[k] < 0.0f) phase_[k] += 2.0f * kPi;
    }

    for (std::size_t k = 0; k <= bins; ++k) {
        const float ph = (peak_of_[k] == k) ? phase_[k] : phase_[peak_of_[k]] + offset_[k];
        if (peak_of_[k] != k) phase_[k] = ph;
        const float m = magnitude_[k];
        re_[k] = m * std::cos(ph);
        im_[k] = m * std::sin(ph);
    }

    // A real signal has a conjugate-symmetric spectrum. Building the mirror
    // explicitly is what guarantees the inverse transform comes back real --
    // otherwise the imaginary residue shows up as a metallic ring.
    for (std::size_t k = kFftSize / 2 + 1; k < kFftSize; ++k) {
        re_[k] = re_[kFftSize - k];
        im_[k] = -im_[kFftSize - k];
    }

    fft(re_, im_, true);

    // Accumulate at offset 0 every time. process() shifts the buffer down by one
    // hop before calling this, which is what puts successive frames a hop apart.
    for (std::size_t i = 0; i < kFftSize; ++i) {
        out_[i] += re_[i] * window_[i] * kOverlapGain;
    }

    const float d = decay_.load(std::memory_order_relaxed);
    if (d < 1.0f) {
        for (auto& m : magnitude_) m *= d;
    }
}

void Freeze::process(float* buffer, std::size_t n_frames) {
    if (release_pending_.exchange(false, std::memory_order_relaxed)) {
        active_ = false;
        std::fill(out_.begin(), out_.end(), 0.0f);
        std::fill(magnitude_.begin(), magnitude_.end(), 0.0f);
    }
    if (capture_pending_.exchange(false, std::memory_order_relaxed)) {
        analyse();
        std::fill(out_.begin(), out_.end(), 0.0f);
        out_pos_ = 0;
        until_next_frame_ = 0;
        active_ = true;
    }

    const float level = level_.load(std::memory_order_relaxed);

    for (std::size_t i = 0; i < n_frames; ++i) {
        history_[history_pos_] = buffer[i];
        history_pos_ = (history_pos_ + 1) % history_.size();

        if (!active_) continue;

        if (until_next_frame_ == 0) {
            // Slide the consumed hop off the front and clear the tail that
            // scrolls in, then add the next frame at offset 0. A ring buffer
            // cannot do this job: the frame is exactly as long as the buffer,
            // so writing one wraps it onto itself and half the overlap lands a
            // whole frame early -- which is audible as the output alternating
            // loud and quiet every 4096 samples.
            std::copy(out_.begin() + kHop, out_.end(), out_.begin());
            std::fill(out_.end() - kHop, out_.end(), 0.0f);
            synthesise();
            out_pos_ = 0;
            until_next_frame_ = kHop;
        }
        --until_next_frame_;

        buffer[i] += out_[out_pos_] * level;
        ++out_pos_;
    }

    published_frozen_.store(active_, std::memory_order_relaxed);
}
