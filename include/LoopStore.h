#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Saved loops on disk, as mono 16-bit PCM WAV.
//
// WAV rather than a raw dump of the float buffer because a saved loop should be
// worth something outside this program: it drops into any DAW, any player, any
// phone. The looper's own buffer is float, so saving costs one conversion --
// 16-bit carries 96 dB of range, which is far more than a guitar loop recorded
// through a bus-powered interface will ever use.
//
// Every method that touches the filesystem runs on the web thread. None of this
// is safe to call from the audio callback, and none of it needs to be: the
// audio thread hands over a copy and gets one back.
class LoopStore {
public:
    struct Entry {
        std::string name;
        std::size_t frames = 0;
        unsigned int rate = 0;
        float seconds = 0.0f;
    };

    // dir is created on first write if it does not exist.
    explicit LoopStore(std::string dir);

    // Newest first, so the list reads as a history rather than an alphabet.
    std::vector<Entry> list() const;

    bool save(const std::string& name, const std::vector<float>& samples, unsigned int rate,
              std::string* error);
    bool load(const std::string& name, std::vector<float>* out, unsigned int* rate,
              std::string* error);
    bool remove(const std::string& name, std::string* error);

    const std::string& dir() const { return dir_; }

    // A loop name arrives from a browser, and it is about to become a path.
    // Anything outside [A-Za-z0-9 _-] is dropped rather than escaped, which
    // makes "../../.ssh/authorized_keys" collapse to "sshauthorized_keys"
    // instead of being rejected with an error that invites another attempt.
    // Returns an empty string if nothing usable survives.
    static std::string sanitise(const std::string& name);

private:
    std::string path_for(const std::string& safe_name) const;
    std::string dir_;
};
