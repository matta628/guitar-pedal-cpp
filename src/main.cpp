#include <RtAudio.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};
std::atomic<int> g_xrun_count{0};

void on_sigint(int) { g_stop.store(true, std::memory_order_relaxed); }

int passthrough_callback(void* output_buffer, void* input_buffer, unsigned int n_frames,
                          double /*stream_time*/, RtAudioStreamStatus status, void* /*user_data*/) {
    if (status) {
        g_xrun_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(output_buffer, input_buffer, n_frames * sizeof(float));
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

    try {
        audio.openStream(&out_params, &in_params, RTAUDIO_FLOAT32, sample_rate, &buffer_frames,
                          &passthrough_callback);
        audio.startStream();
    } catch (const std::exception& e) {
        std::cerr << "RtAudio error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "guitar-pedal-cpp: passthrough running at " << sample_rate << " Hz, "
              << buffer_frames << "-frame buffer. Ctrl+C to stop.\n";

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
