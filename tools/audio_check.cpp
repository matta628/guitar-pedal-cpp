// Bench diagnostic for the audio path, before any DSP is in the way.
//
// guitar_pedal opens the *default* input and output device, which on a Pi is
// almost never the USB interface — it is HDMI, or the headphone jack, or
// nothing at all. That failure looks identical to broken DSP from the outside,
// so this tool proves each link of the chain separately and names the device it
// is actually talking to.
//
//   ./build/audio_check list                 enumerate every device RtAudio sees
//   ./build/audio_check meter [dev]          input only  — guitar reaches the Pi
//   ./build/audio_check tone  [dev] [hz]     output only — the Pi reaches the phones
//   ./build/audio_check thru  [dev] [in]     full duplex — the whole round trip
//   ./build/audio_check fx    [dev] [name]   round trip with one effect applied
//
// [dev] is a device id from `list`, or any case-insensitive part of its name,
// so `audio_check thru mustang` works. `thru` takes a second device only when
// input and output are different devices; normally one USB interface is both.
//
// `thru` and `fx` differ on purpose. `thru` proves the path with a straight
// memcpy, so a fault can only be the wiring or the driver. `fx` then proves the
// Pi is *doing* something: reverb is unmistakable by ear and, unlike distortion,
// stays clean enough that a wiring problem still sounds like a wiring problem.

#include <RtAudio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "Chorus.h"
#include "Distortion.h"
#include "Effect.h"
#include "Reverb.h"

// RtAudio 6 replaced the throwing API with returned error codes and swapped
// device indices for stable ids. Debian trixie ships 6.x; older boards may not.
#if defined(RTAUDIO_VERSION_MAJOR) && RTAUDIO_VERSION_MAJOR >= 6
#define PEDAL_RTAUDIO6 1
#else
#define PEDAL_RTAUDIO6 0
#endif

namespace {

constexpr unsigned int kSampleRate = 48000;
constexpr unsigned int kBufferFrames = 256;

std::atomic<bool> g_stop{false};
void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

// Written by the audio callback, read by the printing loop. Peaks are stored as
// raw bits so the callback never blocks and never allocates.
struct Levels {
    std::atomic<float> in_peak{0.0f};
    std::atomic<float> out_peak{0.0f};
    std::atomic<unsigned> xruns{0};
    std::atomic<unsigned long long> frames{0};
};

void bump_peak(std::atomic<float>& slot, float value) {
    float prev = slot.load(std::memory_order_relaxed);
    while (value > prev && !slot.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
    }
}

float block_peak(const float* buf, unsigned int n) {
    float peak = 0.0f;
    for (unsigned int i = 0; i < n; ++i) {
        peak = std::max(peak, std::fabs(buf[i]));
    }
    return peak;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ---------------------------------------------------------------- device list

struct Device {
    unsigned int id;
    std::string name;
    unsigned int in_channels;
    unsigned int out_channels;
    unsigned int preferred_rate;
    bool default_in;
    bool default_out;
    std::vector<unsigned int> rates;
};

std::vector<Device> enumerate(RtAudio& audio) {
    std::vector<Device> out;
#if PEDAL_RTAUDIO6
    for (unsigned int id : audio.getDeviceIds()) {
        RtAudio::DeviceInfo info = audio.getDeviceInfo(id);
        out.push_back({id, info.name, info.inputChannels, info.outputChannels,
                       info.preferredSampleRate, info.isDefaultInput, info.isDefaultOutput,
                       info.sampleRates});
    }
#else
    unsigned int count = audio.getDeviceCount();
    for (unsigned int i = 0; i < count; ++i) {
        RtAudio::DeviceInfo info = audio.getDeviceInfo(i);
        if (!info.probed) continue;
        out.push_back({i, info.name, info.inputChannels, info.outputChannels,
                       info.preferredSampleRate, info.isDefaultInput, info.isDefaultOutput,
                       info.sampleRates});
    }
#endif
    return out;
}

// Accepts a numeric id or a case-insensitive substring of the device name.
// Returns false and explains itself rather than guessing between two matches.
bool resolve(const std::vector<Device>& devices, const std::string& spec, bool need_input,
             Device* out) {
    if (!spec.empty() && spec.find_first_not_of("0123456789") == std::string::npos) {
        unsigned int want = static_cast<unsigned int>(std::stoul(spec));
        for (const Device& d : devices) {
            if (d.id == want) { *out = d; return true; }
        }
        std::cerr << "no device with id " << want << " — run `audio_check list`\n";
        return false;
    }

    std::vector<const Device*> hits;
    std::string needle = lower(spec);
    for (const Device& d : devices) {
        if (lower(d.name).find(needle) != std::string::npos) hits.push_back(&d);
    }
    if (hits.empty()) {
        std::cerr << "no device name contains \"" << spec << "\" — run `audio_check list`\n";
        return false;
    }
    if (hits.size() > 1) {
        std::cerr << "\"" << spec << "\" matches " << hits.size() << " devices:\n";
        for (const Device* d : hits) std::cerr << "  " << d->id << "  " << d->name << "\n";
        std::cerr << "pass the id instead.\n";
        return false;
    }
    if (need_input && hits[0]->in_channels == 0) {
        std::cerr << hits[0]->name << " has no input channels.\n";
        return false;
    }
    *out = *hits[0];
    return true;
}

// Picks the interface most likely to be the guitar path when no device is named:
// a duplex USB device beats whatever the system calls "default", which on a Pi
// is usually HDMI.
bool guess_device(const std::vector<Device>& devices, bool need_input, bool need_output,
                  Device* out) {
    const Device* best = nullptr;
    for (const Device& d : devices) {
        if (need_input && d.in_channels == 0) continue;
        if (need_output && d.out_channels == 0) continue;
        bool duplex = d.in_channels > 0 && d.out_channels > 0;
        if (best == nullptr) { best = &d; continue; }
        bool best_duplex = best->in_channels > 0 && best->out_channels > 0;
        if (duplex && !best_duplex) best = &d;
    }
    if (best == nullptr) return false;
    *out = *best;
    return true;
}

void print_devices(const std::vector<Device>& devices) {
    std::cout << "id  in  out  rate   name\n"
                 "--  --  ---  -----  ----------------------------------------\n";
    for (const Device& d : devices) {
        std::cout << std::setw(2) << d.id << "  " << std::setw(2) << d.in_channels << "  "
                  << std::setw(3) << d.out_channels << "  " << std::setw(5) << d.preferred_rate
                  << "  " << d.name;
        if (d.default_in) std::cout << "  [default in]";
        if (d.default_out) std::cout << "  [default out]";
        if (d.in_channels > 0 && d.out_channels > 0) std::cout << "  [duplex]";
        std::cout << "\n";
    }
    std::cout << "\nA device needs a non-zero `in` to hear the guitar and a non-zero `out` to\n"
                 "reach the headphones. The Mustang Micro should show both.\n";
}

// ----------------------------------------------------------------- open stream

// RtAudio 6 returns an error code; RtAudio 5 throws. Both are funnelled here so
// the callers read the same either way.
bool open_stream(RtAudio& audio, RtAudio::StreamParameters* out_params,
                 RtAudio::StreamParameters* in_params, unsigned int* buffer_frames,
                 RtAudioCallback cb, void* user_data, unsigned int rate) {
#if PEDAL_RTAUDIO6
    if (audio.openStream(out_params, in_params, RTAUDIO_FLOAT32, rate, buffer_frames, cb,
                         user_data) != RTAUDIO_NO_ERROR) {
        std::cerr << "openStream failed: " << audio.getErrorText() << "\n";
        return false;
    }
    if (audio.startStream() != RTAUDIO_NO_ERROR) {
        std::cerr << "startStream failed: " << audio.getErrorText() << "\n";
        return false;
    }
    return true;
#else
    try {
        audio.openStream(out_params, in_params, RTAUDIO_FLOAT32, rate, buffer_frames, cb,
                         user_data);
        audio.startStream();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "RtAudio error: " << e.what() << "\n";
        return false;
    }
#endif
}

void close_stream(RtAudio& audio) {
    if (audio.isStreamRunning()) audio.stopStream();
    if (audio.isStreamOpen()) audio.closeStream();
}

// ------------------------------------------------------------------- callbacks

int input_callback(void* /*out*/, void* input_buffer, unsigned int n_frames, double,
                   RtAudioStreamStatus status, void* user_data) {
    auto* levels = static_cast<Levels*>(user_data);
    if (status) levels->xruns.fetch_add(1, std::memory_order_relaxed);
    bump_peak(levels->in_peak, block_peak(static_cast<const float*>(input_buffer), n_frames));
    levels->frames.fetch_add(n_frames, std::memory_order_relaxed);
    return 0;
}

struct ToneState {
    Levels levels;
    double phase = 0.0;
    double step = 0.0;   // radians per frame
    float amplitude = 0.25f;  // about -12 dBFS, loud enough to hear, quiet enough to be safe
};

int tone_callback(void* output_buffer, void* /*in*/, unsigned int n_frames, double,
                  RtAudioStreamStatus status, void* user_data) {
    auto* state = static_cast<ToneState*>(user_data);
    if (status) state->levels.xruns.fetch_add(1, std::memory_order_relaxed);
    auto* out = static_cast<float*>(output_buffer);
    for (unsigned int i = 0; i < n_frames; ++i) {
        out[i] = state->amplitude * static_cast<float>(std::sin(state->phase));
        state->phase += state->step;
        if (state->phase > 2.0 * M_PI) state->phase -= 2.0 * M_PI;
    }
    bump_peak(state->levels.out_peak, state->amplitude);
    state->levels.frames.fetch_add(n_frames, std::memory_order_relaxed);
    return 0;
}

int thru_callback(void* output_buffer, void* input_buffer, unsigned int n_frames, double,
                  RtAudioStreamStatus status, void* user_data) {
    auto* levels = static_cast<Levels*>(user_data);
    if (status) levels->xruns.fetch_add(1, std::memory_order_relaxed);
    // Straight copy, no DSP at all — that is the point. Anything audible here is
    // the wiring, the device, or the driver, never the effects.
    std::memcpy(output_buffer, input_buffer, n_frames * sizeof(float));
    float peak = block_peak(static_cast<const float*>(input_buffer), n_frames);
    bump_peak(levels->in_peak, peak);
    bump_peak(levels->out_peak, peak);
    levels->frames.fetch_add(n_frames, std::memory_order_relaxed);
    return 0;
}

// Owns one effect and the metering for the fx mode. The effect is constructed
// before the stream opens, so every allocation it does is finished by the time
// the audio callback first runs.
struct FxState {
    Levels levels;
    Effect* effect = nullptr;
};

int fx_callback(void* output_buffer, void* input_buffer, unsigned int n_frames, double,
                RtAudioStreamStatus status, void* user_data) {
    auto* state = static_cast<FxState*>(user_data);
    if (status) state->levels.xruns.fetch_add(1, std::memory_order_relaxed);

    auto* out = static_cast<float*>(output_buffer);
    std::memcpy(out, input_buffer, n_frames * sizeof(float));
    bump_peak(state->levels.in_peak,
              block_peak(static_cast<const float*>(input_buffer), n_frames));

    state->effect->process(out, n_frames);

    bump_peak(state->levels.out_peak, block_peak(out, n_frames));
    state->levels.frames.fetch_add(n_frames, std::memory_order_relaxed);
    return 0;
}

// --------------------------------------------------------------------- metering

std::string meter_bar(float peak, int width = 40) {
    // dBFS, floored at -60 so silence does not print a screenful of minus signs.
    float db = peak > 1e-6f ? 20.0f * std::log10(peak) : -60.0f;
    if (db < -60.0f) db = -60.0f;
    int filled = static_cast<int>((db + 60.0f) / 60.0f * width);
    filled = std::clamp(filled, 0, width);
    std::string bar(static_cast<size_t>(filled), '#');
    bar.append(static_cast<size_t>(width - filled), '.');
    return bar;
}

std::string db_text(float peak) {
    char buf[16];
    if (peak <= 1e-6f) {
        std::snprintf(buf, sizeof(buf), "  -inf");
    } else {
        std::snprintf(buf, sizeof(buf), "%6.1f", 20.0f * std::log10(peak));
    }
    return buf;
}

// Prints one line per refresh, in place, until Ctrl+C. Returns the loudest peak
// seen so the caller can give a verdict instead of making the user judge.
float run_meter(Levels& levels, bool show_output, unsigned int rate) {
    float loudest = 0.0f;
    bool clipped = false;
    std::cout << "\nCtrl+C to stop.\n\n";
    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        float in_peak = levels.in_peak.exchange(0.0f, std::memory_order_relaxed);
        float out_peak = levels.out_peak.exchange(0.0f, std::memory_order_relaxed);
        loudest = std::max(loudest, in_peak);
        if (in_peak >= 0.999f) clipped = true;

        std::cout << "\r" << (show_output ? "in " : "   ") << meter_bar(in_peak) << " "
                  << db_text(in_peak) << " dBFS";
        if (show_output) std::cout << "   out " << db_text(out_peak);
        unsigned xruns = levels.xruns.load(std::memory_order_relaxed);
        if (xruns > 0) std::cout << "   xruns:" << xruns;
        std::cout << "  " << std::flush;
    }

    unsigned long long frames = levels.frames.load(std::memory_order_relaxed);
    std::cout << "\n\n"
              << frames << " frames (" << std::fixed << std::setprecision(1)
              << static_cast<double>(frames) / rate << " s), "
              << levels.xruns.load(std::memory_order_relaxed) << " xruns.\n";
    if (clipped) {
        std::cout << "Signal hit 0 dBFS — turn the Mustang Micro's output down or it will\n"
                     "distort before the DSP ever sees it.\n";
    }
    return loudest;
}

// ------------------------------------------------------------------- the modes

int mode_list(RtAudio& audio) {
    std::vector<Device> devices = enumerate(audio);
    if (devices.empty()) {
        std::cerr << "RtAudio found no devices at all.\n"
                     "Check `aplay -l` and `arecord -l` — if those are empty too, the interface\n"
                     "is not enumerating and this is a USB or cable problem, not a code problem.\n";
        return 1;
    }
    print_devices(devices);
    return 0;
}

int mode_meter(RtAudio& audio, const std::string& spec) {
    std::vector<Device> devices = enumerate(audio);
    Device dev;
    if (spec.empty() ? !guess_device(devices, true, false, &dev)
                     : !resolve(devices, spec, true, &dev)) {
        if (spec.empty()) std::cerr << "no device with any input channels.\n";
        return 1;
    }

    std::cout << "input: " << dev.name << " (id " << dev.id << ")\n";
    RtAudio::StreamParameters in_params;
    in_params.deviceId = dev.id;
    in_params.nChannels = 1;
    unsigned int buffer_frames = kBufferFrames;

    Levels levels;
    if (!open_stream(audio, nullptr, &in_params, &buffer_frames, &input_callback, &levels,
                     kSampleRate)) {
        return 1;
    }
    std::cout << "Play the guitar. The bar should move.";
    float loudest = run_meter(levels, false, kSampleRate);
    close_stream(audio);

    if (loudest < 1e-4f) {
        std::cout << "\nNothing arrived. The Pi opened the device but received silence, so the\n"
                     "break is upstream: guitar cable, the Micro's own input, or its USB mode.\n";
        return 1;
    }
    std::cout << "\nGuitar reaches the Pi. Peak " << db_text(loudest) << " dBFS.\n";
    return 0;
}

int mode_tone(RtAudio& audio, const std::string& spec, double hz) {
    std::vector<Device> devices = enumerate(audio);
    Device dev;
    if (spec.empty() ? !guess_device(devices, false, true, &dev)
                     : !resolve(devices, spec, false, &dev)) {
        if (spec.empty()) std::cerr << "no device with any output channels.\n";
        return 1;
    }

    std::cout << "output: " << dev.name << " (id " << dev.id << ")\n";
    RtAudio::StreamParameters out_params;
    out_params.deviceId = dev.id;
    out_params.nChannels = 1;
    unsigned int buffer_frames = kBufferFrames;

    ToneState state;
    state.step = 2.0 * M_PI * hz / kSampleRate;
    if (!open_stream(audio, &out_params, nullptr, &buffer_frames, &tone_callback, &state,
                     kSampleRate)) {
        return 1;
    }
    std::cout << "Playing a " << hz << " Hz sine at -12 dBFS. You should hear it in the headphones.";
    run_meter(state.levels, false, kSampleRate);
    close_stream(audio);
    std::cout << "\nIf that was silent, the Pi is generating audio but it is not reaching the\n"
                 "headphones — wrong device, or the Micro is not in USB audio mode.\n";
    return 0;
}

int mode_thru(RtAudio& audio, const std::string& out_spec, const std::string& in_spec) {
    std::vector<Device> devices = enumerate(audio);
    Device out_dev;
    if (out_spec.empty() ? !guess_device(devices, true, true, &out_dev)
                         : !resolve(devices, out_spec, false, &out_dev)) {
        if (out_spec.empty()) {
            std::cerr << "no full-duplex device found. Name input and output explicitly:\n"
                         "  audio_check thru <out-device> <in-device>\n";
        }
        return 1;
    }
    Device in_dev = out_dev;
    if (!in_spec.empty() && !resolve(devices, in_spec, true, &in_dev)) return 1;

    if (in_dev.id == out_dev.id) {
        std::cout << "duplex: " << out_dev.name << " (id " << out_dev.id << ")\n";
    } else {
        std::cout << "input:  " << in_dev.name << " (id " << in_dev.id << ")\n"
                  << "output: " << out_dev.name << " (id " << out_dev.id << ")\n"
                  << "Different devices means two clocks with no shared word clock — expect\n"
                  << "periodic glitches as they drift. Fine for a proof, not for playing.\n";
    }

    RtAudio::StreamParameters out_params;
    out_params.deviceId = out_dev.id;
    out_params.nChannels = 1;
    RtAudio::StreamParameters in_params;
    in_params.deviceId = in_dev.id;
    in_params.nChannels = 1;
    unsigned int buffer_frames = kBufferFrames;

    Levels levels;
    if (!open_stream(audio, &out_params, &in_params, &buffer_frames, &thru_callback, &levels,
                     kSampleRate)) {
        return 1;
    }

    // Report what the device actually granted — openStream is allowed to hand
    // back a different buffer size than the one asked for.
    double block_ms = 1000.0 * buffer_frames / kSampleRate;
    std::cout << kSampleRate << " Hz, " << buffer_frames << "-frame buffer (" << std::fixed
              << std::setprecision(1) << block_ms << " ms per block, so at least "
              << 2 * block_ms << " ms round trip).\n"
              << "Play. You should hear yourself, unprocessed.";

    float loudest = run_meter(levels, true, kSampleRate);
    close_stream(audio);

    if (loudest < 1e-4f) {
        std::cout << "\nThe stream ran but no signal arrived, so there is nothing to pass through.\n"
                     "Run `audio_check meter` first to isolate the input side.\n";
        return 1;
    }
    std::cout << "\nRound trip works: guitar → Micro → Pi → Micro → headphones.\n"
                 "That is the whole audio path proven. Everything after this is DSP.\n";
    return 0;
}

int mode_fx(RtAudio& audio, const std::string& spec, const std::string& effect_name) {
    std::vector<Device> devices = enumerate(audio);
    Device dev;
    if (spec.empty() ? !guess_device(devices, true, true, &dev)
                     : !resolve(devices, spec, true, &dev)) {
        if (spec.empty()) std::cerr << "no full-duplex device found.\n";
        return 1;
    }

    // Mixes are pushed well past the musical defaults in main.cpp. This is a
    // go/no-go test, not a tone: the difference has to be obvious on the first
    // note even through cheap headphones.
    Reverb reverb(static_cast<float>(kSampleRate));
    Chorus chorus(static_cast<float>(kSampleRate));
    Distortion distortion;
    Effect* effect = nullptr;
    std::string chosen = effect_name.empty() ? "reverb" : lower(effect_name);
    if (chosen == "reverb") {
        reverb.set_room_size(0.85f);
        reverb.set_mix(0.6f);
        effect = &reverb;
    } else if (chosen == "chorus") {
        effect = &chorus;
    } else if (chosen == "fuzz" || chosen == "distortion") {
        distortion.set_drive(8.0f);
        effect = &distortion;
    } else {
        std::cerr << "unknown effect \"" << effect_name << "\" — try reverb, chorus or fuzz.\n";
        return 2;
    }

    std::cout << "duplex: " << dev.name << " (id " << dev.id << ")\n"
              << "effect: " << chosen << "\n";

    RtAudio::StreamParameters out_params;
    out_params.deviceId = dev.id;
    out_params.nChannels = 1;
    RtAudio::StreamParameters in_params;
    in_params.deviceId = dev.id;
    in_params.nChannels = 1;
    unsigned int buffer_frames = kBufferFrames;

    FxState state;
    state.effect = effect;
    if (!open_stream(audio, &out_params, &in_params, &buffer_frames, &fx_callback, &state,
                     kSampleRate)) {
        return 1;
    }

    std::cout << "Play a note and let it ring. It should tail off instead of stopping dead —\n"
                 "that tail is the Pi, not the guitar.";
    float loudest = run_meter(state.levels, true, kSampleRate);
    close_stream(audio);

    if (loudest < 1e-4f) {
        std::cout << "\nNo signal arrived, so there was nothing to process. Run `audio_check meter`.\n";
        return 1;
    }
    std::cout << "\nThe Pi is processing audio in real time. Hearing the effect is the proof:\n"
                 "that sound existed nowhere in the signal path until the Pi made it.\n";
    return 0;
}

int usage() {
    std::cerr << "usage: audio_check list\n"
                 "       audio_check meter [device]\n"
                 "       audio_check tone  [device] [hz]\n"
                 "       audio_check thru  [out-device] [in-device]\n"
                 "       audio_check fx    [device] [reverb|chorus|fuzz]\n"
                 "\n"
                 "[device] is an id from `list` or part of its name, e.g. `mustang`.\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);
    if (argc < 2) return usage();

    std::string mode = argv[1];
    std::string arg1 = argc > 2 ? argv[2] : "";
    std::string arg2 = argc > 3 ? argv[3] : "";

    RtAudio audio;

    if (mode == "list") return mode_list(audio);
    if (mode == "meter") return mode_meter(audio, arg1);
    if (mode == "tone") {
        double hz = arg2.empty() ? 440.0 : std::stod(arg2);
        return mode_tone(audio, arg1, hz);
    }
    if (mode == "thru") return mode_thru(audio, arg1, arg2);
    if (mode == "fx") return mode_fx(audio, arg1, arg2);
    return usage();
}
