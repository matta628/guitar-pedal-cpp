#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Per-preset running totals, so "which of these is too hot?" can be answered
// from measurement rather than memory.
//
// A preset can be quietly unusable: loud enough that an ordinary pick attack
// clips the output, which is inaudible as a cause and very audible as an
// effect. Playing through 34 of them and remembering which misbehaved is not a
// thing a person can do, so the audio thread keeps score instead.
//
// Real-time safety is the whole design constraint here. The vector is sized
// once, before the stream starts, and never resized; recording a block is a
// handful of relaxed atomic RMWs into a fixed slot. Nothing allocates, nothing
// locks, and nothing here can make the callback miss its deadline.
class PresetStats {
public:
    struct Row {
        std::uint64_t blocks = 0;   // buffers processed while this preset was live
        std::uint64_t clips = 0;    // output samples at or past full scale
        float out_peak = 0.0f;      // loudest output sample ever seen, linear
        float in_peak = 0.0f;       // loudest input, for context on the above
    };

    // Call before the stream starts, never while it is running.
    void configure(int preset_count);

    // ---- audio thread only ----
    void record(int preset, float in_peak, float out_peak, std::uint32_t clips);

    // ---- any thread ----
    std::vector<Row> rows() const;
    void reset();

    // A CSV of everything recorded so far, newest measurement included.
    // Written by the web thread or at shutdown -- never by the audio thread,
    // which must not touch a filesystem.
    bool write_csv(const std::string& path, const std::vector<std::string>& names,
                   std::string* error = nullptr) const;

private:
    // One cache-line-ish slot per preset. Not padded: contention is nil
    // because only one thread ever writes, and only one slot is live at a time.
    struct Slot {
        std::atomic<std::uint64_t> blocks{0};
        std::atomic<std::uint64_t> clips{0};
        std::atomic<float> out_peak{0.0f};
        std::atomic<float> in_peak{0.0f};
    };
    std::vector<Slot> slots_;
};
