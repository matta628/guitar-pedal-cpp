#include <RtAudio.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <thread>

#include "Chorus.h"
#include "Distortion.h"
#include "Looper.h"
#include "Reverb.h"

#ifdef PEDAL_HAVE_GPIO
#include "ClickDetector.h"
#include "GpioButton.h"
#include "GpioLed.h"
#include "Lcd1602.h"
#include "LedPattern.h"
#endif

namespace {

std::atomic<bool> g_stop{false};
std::atomic<int> g_xrun_count{0};

void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

// Cycled by the utility footswitch. Clean is a real entry, not a bypass flag:
// the looper runs in every preset, so "no effect" still records and plays back.
enum class Preset { Shoegaze = 0, Clean, Fuzz, Chorus, Reverb };
constexpr int kPresetCount = 5;

const char* preset_name(int preset) {
    switch (static_cast<Preset>(preset)) {
        case Preset::Shoegaze: return "shoegaze (fuzz + chorus + reverb)";
        case Preset::Clean:    return "clean (no effect)";
        case Preset::Fuzz:     return "fuzz";
        case Preset::Chorus:   return "chorus";
        case Preset::Reverb:   return "reverb";
    }
    return "unknown";
}

// The LCD has 16 columns, so preset names get a short form of their own
// rather than being silently truncated by write_line().
const char* preset_short_name(int preset) {
    switch (static_cast<Preset>(preset)) {
        case Preset::Shoegaze: return "SHOEGAZE";
        case Preset::Clean:    return "CLEAN";
        case Preset::Fuzz:     return "FUZZ";
        case Preset::Chorus:   return "CHORUS";
        case Preset::Reverb:   return "REVERB";
    }
    return "?";
}

const char* looper_state_name(Looper::State state) {
    switch (state) {
        case Looper::State::Empty:       return "EMPTY";
        case Looper::State::Recording:   return "REC";
        case Looper::State::Playing:     return "PLAY";
        case Looper::State::Overdubbing: return "OVERDUB";
    }
    return "?";
}

struct PedalChain {
    Distortion distortion;
    Chorus chorus;
    Reverb reverb;
    Looper looper;

    // Written by the footswitch thread, read by the audio thread every buffer.
    std::atomic<int> preset{static_cast<int>(Preset::Shoegaze)};
    // Bumped on every clear so the indicator loop can flash an acknowledgement
    // without the click handler touching a LedPattern from another thread.
    std::atomic<unsigned> clear_count{0};

    explicit PedalChain(float sample_rate)
        : chorus(sample_rate), reverb(sample_rate), looper(sample_rate) {
        distortion.set_drive(2.5f);
        distortion.set_mix(0.4f);
        chorus.set_rate_hz(0.6f);
        chorus.set_depth_ms(3.0f);
        chorus.set_mix(0.5f);
        reverb.set_room_size(0.7f);
        reverb.set_damping(0.4f);
        reverb.set_mix(0.4f);
    }

    int cycle_preset() {
        const int next = (preset.load(std::memory_order_relaxed) + 1) % kPresetCount;
        preset.store(next, std::memory_order_relaxed);
        return next;
    }
};

int audio_callback(void* output_buffer, void* input_buffer, unsigned int n_frames,
                   double /*stream_time*/, RtAudioStreamStatus status, void* user_data) {
    if (status) {
        g_xrun_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(output_buffer, input_buffer, n_frames * sizeof(float));

    auto* chain = static_cast<PedalChain*>(user_data);
    auto* out = static_cast<float*>(output_buffer);

    switch (static_cast<Preset>(chain->preset.load(std::memory_order_relaxed))) {
        case Preset::Shoegaze:
            chain->distortion.process(out, n_frames);
            chain->chorus.process(out, n_frames);
            chain->reverb.process(out, n_frames);
            break;
        case Preset::Clean:
            break;
        case Preset::Fuzz:
            chain->distortion.process(out, n_frames);
            break;
        case Preset::Chorus:
            chain->chorus.process(out, n_frames);
            break;
        case Preset::Reverb:
            chain->reverb.process(out, n_frames);
            break;
    }

    // Outside the switch on purpose: the looper sits after whatever the preset
    // selected, and keeps running even on the clean preset.
    chain->looper.process(out, n_frames);

    return 0;
}

}  // namespace

int main() {
    std::signal(SIGINT, on_sigint);

    RtAudio audio;
    if (audio.getDeviceCount() == 0) {
        std::cerr << "No audio devices found\n";
        return 1;
    }

    RtAudio::StreamParameters in_params;
    in_params.deviceId = audio.getDefaultInputDevice();
    in_params.nChannels = 1;

    RtAudio::StreamParameters out_params;
    out_params.deviceId = audio.getDefaultOutputDevice();
    out_params.nChannels = 1;

    unsigned int sample_rate = 48000;
    unsigned int buffer_frames = 256;

    PedalChain chain(static_cast<float>(sample_rate));

    try {
        audio.openStream(&out_params, &in_params, RTAUDIO_FLOAT32, sample_rate, &buffer_frames,
                          &audio_callback, &chain);
        audio.startStream();
    } catch (const std::exception& e) {
        std::cerr << "RtAudio error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "guitar-pedal-cpp running at " << sample_rate << " Hz, " << buffer_frames
              << "-frame buffer. Preset: " << preset_name(chain.preset.load()) << ". Ctrl+C to stop.\n";

#ifdef PEDAL_HAVE_GPIO
    // Pi 5 exposes its 40-pin header through the RP1 chip, which this kernel
    // enumerates as gpiochip0 (confirmed with `gpiodetect`). Run `gpiodetect`
    // if the board or kernel differs.
    constexpr const char* kGpioChip = "gpiochip0";
    constexpr unsigned int kLooperSwitchLine = 17;   // physical pin 11
    constexpr unsigned int kUtilitySwitchLine = 27;  // physical pin 13
    constexpr unsigned int kLooperLedLine = 22;      // physical pin 15
    constexpr unsigned int kPresetLedLine = 23;      // physical pin 16
    // LCD1602 in 4-bit mode: RS, E, D4-D7. R/W is tied to GND on the module.
    constexpr Lcd1602::Pins kLcdPins{
        /*rs=*/5,       // physical pin 29
        /*enable=*/6,   // physical pin 31
        /*d4=*/13,      // physical pin 33
        /*d5=*/19,      // physical pin 35
        /*d6=*/26,      // physical pin 37
        /*d7=*/16,      // physical pin 36
    };

    // Declared before the buttons so it outlives their polling threads, which
    // call into it.
    ClickDetector utility_clicks;

    std::unique_ptr<GpioButton> looper_switch;
    std::unique_ptr<GpioButton> utility_switch;
    std::unique_ptr<GpioLed> looper_led;
    std::unique_ptr<GpioLed> preset_led;
    std::unique_ptr<Lcd1602> lcd;

    try {
        looper_switch = std::make_unique<GpioButton>(kGpioChip, kLooperSwitchLine);
        looper_switch->start([&chain]() { chain.looper.on_trigger(); });
        std::cout << "Looper footswitch armed on line " << kLooperSwitchLine
                  << " (record -> play -> overdub).\n";
    } catch (const std::exception& e) {
        std::cerr << "Looper footswitch unavailable (" << e.what() << ").\n";
    }

    try {
        utility_switch = std::make_unique<GpioButton>(kGpioChip, kUtilitySwitchLine);
        utility_clicks.set_handlers(
            [&chain]() {
                chain.looper.clear();
                chain.clear_count.fetch_add(1, std::memory_order_relaxed);
                std::cout << "Loop cleared.\n";
            },
            [&chain]() {
                std::cout << "Preset: " << preset_name(chain.cycle_preset()) << "\n";
            });
        utility_switch->start(
            [&utility_clicks]() { utility_clicks.on_press(std::chrono::steady_clock::now()); },
            [&utility_clicks]() { utility_clicks.poll(std::chrono::steady_clock::now()); });
        std::cout << "Utility footswitch armed on line " << kUtilitySwitchLine
                  << " (click = clear, double-click = cycle preset).\n";
    } catch (const std::exception& e) {
        std::cerr << "Utility footswitch unavailable (" << e.what() << ").\n";
    }

    try {
        looper_led = std::make_unique<GpioLed>(kGpioChip, kLooperLedLine);
        preset_led = std::make_unique<GpioLed>(kGpioChip, kPresetLedLine);
        std::cout << "Status LEDs armed on lines " << kLooperLedLine << " and " << kPresetLedLine
                  << ".\n";
    } catch (const std::exception& e) {
        std::cerr << "Status LEDs unavailable (" << e.what() << ").\n";
    }

    try {
        lcd = std::make_unique<Lcd1602>(kGpioChip, kLcdPins);
        lcd->write_line(0, "guitar-pedal-cpp");
        lcd->write_line(1, "starting...");
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
            int last_preset = chain.preset.load(std::memory_order_relaxed);
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

                const int preset = chain.preset.load(std::memory_order_relaxed);
                if (preset != last_preset) {
                    last_preset = preset;
                    // preset+1 flashes, so the LED counts out which preset is
                    // live without needing the terminal.
                    preset_pattern.flash_burst(preset + 1, 120ms, 180ms, now);
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
                    lcd->write_line(1, std::string("FX:   ") + preset_short_name(preset));
                }

                std::this_thread::sleep_for(20ms);
            }
        });
    }
#else
    std::cout << "Built without GPIO support; no footswitches or status LEDs.\n";
#endif

    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\nStopping. Xruns: " << g_xrun_count.load(std::memory_order_relaxed) << "\n";

#ifdef PEDAL_HAVE_GPIO
    if (indicator.joinable()) {
        indicator.join();
    }
#endif

    audio.stopStream();
    if (audio.isStreamOpen()) {
        audio.closeStream();
    }

    return 0;
}
