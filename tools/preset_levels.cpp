// Measures every preset's output level against one fixed input, so preset
// balance can be judged from numbers rather than from memory of an evening's
// playing.
//
// Why offline rather than from the live PresetStats report: that report only
// covers presets actually played, at whatever the guitar happened to be doing
// at the time, so two presets are never compared on equal terms. Here every
// preset sees the identical signal, which is the only way "this one is 9 dB
// hotter than that one" means anything.
//
// The test signal is a decaying pluck, not a sine: sustained tones flatter
// compressors and misrepresent transient-sensitive stages like the envelope
// filter and the wave folder, whose whole behaviour depends on attack shape.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Pedalboard.h"

namespace {

constexpr float kSampleRate = 48000.0f;
constexpr std::size_t kBlock = 256;

float db(float linear) {
    return linear <= 1e-9f ? -120.0f : 20.0f * std::log10(linear);
}

// Six seconds of repeated plucks: fundamental plus a few harmonics under an
// exponential decay, re-struck four times so time-based effects (delay,
// reverb, freeze) reach a realistic steady state rather than being measured
// mid-onset.
std::vector<float> pluck_signal() {
    const std::size_t n = static_cast<std::size_t>(kSampleRate * 6.0f);
    std::vector<float> sig(n, 0.0f);
    const float f0 = 196.0f;  // open G
    for (std::size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kSampleRate;
        const float since = std::fmod(t, 1.5f);
        const float env = std::exp(-since * 3.2f);
        float s = 0.0f;
        s += 1.00f * std::sin(2.0f * 3.14159265f * f0 * t);
        s += 0.45f * std::sin(2.0f * 3.14159265f * f0 * 2.0f * t);
        s += 0.22f * std::sin(2.0f * 3.14159265f * f0 * 3.0f * t);
        s += 0.11f * std::sin(2.0f * 3.14159265f * f0 * 4.0f * t);
        sig[i] = 0.25f * env * s;   // ~-12 dBFS peak, a healthy DI level
    }
    return sig;
}

}  // namespace

int main(int argc, char** argv) {
    const bool csv = (argc > 1 && std::string(argv[1]) == "--csv");

    Pedalboard board(kSampleRate);
    const std::vector<float> input = pluck_signal();

    float in_peak = 0.0f;
    float in_sq = 0.0f;
    for (float x : input) {
        in_peak = std::max(in_peak, std::fabs(x));
        in_sq += x * x;
    }
    const float in_rms = std::sqrt(in_sq / static_cast<float>(input.size()));

    if (csv) {
        std::printf("preset,id,peak_dbfs,rms_dbfs,vs_input_db,clips\n");
    } else {
        std::printf("input: peak %.1f dBFS, rms %.1f dBFS\n\n", db(in_peak), db(in_rms));
        std::printf("%-28s %9s %9s %9s %7s\n", "preset", "peak", "rms", "vs in", "clips");
        std::printf("%-28s %9s %9s %9s %7s\n", "----------------------------", "--------",
                    "--------", "--------", "------");
    }

    for (int i = 0; i < board.preset_count(); ++i) {
        board.select(i);

        std::vector<float> buf = input;
        // Process in real buffer-sized blocks, exactly as the callback does:
        // block size is not incidental for stages that hold state per block.
        for (std::size_t off = 0; off < buf.size(); off += kBlock) {
            const std::size_t n = std::min(kBlock, buf.size() - off);
            board.process(buf.data() + off, n);
        }

        // Skip the first half second: several stages need to settle, and their
        // onset is not what a player hears as "how loud is this preset".
        const std::size_t skip = static_cast<std::size_t>(kSampleRate * 0.5f);
        float peak = 0.0f;
        float sq = 0.0f;
        int clips = 0;
        for (std::size_t k = skip; k < buf.size(); ++k) {
            const float a = std::fabs(buf[k]);
            peak = std::max(peak, a);
            sq += buf[k] * buf[k];
            if (a >= 0.999f) ++clips;
        }
        const float rms = std::sqrt(sq / static_cast<float>(buf.size() - skip));
        const auto& p = board.presets()[static_cast<std::size_t>(i)];

        if (csv) {
            std::printf("\"%s\",%s,%.1f,%.1f,%.1f,%d\n", p.name.c_str(), p.id.c_str(), db(peak),
                        db(rms), db(rms) - db(in_rms), clips);
        } else {
            std::printf("%-28s %8.1f  %8.1f  %+8.1f %7d%s\n", p.name.c_str(), db(peak), db(rms),
                        db(rms) - db(in_rms), clips, clips > 0 ? "  <-- CLIPS" : "");
        }
    }
    return 0;
}
