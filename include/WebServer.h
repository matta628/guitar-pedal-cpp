#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Telemetry;

// The dev/testing UI: a browser on the laptop, served by the pedal itself.
//
// One thread, one poll() loop, no third-party HTTP or JSON library. That is a
// deliberate size choice, not stubbornness — the whole surface is six routes
// and a handful of scalars, and pulling in a server framework would add more
// build and deployment risk to a Pi than it removes.
//
// It is emphatically *not* on the audio path. It reads Telemetry (relaxed
// atomics, seqlock for the waveform) and it writes commands through the same
// thread-safe surfaces the footswitch thread already uses: an atomic preset
// index and the looper's pending-action flags. If this thread stalls, blocks or
// dies, the audio callback does not notice.
//
// No authentication, by design: it binds on the LAN for bench work. Don't
// forward the port.
class WebServer {
public:
    // One tweakable knob on a DSP stage. main owns the stages, so it supplies
    // the accessors; the server stays ignorant of what a "reverb" is.
    struct Param {
        std::string id;     // "reverb.mix" — the key the browser posts back
        std::string group;  // "Reverb"     — section heading in the UI
        std::string label;  // "Mix"
        float min = 0.0f;
        float max = 1.0f;
        std::function<float()> get;
        std::function<void(float)> set;
    };

    // What the UI shows about one preset. Fixed for the life of the program,
    // so it is sent once rather than on every frame.
    struct PresetInfo {
        std::string id;
        std::string name;
        std::string blurb;   // what it is trying to sound like
        std::string gear;    // the hardware it approximates
    };

    // Set once at startup: everything that can't change while running.
    struct StaticInfo {
        std::vector<PresetInfo> presets;
        std::string device_in;
        std::string device_out;
        unsigned int sample_rate = 0;
        unsigned int buffer_frames = 0;
    };

    // Rebuilt on every UI frame by main's state provider, on the web thread.
    struct DynamicState {
        int preset = 0;
        std::string looper_state = "EMPTY";
        std::uint64_t loop_frames = 0;     // 0 until a loop has been recorded
        std::uint64_t loop_position = 0;
        bool audio_running = false;
        std::string audio_status;          // why, when it isn't running
        bool simulator = false;
        std::string lcd0;                  // mirror of the physical LCD1602
        std::string lcd1;
        bool have_looper_switch = false;
        bool have_utility_switch = false;
        // Preset indices the second footswitch walks, and the position within
        // that list -- not a preset index -- or -1 before the first step.
        std::vector<int> setlist;
        int setlist_cursor = -1;
        bool have_leds = false;
        bool have_lcd = false;
        // Parameter groups the current preset actually runs. The UI dims the
        // rest rather than hiding them, so a knob that is doing nothing looks
        // different from one that is missing.
        std::vector<std::string> active_groups;
        // Whether this preset has saved edits sitting on top of its defaults.
        bool preset_modified = false;
        float compressor_reduction_db = 0.0f;
    };

    struct Callbacks {
        std::function<void(std::vector<int>)> set_setlist;
        std::function<void(int)> set_preset;
        std::function<void()> looper_trigger;
        std::function<void()> looper_clear;
        std::function<void(bool)> set_simulator;
        std::function<void()> reset_stats;
        // Persist the current knob positions for the selected preset, drop
        // them again, or drop every preset's. These write to disk, which is
        // fine on the web thread and unthinkable on the audio thread.
        std::function<void()> save_preset;
        std::function<void()> reset_preset;
        std::function<void()> reset_all_presets;
    };

    WebServer(const Telemetry& telemetry, std::uint16_t port);
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // All of these must be called before start().
    void set_static_info(StaticInfo info);
    void set_state_provider(std::function<DynamicState()> provider);
    void set_params(std::vector<Param> params);
    void set_callbacks(Callbacks callbacks);

    // Appends to the UI's event log. Thread-safe (takes a mutex), which is
    // exactly why the audio thread must never call it.
    void log(std::string message);

    // Throws std::runtime_error if the socket can't be bound.
    void start();
    void stop();

    std::uint16_t port() const { return port_; }

private:
    struct Connection {
        int fd = -1;
        std::string in_buf;
        std::string out_buf;
        bool sse = false;         // long-lived push connection
        bool close_when_drained = false;
    };

    struct LogEntry {
        std::uint64_t id;
        std::string when;
        std::string text;
    };

    void run();
    void accept_ready();
    void flush(Connection& c);
    void on_readable(Connection& c);
    bool handle_request(Connection& c);
    void route(Connection& c, const std::string& method, const std::string& path,
               const std::string& query);
    void push_to_subscribers();

    // full_log = true on a fresh request (the browser needs the backlog);
    // false on a stream frame, which only carries the newest few entries.
    std::string state_json(bool full_log);
    std::string scope_binary();
    std::string params_json();

    const Telemetry& telemetry_;
    std::uint16_t port_;
    int listen_fd_ = -1;
    int wake_fd_ = -1;  // eventfd, so stop() doesn't wait out the poll timeout

    StaticInfo static_info_;
    std::function<DynamicState()> state_provider_;
    std::vector<Param> params_;
    Callbacks callbacks_;

    std::vector<Connection> connections_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    std::mutex log_mutex_;
    std::deque<LogEntry> log_;
    std::uint64_t log_next_id_ = 1;
};
