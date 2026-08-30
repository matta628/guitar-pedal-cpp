#include "AudioDevice.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace audiodev {
namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string rate_list(const std::vector<unsigned int>& rates) {
    std::ostringstream out;
    for (unsigned int r : rates) out << " " << r;
    return out.str();
}

}  // namespace

std::vector<Device> enumerate(RtAudio& audio) {
    std::vector<Device> out;
#if PEDAL_RTAUDIO6
    for (unsigned int id : audio.getDeviceIds()) {
        RtAudio::DeviceInfo info = audio.getDeviceInfo(id);
        out.push_back({id, info.name, info.inputChannels, info.outputChannels,
                       info.preferredSampleRate, info.isDefaultInput, info.isDefaultOutput,
                       info.sampleRates});
    }
#else
    const unsigned int count = audio.getDeviceCount();
    for (unsigned int i = 0; i < count; ++i) {
        RtAudio::DeviceInfo info = audio.getDeviceInfo(i);
        if (!info.probed) continue;
        out.push_back({i, info.name, info.inputChannels, info.outputChannels,
                       info.preferredSampleRate, info.isDefaultInput, info.isDefaultOutput,
                       info.sampleRates});
    }
#endif
    return out;
}

bool resolve(const std::vector<Device>& devices, const std::string& spec, bool need_input,
             Device* out, std::string* error) {
    std::ostringstream why;

    if (!spec.empty() && spec.find_first_not_of("0123456789") == std::string::npos) {
        const unsigned int want = static_cast<unsigned int>(std::stoul(spec));
        for (const Device& d : devices) {
            if (d.id == want) { *out = d; return true; }
        }
        why << "no device with id " << want << " — run `audio_check list`";
        if (error) *error = why.str();
        return false;
    }

    std::vector<const Device*> hits;
    const std::string needle = lower(spec);
    for (const Device& d : devices) {
        if (lower(d.name).find(needle) != std::string::npos) hits.push_back(&d);
    }
    if (hits.empty()) {
        why << "no device name contains \"" << spec << "\" — run `audio_check list`";
        if (error) *error = why.str();
        return false;
    }
    if (hits.size() > 1) {
        why << "\"" << spec << "\" matches " << hits.size() << " devices:";
        for (const Device* d : hits) why << "\n  " << d->id << "  " << d->name;
        why << "\npass the id instead.";
        if (error) *error = why.str();
        return false;
    }
    if (need_input && hits[0]->in_channels == 0) {
        why << hits[0]->name << " has no input channels.";
        if (error) *error = why.str();
        return false;
    }
    *out = *hits[0];
    return true;
}

bool guess_device(const std::vector<Device>& devices, bool need_input, bool need_output,
                  Device* out) {
    const Device* best = nullptr;
    for (const Device& d : devices) {
        if (need_input && d.in_channels == 0) continue;
        if (need_output && d.out_channels == 0) continue;
        const bool duplex = d.in_channels > 0 && d.out_channels > 0;
        if (best == nullptr) { best = &d; continue; }
        const bool best_duplex = best->in_channels > 0 && best->out_channels > 0;
        if (duplex && !best_duplex) best = &d;
    }
    if (best == nullptr) return false;
    *out = *best;
    return true;
}

unsigned int pick_rate(const Device& d) {
    for (unsigned int r : d.rates) {
        if (r == kPreferredRate) return r;
    }
    if (d.preferred_rate > 0) return d.preferred_rate;
    if (!d.rates.empty()) return d.rates.front();
    return kPreferredRate;
}

bool pick_shared_rate(const Device& in_dev, const Device& out_dev, unsigned int* rate,
                      std::string* error) {
    if (in_dev.id == out_dev.id) {
        *rate = pick_rate(in_dev);
        return true;
    }
    std::vector<unsigned int> shared;
    for (unsigned int r : in_dev.rates) {
        if (std::find(out_dev.rates.begin(), out_dev.rates.end(), r) != out_dev.rates.end()) {
            shared.push_back(r);
        }
    }
    if (shared.empty()) {
        std::ostringstream why;
        why << "These two devices share no sample rate, so one stream cannot drive both.\n"
            << "  " << in_dev.name << " offers:" << rate_list(in_dev.rates) << "\n"
            << "  " << out_dev.name << " offers:" << rate_list(out_dev.rates) << "\n\n"
            << "This is not fixable by picking a different buffer size. Either use one\n"
               "device for both directions, or use an output device that offers a rate the\n"
               "input device also supports.";
        if (error) *error = why.str();
        return false;
    }
    *rate = std::find(shared.begin(), shared.end(), kPreferredRate) != shared.end()
                ? kPreferredRate
                : shared.front();
    return true;
}

}  // namespace audiodev
