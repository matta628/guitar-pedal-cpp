#include "LoopStore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

constexpr char kExt[] = ".wav";

void put_u32(std::ostream& os, std::uint32_t v) {
    const unsigned char b[4] = {static_cast<unsigned char>(v & 0xFF),
                                static_cast<unsigned char>((v >> 8) & 0xFF),
                                static_cast<unsigned char>((v >> 16) & 0xFF),
                                static_cast<unsigned char>((v >> 24) & 0xFF)};
    os.write(reinterpret_cast<const char*>(b), 4);
}

void put_u16(std::ostream& os, std::uint16_t v) {
    const unsigned char b[2] = {static_cast<unsigned char>(v & 0xFF),
                                static_cast<unsigned char>((v >> 8) & 0xFF)};
    os.write(reinterpret_cast<const char*>(b), 2);
}

std::uint32_t get_u32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t get_u16(const unsigned char* p) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[0]) |
                                      (static_cast<std::uint16_t>(p[1]) << 8));
}

}  // namespace

LoopStore::LoopStore(std::string dir) : dir_(std::move(dir)) {}

std::string LoopStore::sanitise(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-';
        if (ok) out.push_back(c);
    }
    // Trim surrounding spaces so " " does not become a file called " ".
    const auto first = out.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = out.find_last_not_of(' ');
    out = out.substr(first, last - first + 1);
    if (out.size() > 64) out.resize(64);
    return out;
}

std::string LoopStore::path_for(const std::string& safe_name) const {
    return dir_ + "/" + safe_name + kExt;
}

std::vector<LoopStore::Entry> LoopStore::list() const {
    std::vector<Entry> out;
    std::error_code ec;
    if (!fs::exists(dir_, ec)) return out;

    std::vector<std::pair<fs::file_time_type, Entry>> dated;
    for (const auto& de : fs::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (!de.is_regular_file(ec)) continue;
        const fs::path p = de.path();
        if (p.extension() != kExt) continue;

        Entry e;
        e.name = p.stem().string();

        // Read only the header; the samples are not needed to list a loop.
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        unsigned char hdr[44];
        in.read(reinterpret_cast<char*>(hdr), sizeof hdr);
        if (in.gcount() != static_cast<std::streamsize>(sizeof hdr)) continue;
        if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) continue;

        e.rate = get_u32(hdr + 24);
        const std::uint16_t bits = get_u16(hdr + 34);
        const std::uint32_t data_bytes = get_u32(hdr + 40);
        const std::uint32_t bytes_per_frame = bits / 8u;
        e.frames = bytes_per_frame ? data_bytes / bytes_per_frame : 0;
        e.seconds = e.rate ? static_cast<float>(e.frames) / static_cast<float>(e.rate) : 0.0f;

        dated.emplace_back(de.last_write_time(ec), std::move(e));
    }

    std::sort(dated.begin(), dated.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    out.reserve(dated.size());
    for (auto& d : dated) out.push_back(std::move(d.second));
    return out;
}

bool LoopStore::save(const std::string& name, const std::vector<float>& samples, unsigned int rate,
                     std::string* error) {
    const std::string safe = sanitise(name);
    if (safe.empty()) {
        if (error) *error = "that name has no usable characters in it";
        return false;
    }
    if (samples.empty()) {
        if (error) *error = "there is no loop to save";
        return false;
    }

    std::error_code ec;
    fs::create_directories(dir_, ec);
    if (ec) {
        if (error) *error = "could not create " + dir_ + ": " + ec.message();
        return false;
    }

    // Write to a temporary and rename, so a crash mid-write cannot leave a
    // half-file that lists as a valid loop and loads as garbage.
    const std::string final_path = path_for(safe);
    const std::string tmp_path = final_path + ".part";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "could not open " + tmp_path + " for writing";
            return false;
        }
        const std::uint32_t data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
        out.write("RIFF", 4);
        put_u32(out, 36 + data_bytes);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        put_u32(out, 16);                                   // PCM fmt chunk size
        put_u16(out, 1);                                    // PCM
        put_u16(out, 1);                                    // mono
        put_u32(out, rate);
        put_u32(out, rate * 2);                             // byte rate
        put_u16(out, 2);                                    // block align
        put_u16(out, 16);                                   // bits
        out.write("data", 4);
        put_u32(out, data_bytes);

        for (const float s : samples) {
            // Clamp before scaling: a loop recorded hot can sit above 1.0 after
            // overdubbing, and wrapping that would turn a loud loop into noise.
            const float c = std::clamp(s, -1.0f, 1.0f);
            const auto v = static_cast<std::int16_t>(std::lround(c * 32767.0f));
            put_u16(out, static_cast<std::uint16_t>(v));
        }
        if (!out) {
            if (error) *error = "write failed (disk full?)";
            return false;
        }
    }

    fs::rename(tmp_path, final_path, ec);
    if (ec) {
        if (error) *error = "could not finish writing: " + ec.message();
        fs::remove(tmp_path, ec);
        return false;
    }
    return true;
}

bool LoopStore::load(const std::string& name, std::vector<float>* out, unsigned int* rate,
                     std::string* error) {
    const std::string safe = sanitise(name);
    if (safe.empty()) {
        if (error) *error = "no such loop";
        return false;
    }
    std::ifstream in(path_for(safe), std::ios::binary);
    if (!in) {
        if (error) *error = "no loop called '" + safe + "'";
        return false;
    }
    unsigned char hdr[44];
    in.read(reinterpret_cast<char*>(hdr), sizeof hdr);
    if (in.gcount() != static_cast<std::streamsize>(sizeof hdr) ||
        std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        if (error) *error = "'" + safe + "' is not a WAV file";
        return false;
    }
    const std::uint16_t channels = get_u16(hdr + 22);
    const std::uint16_t bits = get_u16(hdr + 34);
    if (channels != 1 || bits != 16) {
        if (error) *error = "only mono 16-bit WAV is supported";
        return false;
    }
    if (rate) *rate = get_u32(hdr + 24);

    const std::uint32_t data_bytes = get_u32(hdr + 40);
    out->clear();
    out->reserve(data_bytes / 2);
    std::vector<unsigned char> raw(data_bytes);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(data_bytes));
    const auto got = static_cast<std::size_t>(in.gcount());
    for (std::size_t i = 0; i + 1 < got; i += 2) {
        const auto v = static_cast<std::int16_t>(get_u16(raw.data() + i));
        out->push_back(static_cast<float>(v) / 32768.0f);
    }
    return true;
}

bool LoopStore::remove(const std::string& name, std::string* error) {
    const std::string safe = sanitise(name);
    if (safe.empty()) {
        if (error) *error = "no such loop";
        return false;
    }
    std::error_code ec;
    if (!fs::remove(path_for(safe), ec) || ec) {
        if (error) *error = "could not delete '" + safe + "'";
        return false;
    }
    return true;
}
