#include "PresetStats.h"

#include <cmath>
#include <cstdio>
#include <fstream>

namespace {

// Raise `slot` to `value` if it is larger. A CAS loop rather than a plain
// store because a store would lose a peak recorded between the load and the
// write -- the same reason audio_check's meter uses one.
void bump_peak(std::atomic<float>& slot, float value) {
    float prev = slot.load(std::memory_order_relaxed);
    while (value > prev && !slot.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
    }
}

}  // namespace

void PresetStats::configure(int preset_count) {
    slots_ = std::vector<Slot>(preset_count > 0 ? static_cast<std::size_t>(preset_count) : 0);
}

void PresetStats::record(int preset, float in_peak, float out_peak, std::uint32_t clips) {
    if (preset < 0 || static_cast<std::size_t>(preset) >= slots_.size()) return;
    Slot& s = slots_[static_cast<std::size_t>(preset)];
    s.blocks.fetch_add(1, std::memory_order_relaxed);
    if (clips > 0) s.clips.fetch_add(clips, std::memory_order_relaxed);
    bump_peak(s.out_peak, out_peak);
    bump_peak(s.in_peak, in_peak);
}

std::vector<PresetStats::Row> PresetStats::rows() const {
    std::vector<Row> out;
    out.reserve(slots_.size());
    for (const Slot& s : slots_) {
        out.push_back(Row{s.blocks.load(std::memory_order_relaxed),
                          s.clips.load(std::memory_order_relaxed),
                          s.out_peak.load(std::memory_order_relaxed),
                          s.in_peak.load(std::memory_order_relaxed)});
    }
    return out;
}

void PresetStats::reset() {
    for (Slot& s : slots_) {
        s.blocks.store(0, std::memory_order_relaxed);
        s.clips.store(0, std::memory_order_relaxed);
        s.out_peak.store(0.0f, std::memory_order_relaxed);
        s.in_peak.store(0.0f, std::memory_order_relaxed);
    }
}

bool PresetStats::write_csv(const std::string& path, const std::vector<std::string>& names,
                            std::string* error) const {
    std::ofstream f(path, std::ios::trunc);
    if (!f) {
        if (error) *error = "could not open " + path;
        return false;
    }
    f << "preset,blocks,clips,out_peak_dbfs,in_peak_dbfs\n";
    const std::vector<Row> data = rows();
    for (std::size_t i = 0; i < data.size(); ++i) {
        const Row& r = data[i];
        // Presets never played are noise in the report; skip them.
        if (r.blocks == 0) continue;
        const std::string name = i < names.size() ? names[i] : ("#" + std::to_string(i));
        char buf[64];
        auto db = [&buf](float linear) -> const char* {
            if (linear <= 0.0f) return "-inf";
            std::snprintf(buf, sizeof(buf), "%.1f", 20.0 * std::log10(static_cast<double>(linear)));
            return buf;
        };
        f << '"' << name << "\"," << r.blocks << ',' << r.clips << ',';
        f << db(r.out_peak) << ',';
        f << db(r.in_peak) << '\n';
    }
    return static_cast<bool>(f);
}
