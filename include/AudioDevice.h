#pragma once

#include <RtAudio.h>

#include <string>
#include <vector>

// RtAudio 6 replaced the throwing API with returned error codes and swapped
// device indices for stable ids. Debian trixie ships 6.x; older boards may not.
#if defined(RTAUDIO_VERSION_MAJOR) && RTAUDIO_VERSION_MAJOR >= 6
#define PEDAL_RTAUDIO6 1
#else
#define PEDAL_RTAUDIO6 0
#endif

// Picking the right sound card is its own problem, separate from both the
// bench tool and the pedal: on a Pi the "default" device is HDMI or nothing,
// and a device that advertises only 44100 makes a hardcoded 48000 look like
// broken hardware. Both audio_check and guitar_pedal need the same answers, so
// the logic lives here rather than twice.
namespace audiodev {

// Preferred, not required. Plenty of class-compliant interfaces advertise 44100
// and nothing else, so the rate is negotiated against the device rather than
// assumed.
constexpr unsigned int kPreferredRate = 48000;

struct Device {
    unsigned int id = 0;
    std::string name;
    unsigned int in_channels = 0;
    unsigned int out_channels = 0;
    unsigned int preferred_rate = 0;
    bool default_in = false;
    bool default_out = false;
    std::vector<unsigned int> rates;
};

std::vector<Device> enumerate(RtAudio& audio);

// Accepts a numeric id or a case-insensitive substring of the device name.
// Returns false and writes an explanation to `error` rather than guessing
// between two matches.
bool resolve(const std::vector<Device>& devices, const std::string& spec, bool need_input,
             Device* out, std::string* error);

// Picks the interface most likely to be the guitar path when no device is
// named: a duplex device beats whatever the system calls "default".
bool guess_device(const std::vector<Device>& devices, bool need_input, bool need_output,
                  Device* out);

// Devices advertise a rate list; honour it rather than insisting on 48k.
unsigned int pick_rate(const Device& d);

// Two different devices must agree on ONE rate: an RtAudio duplex stream has a
// single sampleRate for both directions. Returns false with an explanation when
// their rate lists don't intersect — a failure no buffer size can fix.
bool pick_shared_rate(const Device& in_dev, const Device& out_dev, unsigned int* rate,
                      std::string* error);

}  // namespace audiodev
