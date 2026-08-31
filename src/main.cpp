#include <RtAudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "AudioDevice.h"
#include "Looper.h"
#include "Pedalboard.h"
#include "Freeze.h"
#include "PresetStats.h"
#include "Setlist.h"
#include "Telemetry.h"
#include "WebServer.h"

#ifdef PEDAL_HAVE_GPIO
#include "ClickDetector.h"
#include "GpioButton.h"
#include "GpioLed.h"
#include "Lcd1602.h"
#include "LedPattern.h"
#endif

namespace {

std::atomic<bool> g_stop{false};
Telemetry g_telemetry;

void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

// Preset names now come from the pedalboard's table (src/Presets.cpp), not
// from an enum here. The LCD's second line has ten columns to spare after its
// "FX:   " label, which is what short_name is sized for.
const char* looper_state_name(Looper::State state) {
    switch (state) {
        case Looper::State::Empty:       return "EMPTY";
        case Looper::State::Recording:   return "REC";
        case Looper::State::Playing:     return "PLAY";
        case Looper::State::Overdubbing: return "OVERDUB";
    }
    return "?";
}

// What the single footswitch drives. Only meaningful with --switches 1: with
// two switches, switch 1 is always the looper and this stays on Looper.
//
// Set from the web UI rather than by a gesture on the switch itself. That is
// the point of doing it this way -- the switch keeps firing on the press edge,
// so the looper's boundary still lands exactly where the foot does, which a
// double-tap or long-press gesture would have cost.
enum class ControlMode { Looper, Freeze };

struct PedalChain {
    Pedalboard board;
    // Outside the board on purpose, like the looper below it. Freeze is a
    // performance control, not a tone stage: it has to work on whichever
    // preset is selected, and as a stage it would silently do nothing on the
    // 36 presets whose chain does not list it.
    Freeze freeze;
    Looper looper;

    std::atomic<ControlMode> mode{ControlMode::Looper};

    // Which presets the second footswitch walks, and where in them we are.
    // Never touched by the audio thread -- see Setlist.h.
    Setlist setlist;

    // Per-preset running totals, written from the audio callback. Sized before
    // the stream starts and never resized.
    PresetStats stats;

    // Bumped on every clear so the indicator loop can flash an acknowledgement
    // without the click handler touching a LedPattern from another thread.
    std::atomic<unsigned> clear_count{0};

    // The chain is mono: one guitar, one signal path, no stereo image to
    // preserve. The interface is not mono, so the result has to be fanned out
    // to its channels. That needs somewhere to put the mono signal while it is
    // being processed, and the audio callback may not allocate — hence a
    // scratch buffer sized once, before the stream starts.
    std::vector<float> mono;
    unsigned int out_channels = 1;

    explicit PedalChain(float sample_rate)
        : board(sample_rate), freeze(sample_rate), looper(sample_rate) {}

    // Call before the stream is started, never while it is running.
    void prepare(unsigned int max_frames, unsigned int channels) {
        mono.assign(max_frames, 0.0f);
        out_channels = channels;
    }
};

// The whole per-buffer job, factored out so the offline signal generator can
// drive the identical path the sound card drives. Real-time-safe: no
// allocation, no locks, no syscalls (steady_clock::now() is a vDSO read).
void run_block(PedalChain& chain, const float* in, float* out, unsigned int n_frames) {
    const auto t0 = std::chrono::steady_clock::now();
    const unsigned int channels = chain.out_channels;

    // RtAudio should never ask for more frames than the stream was opened
    // with, but growing the scratch buffer here would be an allocation on the
    // audio thread. Silence is the safe way to be wrong.
    if (n_frames > chain.mono.size()) {
        std::memset(out, 0, static_cast<std::size_t>(n_frames) * channels * sizeof(float));
        g_telemetry.note_xrun();
        return;
    }

    float* mono = chain.mono.data();
    if (in != nullptr) {
        std::memcpy(mono, in, n_frames * sizeof(float));
    } else {
        std::memset(mono, 0, n_frames * sizeof(float));
    }

    chain.board.process(mono, n_frames);

    // Freeze before the looper so a captured drone is part of what the looper
    // records -- hold a chord, loop over it, and the loop contains the pad.
    chain.freeze.process(mono, n_frames);

    // After the pedalboard, not inside it: the looper records what you would
    // hear, and keeps running even on the clean preset.
    chain.looper.process(mono, n_frames);

    // Fan the finished mono signal out to every output channel, interleaved.
    // Writing only channel 0 -- which is what asking RtAudio for a one-channel
    // output stream does -- puts the guitar in the left ear and silence in the
    // right one.
    if (channels == 1) {
        std::memcpy(out, mono, n_frames * sizeof(float));
    } else {
        for (unsigned int i = 0; i < n_frames; ++i) {
            const float sample = mono[i];
            for (unsigned int c = 0; c < channels; ++c) {
                out[static_cast<std::size_t>(i) * channels + c] = sample;
            }
        }
    }

    // The mono signal is what the meters and the scope should show: it is the
    // thing the chain actually produced, and the duplicate adds no information.
    const Telemetry::BlockStats block =
        g_telemetry.record_block(in, mono, n_frames, std::chrono::steady_clock::now() - t0);

    // Attribute the block to whichever preset produced it. record_block already
    // scanned the buffer for these figures, so this costs a few atomics rather
    // than a second pass.
    chain.stats.record(chain.board.current(), block.in_peak, block.out_peak, block.clips);
}

int audio_callback(void* output_buffer, void* input_buffer, unsigned int n_frames,
                   double /*stream_time*/, RtAudioStreamStatus status, void* user_data) {
    if (status) g_telemetry.note_xrun();
    run_block(*static_cast<PedalChain*>(user_data), static_cast<const float*>(input_buffer),
              static_cast<float*>(output_buffer), n_frames);
    return 0;
}

// RtAudio 6 returns an error code where RtAudio 5 throws. Funnelled here so the
// caller reads the same either way — and so a failed open is actually noticed:
// a try/catch alone silently "succeeds" against RtAudio 6.
// Opening and starting are deliberately separate calls. openStream is allowed
// to grant a different buffer size than the one asked for, and the scratch
// buffer has to be sized against what was actually granted -- which has to
// happen before the first callback can fire, not after.
bool open_stream(RtAudio& audio, RtAudio::StreamParameters* out_params,
                 RtAudio::StreamParameters* in_params, unsigned int* buffer_frames,
                 unsigned int rate, void* user_data, std::string* error) {
#if PEDAL_RTAUDIO6
    if (audio.openStream(out_params, in_params, RTAUDIO_FLOAT32, rate, buffer_frames,
                         &audio_callback, user_data) != RTAUDIO_NO_ERROR) {
        *error = audio.getErrorText();
        return false;
    }
    return true;
#else
    try {
        audio.openStream(out_params, in_params, RTAUDIO_FLOAT32, rate, buffer_frames,
                         &audio_callback, user_data);
        return true;
    } catch (const std::exception& e) {
        *error = e.what();
        return false;
    }
#endif
}

bool start_stream(RtAudio& audio, std::string* error) {
#if PEDAL_RTAUDIO6
    if (audio.startStream() != RTAUDIO_NO_ERROR) {
        *error = audio.getErrorText();
        return false;
    }
    return true;
#else
    try {
        audio.startStream();
        return true;
    } catch (const std::exception& e) {
        *error = e.what();
        return false;
    }
#endif
}

// ---------------------------------------------------------------- signal source

// A stand-in guitar, for working on the UI or the looper with no interface
// plugged in. Not a test oscillator: a decaying harmonic stack retriggered on a
// pentatonic walk, because a steady sine makes every meter, scope and loop look
// correct whether or not it is.
class SignalGenerator {
public:
    explicit SignalGenerator(float sample_rate) : sample_rate_(sample_rate) {}

    void fill(float* out, unsigned int n_frames) {
        static constexpr float kNotes[] = {82.41f, 110.0f, 146.83f, 196.0f, 246.94f, 329.63f};
        for (unsigned int i = 0; i < n_frames; ++i) {
            if (age_ >= sample_rate_ * 0.9f) {  // new pluck every 900 ms
                age_ = 0.0f;
                phase_ = 0.0f;
                note_ = (note_ + 3) % 6;
            }
            const float f = kNotes[note_];
            const float env = std::exp(-age_ / (sample_rate_ * 0.45f));
            // Fundamental plus two harmonics, the third rolling off fastest —
            // roughly how a plucked string actually decays.
            const float s = std::sin(phase_) + 0.45f * std::sin(2.0f * phase_) * env +
                            0.22f * std::sin(3.0f * phase_) * env * env;
            out[i] = 0.35f * env * s;
            phase_ += 2.0f * 3.14159265f * f / sample_rate_;
            if (phase_ > 6.2831853f) phase_ -= 6.2831853f;
            age_ += 1.0f;
        }
    }

private:
    float sample_rate_;
    float phase_ = 0.0f;
    float age_ = 1e9f;
    int note_ = 0;
};

// --------------------------------------------------------------- command line

struct Options {
    std::string device;       // input device: numeric id or name substring
    std::string out_device;   // defaults to the input device
    unsigned int rate = 0;    // 0 = whatever the device prefers
    unsigned int frames = 256;
    std::uint16_t port = 8080;
    std::string settings;     // empty = the default path under $HOME
    bool web = true;
    bool simulate = false;
    bool list = false;
    // How many footswitches are physically wired. The program cannot detect
    // this: a GPIO line requests successfully whether or not a switch is on
    // the end of it, so it has to be told.
    int switches = 1;
};

void print_usage() {
    std::cout <<
        "usage: guitar_pedal [options]\n"
        "  --device <id|name>      capture device (default: first duplex device found)\n"
        "  --out-device <id|name>  playback device (default: same as --device)\n"
        "  --rate <hz>             sample rate (default: whatever the device prefers)\n"
        "  --frames <n>            buffer size in frames (default 256)\n"
        "  --port <n>              web UI port (default 8080)\n"
        "  --switches <1|2>        footswitches wired (default 1). 1: the switch drives\n"
        "                          the looper and the web UI does everything else.\n"
        "                          2: switch 2 taps to clear, double-taps to step the setlist.\n"
        "  --settings <path>       saved preset edits (default ~/.config/guitar-pedal-cpp/presets.conf)\n"
        "  --no-web                don't serve the web UI\n"
        "  --simulate              start with the built-in signal generator on\n"
        "  --list                  list audio devices and exit\n";
}

bool parse_args(int argc, char** argv, Options* opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_next = (i + 1 < argc);
        if (a == "--device" && has_next) opt->device = argv[++i];
        else if (a == "--out-device" && has_next) opt->out_device = argv[++i];
        else if (a == "--rate" && has_next) opt->rate = std::stoul(argv[++i]);
        else if (a == "--frames" && has_next) opt->frames = std::stoul(argv[++i]);
        else if (a == "--port" && has_next) opt->port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
        else if (a == "--switches" && has_next) opt->switches = std::stoi(argv[++i]);
        else if (a == "--settings" && has_next) opt->settings = argv[++i];
        else if (a == "--no-web") opt->web = false;
        else if (a == "--simulate") opt->simulate = true;
        else if (a == "--list") opt->list = true;
        else if (a == "-h" || a == "--help") { print_usage(); return false; }
        else {
            std::cerr << "unknown option: " << a << "\n\n";
            print_usage();
            return false;
        }
    }
    return true;
}

// Where saved preset edits live. Under $HOME rather than beside the binary so
// a rebuild, or running from a different directory, doesn't lose them.
std::string default_settings_path() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return "guitar-pedal-presets.conf";
    return std::string(home) + "/.config/guitar-pedal-cpp/presets.conf";
}

// Sits next to the settings file rather than in the working directory: the
// pedal is usually started from somewhere arbitrary, or by a service.
std::string stats_path(const std::string& settings) {
    const std::string base = settings.empty() ? default_settings_path() : settings;
    const std::size_t slash = base.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".") : base.substr(0, slash);
    return dir + "/preset-levels.csv";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, &opt)) return 1;

    std::signal(SIGINT, on_sigint);

    RtAudio audio;
    const std::vector<audiodev::Device> devices = audiodev::enumerate(audio);

    if (opt.list) {
        for (const auto& d : devices) {
            std::cout << "  " << d.id << "  " << d.name << "  (in " << d.in_channels << ", out "
                      << d.out_channels << ")\n";
        }
        return 0;
    }

    // Resolve devices before anything else, because the sample rate the whole
    // engine is built around comes from them.
    std::string audio_status;
    bool have_devices = false;
    audiodev::Device in_dev;
    audiodev::Device out_dev;

    if (devices.empty()) {
        audio_status = "no audio devices found";
    } else if (!opt.device.empty() &&
               !audiodev::resolve(devices, opt.device, true, &in_dev, &audio_status)) {
        // audio_status already explains it.
    } else if (opt.device.empty() && !audiodev::guess_device(devices, true, false, &in_dev)) {
        audio_status = "no capture device — plug the interface in and restart";
    } else {
        out_dev = in_dev;
        if (!opt.out_device.empty() &&
            !audiodev::resolve(devices, opt.out_device, false, &out_dev, &audio_status)) {
            // audio_status already explains it.
        } else {
            have_devices = true;
        }
    }

    unsigned int sample_rate = opt.rate;
    if (have_devices && sample_rate == 0 &&
        !audiodev::pick_shared_rate(in_dev, out_dev, &sample_rate, &audio_status)) {
        have_devices = false;
    }
    if (sample_rate == 0) sample_rate = audiodev::kPreferredRate;

    unsigned int buffer_frames = opt.frames;

    PedalChain chain(static_cast<float>(sample_rate));
    // Sized once, here, because record() runs on the audio thread and must
    // never allocate.
    chain.stats.configure(chain.board.preset_count());
    g_telemetry.configure(static_cast<float>(sample_rate), buffer_frames);

    // Saved edits are read before the stream opens, so the first buffer already
    // sounds the way it did when the pedal was last switched off.
    {
        const std::string settings_path =
            opt.settings.empty() ? default_settings_path() : opt.settings;
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(settings_path).parent_path(), ec);
        chain.board.set_storage_path(settings_path);
        chain.board.load_user_presets();
        std::cout << "Preset edits: " << settings_path << "\n";
    }

    // The stream is allowed to fail. The dev UI, the footswitches and the LCD
    // are all still worth having on a Pi with nothing plugged into it — and
    // "audio stopped, here is why" is a far more useful screen than an exit
    // code, especially when the board is not in the room.
    bool audio_running = false;
    if (have_devices) {
        RtAudio::StreamParameters in_params;
        in_params.deviceId = in_dev.id;
        // One input channel: a guitar is one signal, and it arrives on the
        // interface's first input. RtAudio converts from however many channels
        // the device natively opens.
        in_params.nChannels = 1;
        RtAudio::StreamParameters out_params;
        out_params.deviceId = out_dev.id;
        // Both sides of a pair of headphones, where the device has two. Capped
        // at two on purpose: on a multi-output interface the headphone jack is
        // channels 1/2, and filling the rest would be noise in someone's
        // monitors.
        const unsigned int out_channels = std::min(out_dev.out_channels, 2u);
        out_params.nChannels = out_channels;

        if (open_stream(audio, &out_params, &in_params, &buffer_frames, sample_rate, &chain,
                        &audio_status)) {
            // openStream is allowed to grant a different buffer size than
            // asked, so both of these are sized against what came back -- and
            // both must happen before startStream lets a callback run.
            chain.prepare(buffer_frames, out_channels);
            g_telemetry.configure(static_cast<float>(sample_rate), buffer_frames);

            audio_running = start_stream(audio, &audio_status);
            if (audio_running) {
                audio_status.clear();
                std::cout << "Audio: " << in_dev.name << " -> " << out_dev.name << ", "
                          << sample_rate << " Hz, " << buffer_frames << "-frame buffer, "
                          << out_channels << " out channel" << (out_channels == 1 ? "" : "s")
                          << ".\n";
            }
        }
        if (!audio_running) {
            std::cerr << "Audio not started: " << audio_status << "\n";
        }
    } else {
        std::cerr << "Audio not started: " << audio_status << "\n";
    }
    std::cout << "Preset: " << chain.board.presets()[chain.board.current()].name << " ("
              << chain.board.preset_count() << " available). Ctrl+C to stop.\n";

    // ------------------------------------------------------------- simulator
    std::atomic<bool> simulate{opt.simulate};
    std::thread sim_thread;
    if (!audio_running) {
        chain.prepare(buffer_frames, 1);
        sim_thread = std::thread([&]() {
            SignalGenerator gen(static_cast<float>(sample_rate));
            std::vector<float> in(buffer_frames);
            std::vector<float> out(buffer_frames);
            const auto period = std::chrono::nanoseconds(
                static_cast<long long>(1e9 * buffer_frames / sample_rate));
            auto next = std::chrono::steady_clock::now();
            while (!g_stop.load(std::memory_order_relaxed)) {
                next += period;
                if (simulate.load(std::memory_order_relaxed)) {
                    gen.fill(in.data(), buffer_frames);
                    run_block(chain, in.data(), out.data(), buffer_frames);
                }
                std::this_thread::sleep_until(next);
            }
        });
    }

    // ------------------------------------------------------- control surface
    bool have_looper_switch = false;
    bool have_utility_switch = false;
    bool have_leds = false;
    bool have_lcd = false;

    std::unique_ptr<WebServer> web;
    if (opt.web) {
        web = std::make_unique<WebServer>(g_telemetry, opt.port);
    }
    const auto note = [&web](std::string message) {
        std::cout << message << "\n";
        if (web) web->log(std::move(message));
    };

#ifdef PEDAL_HAVE_GPIO
    // Pi 5 exposes its 40-pin header through the RP1 chip, which this kernel
    // enumerates as gpiochip0 (confirmed with `gpiodetect`). Run `gpiodetect`
    // if the board or kernel differs.
    constexpr const char* kGpioChip = "gpiochip0";
    constexpr unsigned int kLooperSwitchLine = 17;   // physical pin 11
    // Only requested under --switches 2. Nothing is wired here in a one-switch
    // build, and the web UI covers both of its gestures.
    constexpr unsigned int kUtilitySwitchLine = 27;  // physical pin 13
    constexpr unsigned int kLooperLedLine = 22;      // physical pin 15
    constexpr unsigned int kPresetLedLine = 23;      // physical pin 16
    // LCD1602 in 4-bit mode: RS, E, D4-D7. R/W is tied to GND on the module.
    // Pins 29-37 are five consecutive inner-row GPIO lines, taken in the same
    // order as the LCD's own pins, so those five wires run parallel and cannot
    // be swapped by miscounting. The sixth cannot join them: the next inner
    // pin, 39, is hardwired GND. D7 therefore goes to pin 38 -- outer row, but
    // *after* pin 37 rather than between 35 and 37, which is where it used to
    // sit and why it had to cross two wires to reach the far end of the LCD.
    constexpr Lcd1602::Pins kLcdPins{
        /*rs=*/5,       // physical pin 29
        /*enable=*/6,   // physical pin 31
        /*d4=*/13,      // physical pin 33
        /*d5=*/19,      // physical pin 35
        /*d6=*/26,      // physical pin 37
        /*d7=*/20,      // physical pin 38
    };

    // Declared before the button so it outlives the polling thread that calls
    // into it.
    ClickDetector utility_clicks;

    std::unique_ptr<GpioButton> looper_switch;
    std::unique_ptr<GpioButton> utility_switch;
    std::unique_ptr<GpioLed> looper_led;
    std::unique_ptr<GpioLed> preset_led;
    std::unique_ptr<Lcd1602> lcd;

    try {
        looper_switch = std::make_unique<GpioButton>(kGpioChip, kLooperSwitchLine);
        // Still a bare press handler, deliberately: the mode is chosen in the
        // browser, so the stomp itself needs no gesture and fires the instant
        // the foot lands.
        looper_switch->start([&]() {
            if (opt.switches < 2 &&
                chain.mode.load(std::memory_order_relaxed) == ControlMode::Freeze) {
                chain.freeze.toggle();
                if (web) web->log(chain.freeze.frozen() ? "footswitch: freeze captured"
                                                        : "footswitch: freeze released");
                return;
            }
            chain.looper.on_trigger();
            if (web) web->log("footswitch: looper trigger");
        });
        have_looper_switch = true;
        std::cout << "Looper footswitch armed on line " << kLooperSwitchLine
                  << " (record -> play -> overdub).\n";
    } catch (const std::exception& e) {
        std::cerr << "Looper footswitch unavailable (" << e.what() << ").\n";
    }

    // The looper switch fires on the press edge and always will: the instant
    // the foot lands is the loop boundary, so it must never wait to find out
    // whether a second tap is coming. That is exactly why the gesture pair
    // lives over here instead. Both of these tolerate the delay -- being
    // ~350 ms late to clear a loop or step a preset is inaudible.
    if (opt.switches >= 2) {
        try {
            utility_switch = std::make_unique<GpioButton>(kGpioChip, kUtilitySwitchLine);
            utility_clicks.set_handlers(
                [&]() {
                    chain.looper.clear();
                    chain.clear_count.fetch_add(1, std::memory_order_relaxed);
                    note("footswitch: loop cleared");
                },
                [&]() {
                    const int next = chain.setlist.advance();
                    if (next < 0) {
                        note("footswitch: setlist is empty — pick presets in the web UI");
                        return;
                    }
                    chain.board.select(next);
                    note("footswitch: preset -> " +
                         chain.board.presets()[static_cast<std::size_t>(next)].name);
                });
            utility_switch->start(
                [&utility_clicks]() { utility_clicks.on_press(std::chrono::steady_clock::now()); },
                [&utility_clicks]() { utility_clicks.poll(std::chrono::steady_clock::now()); });
            have_utility_switch = true;
            std::cout << "Utility footswitch armed on line " << kUtilitySwitchLine
                      << " (tap: clear, double tap: next in setlist).\n";
        } catch (const std::exception& e) {
            std::cerr << "Utility footswitch unavailable (" << e.what() << ").\n";
        }
    }

    try {
        looper_led = std::make_unique<GpioLed>(kGpioChip, kLooperLedLine);
        preset_led = std::make_unique<GpioLed>(kGpioChip, kPresetLedLine);
        have_leds = true;
        std::cout << "Status LEDs armed on lines " << kLooperLedLine << " and " << kPresetLedLine
                  << ".\n";
    } catch (const std::exception& e) {
        std::cerr << "Status LEDs unavailable (" << e.what() << ").\n";
    }

    try {
        lcd = std::make_unique<Lcd1602>(kGpioChip, kLcdPins);
        lcd->write_line(0, "guitar-pedal-cpp");
        lcd->write_line(1, "starting...");
        have_lcd = true;
        std::cout << "LCD1602 armed (RS=" << kLcdPins.rs << ", E=" << kLcdPins.enable << ").\n";
    } catch (const std::exception& e) {
        std::cerr << "LCD1602 unavailable (" << e.what() << ").\n";
    }

    // One thread drives both LEDs and the LCD. 20 ms is fast enough that the blink edges
    // land within a frame of where they should and slow enough to be free.
    std::thread indicator;
    if (looper_led || preset_led || lcd) {
        indicator = std::thread([&]() {
            using namespace std::chrono_literals;
            LedPattern looper_pattern;
            LedPattern preset_pattern;
            int last_preset = chain.board.current();
            unsigned last_clear = chain.clear_count.load(std::memory_order_relaxed);

            while (!g_stop.load(std::memory_order_relaxed)) {
                const auto now = std::chrono::steady_clock::now();

                const Looper::State state = chain.looper.state();
                switch (state) {
                    case Looper::State::Empty:       looper_pattern.set_off(); break;
                    case Looper::State::Recording:   looper_pattern.set_solid(); break;
                    case Looper::State::Playing:     looper_pattern.set_blink(1000ms, now); break;
                    case Looper::State::Overdubbing: looper_pattern.set_blink(200ms, now); break;
                }

                const int preset = chain.board.current();
                if (preset != last_preset) {
                    last_preset = preset;
                    // Two quick flashes to acknowledge the change. This used to
                    // flash the preset number, which stopped being countable
                    // once the table grew past five entries — the LCD and the
                    // web UI both name the preset, so the LED only needs to say
                    // that something happened.
                    preset_pattern.flash_burst(2, 90ms, 110ms, now);
                }

                const unsigned clears = chain.clear_count.load(std::memory_order_relaxed);
                if (clears != last_clear) {
                    last_clear = clears;
                    // One long flash, visibly different from a preset count.
                    preset_pattern.flash_burst(1, 700ms, 100ms, now);
                }

                if (looper_led) looper_led->set(looper_pattern.value(now));
                if (preset_led) preset_led->set(preset_pattern.value(now));

                if (lcd) {
                    // write_line() no-ops when the text is unchanged, so this
                    // only costs GPIO traffic when something actually moved.
                    lcd->write_line(0, std::string("LOOP: ") + looper_state_name(state));
                    lcd->write_line(
                        1, "FX:   " + chain.board.presets()[static_cast<std::size_t>(preset)]
                                          .short_name);
                }

                std::this_thread::sleep_for(20ms);
            }
        });
    }
#else
    std::cout << "Built without GPIO support; no footswitches or status LEDs.\n";
#endif

    // -------------------------------------------------------------- web UI
    if (web) {
        WebServer::StaticInfo info;
        for (const auto& preset : chain.board.presets()) {
            info.presets.push_back({preset.id, preset.name, preset.blurb, preset.gear});
        }
        info.device_in = audio_running ? in_dev.name : "-";
        info.device_out = audio_running ? out_dev.name : "-";
        info.sample_rate = sample_rate;
        info.buffer_frames = buffer_frames;
        web->set_static_info(std::move(info));

        // The pedalboard owns the stages and already describes its own knobs,
        // so the table is translated rather than restated — adding an effect
        // changes Presets.cpp and nothing here. The looper is appended by hand
        // because it sits after the board, not inside it.
        std::vector<WebServer::Param> params;
        params.reserve(chain.board.params().size() + 1);
        for (const auto& p : chain.board.params()) {
            params.push_back({p.id, p.group, p.label, p.min, p.max, p.get, p.set});
        }
        params.push_back({"freeze.grain", "Freeze", "Grain ms", 40.0f, 1500.0f,
                          [&] { return chain.freeze.grain_ms(); },
                          [&](float v) { chain.freeze.set_grain_ms(v); }});
        params.push_back({"freeze.level", "Freeze", "Pad level", 0.0f, 2.0f,
                          [&] { return chain.freeze.level(); },
                          [&](float v) { chain.freeze.set_level(v); }});
        params.push_back({"freeze.decay", "Freeze", "Hold", 0.9f, 1.0f,
                          [&] { return chain.freeze.decay(); },
                          [&](float v) { chain.freeze.set_decay(v); }});
        params.push_back({"looper.level", "Looper", "Loop volume", 0.0f, 1.5f,
                          [&] { return chain.looper.level(); },
                          [&](float v) { chain.looper.set_level(v); }});
        params.push_back({"looper.decay", "Looper", "Overdub decay", 0.5f, 1.0f,
                          [&] { return chain.looper.overdub_decay(); },
                          [&](float v) { chain.looper.set_overdub_decay(v); }});
        web->set_params(std::move(params));

        WebServer::Callbacks cb;
        // Every one of these is an atomic store or a pending flag — the same
        // surfaces the footswitch thread uses. The web thread gets no special
        // access to the engine, and the audio thread never waits on it.
        cb.set_note = [&](int index, std::string text) {
            std::string error;
            if (chain.board.set_note(index, std::move(text), &error)) {
                note("web: note saved for " +
                     chain.board.presets()[static_cast<std::size_t>(index)].name);
            } else {
                note("could not save note: " + error);
            }
        };
        cb.get_notes = [&]() {
            std::vector<std::string> notes;
            notes.reserve(chain.board.presets().size());
            for (int i = 0; i < chain.board.preset_count(); ++i) {
                notes.push_back(chain.board.note(i));
            }
            return notes;
        };
        cb.freeze_toggle = [&]() {
            chain.freeze.toggle();
            if (web) web->log(chain.freeze.frozen() ? "web: freeze captured" : "web: freeze released");
        };
        cb.set_mode = [&](bool freeze_mode) {
            chain.mode.store(freeze_mode ? ControlMode::Freeze : ControlMode::Looper,
                             std::memory_order_relaxed);
            note(freeze_mode ? "web: footswitch -> FREEZE" : "web: footswitch -> LOOPER");
        };
        cb.setlist_advance = [&]() {
            const int next = chain.setlist.advance();
            if (next < 0) {
                note("web: setlist is empty");
                return;
            }
            chain.board.select(next);
            note("web: preset -> " + chain.board.presets()[static_cast<std::size_t>(next)].name);
        };
        cb.set_setlist = [&](std::vector<int> presets) {
            chain.setlist.set(std::move(presets), chain.board.preset_count());
            note("web: setlist set to " + std::to_string(chain.setlist.presets().size()) +
                 " preset(s)");
        };
        cb.set_preset = [&](int value) {
            if (value < 0 || value >= chain.board.preset_count()) return;
            chain.board.select(value);
            // Picking in the browser moves the footswitch's place in the
            // setlist too, so the next stomp steps on from what you are
            // actually hearing rather than from wherever it last left off.
            chain.setlist.sync_to(value);
            note("web: preset -> " + chain.board.presets()[static_cast<std::size_t>(value)].name);
        };
        cb.looper_trigger = [&]() {
            chain.looper.on_trigger();
            if (web) web->log("web: looper trigger");
        };
        cb.looper_clear = [&]() {
            chain.looper.clear();
            chain.clear_count.fetch_add(1, std::memory_order_relaxed);
            if (web) web->log("web: loop cleared");
        };
        cb.set_simulator = [&](bool on) {
            if (audio_running) {
                if (web) web->log("web: simulator ignored — the sound card owns the chain");
                return;
            }
            simulate.store(on, std::memory_order_relaxed);
            if (web) web->log(on ? "web: simulator on" : "web: simulator off");
        };
        cb.reset_stats = [&]() {
            g_telemetry.reset_peaks();
            if (web) web->log("web: counters reset");
        };
        cb.save_preset = [&]() {
            const int index = chain.board.current();
            const std::string name = chain.board.presets()[static_cast<std::size_t>(index)].name;
            std::string error;
            if (chain.board.save_user_preset(index, &error)) {
                note("saved settings for " + name);
            } else {
                note("could not save " + name + ": " + error);
            }
        };
        cb.reset_preset = [&]() {
            const int index = chain.board.current();
            const std::string name = chain.board.presets()[static_cast<std::size_t>(index)].name;
            std::string error;
            if (chain.board.reset_preset(index, &error)) {
                note(name + " restored to defaults");
            } else {
                note("could not reset " + name + ": " + error);
            }
        };
        cb.reset_all_presets = [&]() {
            std::string error;
            if (chain.board.reset_all(&error)) {
                note("every preset restored to defaults");
            } else {
                note("could not reset presets: " + error);
            }
        };
        web->set_callbacks(std::move(cb));

        web->set_state_provider([&]() {
            WebServer::DynamicState d;
            const int preset = chain.board.current();
            const auto& spec = chain.board.presets()[static_cast<std::size_t>(preset)];
            d.preset = preset;
            d.active_groups = spec.groups;
            d.preset_modified = chain.board.has_override(preset);
            d.compressor_reduction_db = chain.board.compressor().reduction_db();
            const Looper::State state = chain.looper.state();
            d.looper_state = looper_state_name(state);
            d.loop_frames = chain.looper.length();
            d.loop_position = chain.looper.position();
            d.audio_running = audio_running;
            d.audio_status = audio_status;
            d.simulator = simulate.load(std::memory_order_relaxed);
            // The same two strings the LCD gets, so the browser shows what the
            // panel shows even on a board with no LCD wired to it.
            d.lcd0 = std::string("LOOP: ") + looper_state_name(state);
            d.lcd1 = "FX:   " + spec.short_name;
            d.frozen = chain.freeze.frozen();
            d.freeze_mode = chain.mode.load(std::memory_order_relaxed) == ControlMode::Freeze;
            d.setlist = chain.setlist.presets();
            d.setlist_cursor = chain.setlist.cursor();
            d.have_looper_switch = have_looper_switch;
            d.have_utility_switch = have_utility_switch;
            d.have_leds = have_leds;
            d.have_lcd = have_lcd;
            return d;
        });

        try {
            web->start();
            std::cout << "Dev UI on http://0.0.0.0:" << opt.port << "/ (LAN only, no auth).\n";
            web->log("pedal started");
            if (!audio_running) web->log("audio not started: " + audio_status);
        } catch (const std::exception& e) {
            std::cerr << "Web UI unavailable (" << e.what() << ").\n";
            web.reset();
        }
    }

    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    const Telemetry::Snapshot final_stats = g_telemetry.snapshot();
    std::cout << "\nStopping. Xruns: " << final_stats.xruns << ", worst callback: "
              << final_stats.block_us_max << " us of a " << final_stats.budget_us << " us budget.\n";

    // Dump what each preset actually did, so "that one was too loud" becomes a
    // number to look at later rather than something to remember.
    {
        std::vector<std::string> names;
        names.reserve(chain.board.presets().size());
        for (const auto& preset : chain.board.presets()) names.push_back(preset.name);
        const std::string csv = stats_path(opt.settings);
        std::string error;
        if (chain.stats.write_csv(csv, names, &error)) {
            std::cout << "Per-preset levels written to " << csv << "\n";
        } else {
            std::cerr << "Could not write " << csv << ": " << error << "\n";
        }
    }

    if (web) web->stop();
    if (sim_thread.joinable()) sim_thread.join();

#ifdef PEDAL_HAVE_GPIO
    if (indicator.joinable()) indicator.join();
#endif

    if (audio.isStreamRunning()) audio.stopStream();
    if (audio.isStreamOpen()) audio.closeStream();

    return 0;
}
