#include "Setlist.h"

#include <algorithm>
#include <utility>

void Setlist::set(std::vector<int> presets, int preset_count) {
    std::vector<int> clean;
    clean.reserve(presets.size());
    for (int p : presets) {
        if (p >= 0 && p < preset_count) clean.push_back(p);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    presets_ = std::move(clean);
    // Starting at -1 rather than 0 means the first stomp selects the first
    // entry, instead of skipping it to land on the second.
    cursor_ = -1;
}

std::vector<int> Setlist::presets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return presets_;
}

bool Setlist::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return presets_.empty();
}

int Setlist::advance() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (presets_.empty()) return -1;
    cursor_ = (cursor_ + 1) % static_cast<int>(presets_.size());
    return presets_[static_cast<std::size_t>(cursor_)];
}

void Setlist::sync_to(int preset) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find(presets_.begin(), presets_.end(), preset);
    if (it != presets_.end()) {
        cursor_ = static_cast<int>(it - presets_.begin());
    }
}

int Setlist::cursor() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cursor_;
}
