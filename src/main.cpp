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
#include "GpioButton.h"
#endif

namespace {

std::atomic<bool> g_stop{false};
std::atomic<int> g_xrun_count{0};

void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

struct ShoegazeChain {
    Distortion distortion;
    Chorus chorus;
    Reverb reverb;
    Looper looper;

    explicit ShoegazeChain(float sample_rate)
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
};

int shoegaze_callback(void* output_buffer, void* input_buffer, unsigned int n_frames,
                       double /*stream_time*/, RtAudioStreamStatus status, void* user_data) {
    if (status) {
        g_xrun_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(output_buffer, input_buffer, n_frames * sizeof(float));

    auto* chain = static_cast<ShoegazeChain*>(user_data);
    auto* out = static_cast<float*>(output_buffer);
    chain->distortion.process(out, n_frames);
    chain->chorus.process(out, n_frames);
    chain->reverb.process(out, n_frames);
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

    ShoegazeChain chain(static_cast<float>(sample_rate));

    try {
        audio.openStream(&out_params, &in_params, RTAUDIO_FLOAT32, sample_rate, &buffer_frames,
                          &shoegaze_callback, &chain);
        audio.startStream();
    } catch (const std::exception& e) {
        std::cerr << "RtAudio error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "guitar-pedal-cpp: shoegaze mode (fuzz + chorus + reverb + looper) running at "
              << sample_rate << " Hz, " << buffer_frames << "-frame buffer. Ctrl+C to stop.\n";

#ifdef PEDAL_HAVE_GPIO
    // Unverified: chip name and line offset need confirming on the actual Pi
    // with `gpioinfo` before this will do anything. Wire one leg of the
    // button to this GPIO line, the other to GND (internal pull-up handles
    // the rest).
    std::unique_ptr<GpioButton> footswitch;
    try {
        footswitch = std::make_unique<GpioButton>("gpiochip4", 17);
        footswitch->start([&chain]() { chain.looper.on_trigger(); });
        std::cout << "Footswitch armed on gpiochip4 line 17.\n";
    } catch (const std::exception& e) {
        std::cerr << "Footswitch unavailable (" << e.what() << "); looper has no physical trigger yet.\n";
    }
#else
    std::cout << "Built without GPIO support; looper has no physical trigger.\n";
#endif

    while (!g_stop.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "\nStopping. Xruns: " << g_xrun_count.load(std::memory_order_relaxed) << "\n";

    audio.stopStream();
    if (audio.isStreamOpen()) {
        audio.closeStream();
    }

    return 0;
}
