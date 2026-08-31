#pragma once

#include <mutex>
#include <vector>

// The ordered subset of presets a footswitch walks, and where in it you are.
//
// Cycling all 34 presets by foot is unusable -- reaching a given sound can
// cost thirty stomps, which is why footswitch preset-cycling was removed from
// this build once. A setlist fixes that by cycling only what was chosen for
// tonight, in the order chosen.
//
// Threading: written by the web thread, read and advanced by the footswitch
// thread, so it is guarded by a plain mutex. That is safe precisely because
// **the audio thread never touches this class**. Advancing publishes its
// result by calling Pedalboard::select(), which is the atomic store the audio
// callback actually reads; nothing here is in the callback's path.
class Setlist {
public:
    // Replaces the whole list. Indices are positions in Pedalboard::presets().
    // Out-of-range entries are dropped rather than trusted, since these arrive
    // from an HTTP request. Duplicates are kept: repeating a preset in a set is
    // a legitimate thing to want.
    void set(std::vector<int> presets, int preset_count);

    std::vector<int> presets() const;
    bool empty() const;

    // The next preset to select, wrapping at the end, or -1 when the list is
    // empty. Call this from the footswitch thread and hand the result to
    // Pedalboard::select().
    int advance();

    // Point the cursor at `preset` if it is in the list, so that a preset
    // chosen in the browser doesn't make the next stomp jump somewhere
    // unrelated. No-op when it isn't a member.
    void sync_to(int preset);

    // Position within the list, not a preset index; -1 when empty.
    int cursor() const;

private:
    mutable std::mutex mutex_;
    std::vector<int> presets_;
    int cursor_ = -1;
};
