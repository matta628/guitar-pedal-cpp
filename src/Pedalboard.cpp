#include "Pedalboard.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

const char* stage_group_name(Pedalboard::StageId id) {
    switch (id) {
        case Pedalboard::StageId::Compressor: return "Compressor";
        case Pedalboard::StageId::Amp:        return "Amp";
        case Pedalboard::StageId::Fuzz:       return "Fuzz";
        case Pedalboard::StageId::Distortion: return "Drive";
        case Pedalboard::StageId::BitCrusher: return "Bit Crusher";
        case Pedalboard::StageId::RingMod:    return "Ring Mod";
        case Pedalboard::StageId::Tone:       return "Tone";
        case Pedalboard::StageId::Pitch:      return "Pitch";
        case Pedalboard::StageId::Phaser:     return "Phaser";
        case Pedalboard::StageId::Flanger:    return "Flanger";
        case Pedalboard::StageId::Chorus:     return "Chorus";
        case Pedalboard::StageId::Tremolo:    return "Tremolo";
        case Pedalboard::StageId::Delay:      return "Delay";
        case Pedalboard::StageId::Reverb:     return "Reverb";
        case Pedalboard::StageId::Count:      break;
    }
    return "?";
}

Pedalboard::Pedalboard(float sample_rate)
    : flanger_(sample_rate),
      pitch_(sample_rate),
      chorus_(sample_rate),
      delay_(sample_rate),
      reverb_(sample_rate) {
    compressor_.set_sample_rate(sample_rate);
    amp_.set_sample_rate(sample_rate);
    tone_.set_sample_rate(sample_rate);
    phaser_.set_sample_rate(sample_rate);
    tremolo_.set_sample_rate(sample_rate);
    ring_mod_.set_sample_rate(sample_rate);

    build_params();
    build_presets();
    // Apply preset 0 now, so the very first buffer runs with known values
    // rather than each stage's constructor defaults.
    select(0);
}

Effect* Pedalboard::stage(StageId id) {
    switch (id) {
        case StageId::Compressor: return &compressor_;
        case StageId::Amp:        return &amp_;
        case StageId::Fuzz:       return &fuzz_;
        case StageId::Distortion: return &distortion_;
        case StageId::BitCrusher: return &bit_crusher_;
        case StageId::RingMod:    return &ring_mod_;
        case StageId::Tone:       return &tone_;
        case StageId::Pitch:      return &pitch_;
        case StageId::Phaser:     return &phaser_;
        case StageId::Flanger:    return &flanger_;
        case StageId::Chorus:     return &chorus_;
        case StageId::Tremolo:    return &tremolo_;
        case StageId::Delay:      return &delay_;
        case StageId::Reverb:     return &reverb_;
        case StageId::Count:      break;
    }
    return nullptr;
}

void Pedalboard::build_params() {
    const auto add = [this](const char* id, StageId group, const char* label, float min, float max,
                            std::function<float()> get, std::function<void(float)> set) {
        params_.push_back({id, stage_group_name(group), label, min, max, std::move(get),
                           std::move(set)});
    };
    using S = StageId;

    add("comp.threshold", S::Compressor, "Threshold dB", -60.0f, 0.0f,
        [this] { return compressor_.threshold_db(); },
        [this](float v) { compressor_.set_threshold_db(v); });
    add("comp.ratio", S::Compressor, "Ratio", 1.0f, 20.0f,
        [this] { return compressor_.ratio(); }, [this](float v) { compressor_.set_ratio(v); });
    add("comp.attack", S::Compressor, "Attack ms", 0.1f, 200.0f,
        [this] { return compressor_.attack_ms(); }, [this](float v) { compressor_.set_attack_ms(v); });
    add("comp.release", S::Compressor, "Release ms", 5.0f, 2000.0f,
        [this] { return compressor_.release_ms(); },
        [this](float v) { compressor_.set_release_ms(v); });
    add("comp.makeup", S::Compressor, "Makeup dB", -12.0f, 24.0f,
        [this] { return compressor_.makeup_db(); }, [this](float v) { compressor_.set_makeup_db(v); });

    add("amp.gain", S::Amp, "Gain", 1.0f, 50.0f,
        [this] { return amp_.gain(); }, [this](float v) { amp_.set_gain(v); });
    add("amp.bass", S::Amp, "Bass", -12.0f, 12.0f,
        [this] { return amp_.bass_db(); }, [this](float v) { amp_.set_bass_db(v); });
    add("amp.mid", S::Amp, "Middle", -12.0f, 12.0f,
        [this] { return amp_.mid_db(); }, [this](float v) { amp_.set_mid_db(v); });
    add("amp.treble", S::Amp, "Treble", -12.0f, 12.0f,
        [this] { return amp_.treble_db(); }, [this](float v) { amp_.set_treble_db(v); });
    add("amp.presence", S::Amp, "Presence", -6.0f, 12.0f,
        [this] { return amp_.presence_db(); }, [this](float v) { amp_.set_presence_db(v); });
    add("amp.master", S::Amp, "Master", 1.0f, 20.0f,
        [this] { return amp_.master(); }, [this](float v) { amp_.set_master(v); });
    add("amp.volume", S::Amp, "Volume", 0.0f, 1.5f,
        [this] { return amp_.volume(); }, [this](float v) { amp_.set_volume(v); });
    add("amp.cab", S::Amp, "Cabinet", 0.0f, 1.0f,
        [this] { return amp_.cab(); }, [this](float v) { amp_.set_cab(v); });

    add("fuzz.drive", S::Fuzz, "Drive", 1.0f, 100.0f,
        [this] { return fuzz_.drive(); }, [this](float v) { fuzz_.set_drive(v); });
    add("fuzz.bias", S::Fuzz, "Bias", -0.5f, 0.5f,
        [this] { return fuzz_.bias(); }, [this](float v) { fuzz_.set_bias(v); });
    add("fuzz.gate", S::Fuzz, "Gate", 0.0f, 1.0f,
        [this] { return fuzz_.gate(); }, [this](float v) { fuzz_.set_gate(v); });
    add("fuzz.tone", S::Fuzz, "Tone", 0.0f, 1.0f,
        [this] { return fuzz_.tone(); }, [this](float v) { fuzz_.set_tone(v); });
    add("fuzz.level", S::Fuzz, "Level", 0.0f, 2.0f,
        [this] { return fuzz_.level(); }, [this](float v) { fuzz_.set_level(v); });
    add("fuzz.mix", S::Fuzz, "Mix", 0.0f, 1.0f,
        [this] { return fuzz_.mix(); }, [this](float v) { fuzz_.set_mix(v); });

    add("drive.drive", S::Distortion, "Drive", 1.0f, 20.0f,
        [this] { return distortion_.drive(); }, [this](float v) { distortion_.set_drive(v); });
    add("drive.mix", S::Distortion, "Mix", 0.0f, 1.0f,
        [this] { return distortion_.mix(); }, [this](float v) { distortion_.set_mix(v); });

    add("crush.bits", S::BitCrusher, "Bits", 1.0f, 16.0f,
        [this] { return bit_crusher_.bits(); }, [this](float v) { bit_crusher_.set_bits(v); });
    add("crush.rate", S::BitCrusher, "Downsample", 1.0f, 64.0f,
        [this] { return bit_crusher_.downsample(); },
        [this](float v) { bit_crusher_.set_downsample(v); });
    add("crush.mix", S::BitCrusher, "Mix", 0.0f, 1.0f,
        [this] { return bit_crusher_.mix(); }, [this](float v) { bit_crusher_.set_mix(v); });

    add("ring.freq", S::RingMod, "Carrier Hz", 1.0f, 4000.0f,
        [this] { return ring_mod_.frequency_hz(); },
        [this](float v) { ring_mod_.set_frequency_hz(v); });
    add("ring.mix", S::RingMod, "Mix", 0.0f, 1.0f,
        [this] { return ring_mod_.mix(); }, [this](float v) { ring_mod_.set_mix(v); });

    add("tone.bass", S::Tone, "Bass dB", -18.0f, 18.0f,
        [this] { return tone_.bass_db(); }, [this](float v) { tone_.set_bass_db(v); });
    add("tone.mid", S::Tone, "Mid dB", -18.0f, 18.0f,
        [this] { return tone_.mid_db(); }, [this](float v) { tone_.set_mid_db(v); });
    add("tone.midhz", S::Tone, "Mid Hz", 200.0f, 3000.0f,
        [this] { return tone_.mid_hz(); }, [this](float v) { tone_.set_mid_hz(v); });
    add("tone.midq", S::Tone, "Mid Q", 0.3f, 8.0f,
        [this] { return tone_.mid_q(); }, [this](float v) { tone_.set_mid_q(v); });
    add("tone.treble", S::Tone, "Treble dB", -18.0f, 18.0f,
        [this] { return tone_.treble_db(); }, [this](float v) { tone_.set_treble_db(v); });

    add("pitch.semitones", S::Pitch, "Semitones", -24.0f, 24.0f,
        [this] { return pitch_.semitones(); }, [this](float v) { pitch_.set_semitones(v); });
    add("pitch.cents", S::Pitch, "Detune cents", -50.0f, 50.0f,
        [this] { return pitch_.cents(); }, [this](float v) { pitch_.set_cents(v); });
    add("pitch.mix", S::Pitch, "Mix", 0.0f, 1.0f,
        [this] { return pitch_.mix(); }, [this](float v) { pitch_.set_mix(v); });

    add("phaser.rate", S::Phaser, "Rate Hz", 0.05f, 10.0f,
        [this] { return phaser_.rate_hz(); }, [this](float v) { phaser_.set_rate_hz(v); });
    add("phaser.depth", S::Phaser, "Depth", 0.0f, 1.0f,
        [this] { return phaser_.depth(); }, [this](float v) { phaser_.set_depth(v); });
    add("phaser.feedback", S::Phaser, "Feedback", 0.0f, 0.9f,
        [this] { return phaser_.feedback(); }, [this](float v) { phaser_.set_feedback(v); });
    add("phaser.stages", S::Phaser, "Stages", 2.0f, 8.0f,
        [this] { return phaser_.stages(); },
        [this](float v) { phaser_.set_stages(static_cast<int>(v)); });
    add("phaser.mix", S::Phaser, "Mix", 0.0f, 1.0f,
        [this] { return phaser_.mix(); }, [this](float v) { phaser_.set_mix(v); });

    add("flanger.rate", S::Flanger, "Rate Hz", 0.02f, 10.0f,
        [this] { return flanger_.rate_hz(); }, [this](float v) { flanger_.set_rate_hz(v); });
    add("flanger.depth", S::Flanger, "Depth ms", 0.0f, 8.0f,
        [this] { return flanger_.depth_ms(); }, [this](float v) { flanger_.set_depth_ms(v); });
    add("flanger.delay", S::Flanger, "Centre ms", 0.5f, 10.0f,
        [this] { return flanger_.delay_ms(); }, [this](float v) { flanger_.set_delay_ms(v); });
    add("flanger.feedback", S::Flanger, "Feedback", -0.95f, 0.95f,
        [this] { return flanger_.feedback(); }, [this](float v) { flanger_.set_feedback(v); });
    add("flanger.mix", S::Flanger, "Mix", 0.0f, 1.0f,
        [this] { return flanger_.mix(); }, [this](float v) { flanger_.set_mix(v); });

    add("chorus.rate", S::Chorus, "Rate Hz", 0.05f, 10.0f,
        [this] { return chorus_.rate_hz(); }, [this](float v) { chorus_.set_rate_hz(v); });
    add("chorus.depth", S::Chorus, "Depth ms", 0.0f, 10.0f,
        [this] { return chorus_.depth_ms(); }, [this](float v) { chorus_.set_depth_ms(v); });
    add("chorus.mix", S::Chorus, "Mix", 0.0f, 1.0f,
        [this] { return chorus_.mix(); }, [this](float v) { chorus_.set_mix(v); });

    add("trem.rate", S::Tremolo, "Rate Hz", 0.1f, 20.0f,
        [this] { return tremolo_.rate_hz(); }, [this](float v) { tremolo_.set_rate_hz(v); });
    add("trem.depth", S::Tremolo, "Depth", 0.0f, 1.0f,
        [this] { return tremolo_.depth(); }, [this](float v) { tremolo_.set_depth(v); });
    add("trem.shape", S::Tremolo, "Shape", 0.0f, 1.0f,
        [this] { return tremolo_.shape(); }, [this](float v) { tremolo_.set_shape(v); });

    add("delay.time", S::Delay, "Time s", 0.01f, 2.0f,
        [this] { return delay_.delay_seconds(); },
        [this](float v) { delay_.set_delay_seconds(v); });
    add("delay.feedback", S::Delay, "Feedback", 0.0f, 0.95f,
        [this] { return delay_.feedback(); }, [this](float v) { delay_.set_feedback(v); });
    add("delay.tone", S::Delay, "Repeat tone", 0.0f, 1.0f,
        [this] { return delay_.tone(); }, [this](float v) { delay_.set_tone(v); });
    add("delay.mod", S::Delay, "Wow ms", 0.0f, 20.0f,
        [this] { return delay_.modulation_ms(); },
        [this](float v) { delay_.set_modulation_ms(v); });
    add("delay.mix", S::Delay, "Mix", 0.0f, 1.0f,
        [this] { return delay_.mix(); }, [this](float v) { delay_.set_mix(v); });

    add("reverb.mode", S::Reverb, "Mode", 0.0f, 3.0f,
        [this] { return reverb_.mode_index(); },
        [this](float v) {
            reverb_.set_mode(static_cast<Reverb::Mode>(std::clamp(static_cast<int>(v + 0.5f), 0, 3)));
        });
    add("reverb.size", S::Reverb, "Size", 0.0f, 1.0f,
        [this] { return reverb_.room_size(); }, [this](float v) { reverb_.set_room_size(v); });
    add("reverb.damping", S::Reverb, "Damping", 0.0f, 1.0f,
        [this] { return reverb_.damping(); }, [this](float v) { reverb_.set_damping(v); });
    add("reverb.mix", S::Reverb, "Mix", 0.0f, 1.0f,
        [this] { return reverb_.mix(); }, [this](float v) { reverb_.set_mix(v); });
}

void Pedalboard::build_presets() {
    for (const PresetSpec& spec : preset_table()) {
        Preset preset;
        preset.id = spec.id;
        preset.name = spec.name;
        preset.short_name = spec.short_name;
        preset.blurb = spec.blurb;
        preset.gear = spec.gear;

        if (spec.chain.size() > kMaxChain) {
            throw std::runtime_error(std::string("preset '") + spec.id + "' exceeds kMaxChain");
        }
        for (StageId id : spec.chain) {
            preset.chain[preset.chain_length++] = stage(id);
            preset.groups.push_back(stage_group_name(id));
        }

        // Resolve every setting to a parameter index now. A typo in the table
        // is a startup failure with a name in it, not a silently ignored knob
        // discovered by ear three presets later.
        for (const auto& [id, value] : spec.settings) {
            const auto it = std::find_if(params_.begin(), params_.end(),
                                         [&](const Param& p) { return p.id == id; });
            if (it == params_.end()) {
                throw std::runtime_error(std::string("preset '") + spec.id +
                                         "' references unknown parameter '" + id + "'");
            }
            preset.settings.emplace_back(
                static_cast<std::size_t>(std::distance(params_.begin(), it)), value);
        }

        presets_.push_back(std::move(preset));
    }
}

void Pedalboard::select(int index) {
    if (index < 0 || index >= static_cast<int>(presets_.size())) return;

    const Preset& preset = presets_[static_cast<std::size_t>(index)];
    // Defaults first, then the user's edits on top. Applying them in this
    // order is what makes an override a diff against the table rather than a
    // replacement for it — a preset gaining a new parameter in a later build
    // still gets a sensible value for it.
    for (const auto& [param_index, value] : preset.settings) {
        params_[param_index].set(value);
    }
    for (const auto& [param_index, value] : preset.overrides) {
        params_[param_index].set(value);
    }
    current_.store(index, std::memory_order_relaxed);
}

int Pedalboard::cycle() {
    const int next = (current_.load(std::memory_order_relaxed) + 1) %
                     static_cast<int>(presets_.size());
    select(next);
    return next;
}

void Pedalboard::process(float* buffer, std::size_t n_frames) {
    // One relaxed load, then a walk over a fixed array of pointers. Nothing
    // here can allocate, block or fail.
    const Preset& preset = presets_[static_cast<std::size_t>(
        current_.load(std::memory_order_relaxed))];
    for (std::size_t i = 0; i < preset.chain_length; ++i) {
        preset.chain[i]->process(buffer, n_frames);
    }
}

// ---------------------------------------------------------------- user presets
//
// The file format is deliberately plain text rather than JSON: it needs no
// parser beyond a stream extraction, it diffs readably in git, and it can be
// fixed in a text editor over SSH when a preset has been saved unlistenable.
//
//   # comment
//   [preset-id]
//   param.id  value
//
// All of this runs on whichever control thread asked for it — the web server's,
// normally. It opens files, allocates and can block on disk, none of which may
// ever happen on the audio thread. The audio thread's only involvement is that
// select() eventually stores one integer.

void Pedalboard::set_storage_path(std::string path) { storage_path_ = std::move(path); }

bool Pedalboard::has_override(int index) const {
    if (index < 0 || index >= static_cast<int>(presets_.size())) return false;
    return presets_[static_cast<std::size_t>(index)].has_override;
}

void Pedalboard::load_user_presets() {
    if (storage_path_.empty()) return;
    std::ifstream in(storage_path_);
    if (!in) return;  // nothing saved yet is the normal case, not an error

    Preset* target = nullptr;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos) continue;
        line = line.substr(begin);
        if (line[0] == '#') continue;

        if (line[0] == '[') {
            const std::size_t close = line.find(']');
            if (close == std::string::npos) continue;
            const std::string id = line.substr(1, close - 1);
            const auto it = std::find_if(presets_.begin(), presets_.end(),
                                         [&](const Preset& p) { return p.id == id; });
            // A preset that no longer exists: skip its whole block rather than
            // refusing to start.
            target = (it == presets_.end()) ? nullptr : &*it;
            if (target != nullptr) {
                target->overrides.clear();
                // Provisional: a block containing only a note carries no edits,
                // so this is corrected once the block has been read.
                target->has_override = true;
            }
            continue;
        }

        if (target == nullptr) continue;

        // Before the numeric parse: a note's text is not a float, so the
        // `fields >> key >> value` below would silently discard it.
        if (line.rfind("note ", 0) == 0) {
            const std::string raw = line.substr(5);
            std::string text;
            text.reserve(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == 'n') {
                    text += '\n';
                    ++i;
                } else {
                    text += raw[i];
                }
            }
            target->note = text;
            continue;
        }

        std::istringstream fields(line);
        std::string key;
        float value = 0.0f;
        if (!(fields >> key >> value)) continue;

        const auto it = std::find_if(params_.begin(), params_.end(),
                                     [&](const Param& p) { return p.id == key; });
        if (it == params_.end()) continue;  // parameter renamed or removed
        target->overrides.emplace_back(
            static_cast<std::size_t>(std::distance(params_.begin(), it)), value);
    }

    // A preset whose block turned out to be empty is not overridden.
    for (Preset& preset : presets_) {
        if (preset.overrides.empty()) preset.has_override = false;
    }

    // Whatever is selected now was applied before the file was read.
    select(current_.load(std::memory_order_relaxed));
}

bool Pedalboard::write_user_presets(std::string* error) {
    if (storage_path_.empty()) {
        if (error) *error = "no storage path configured";
        return false;
    }

    // Write to a temporary file and rename over the target. A crash or a full
    // disk halfway through then leaves the previous settings intact rather
    // than a truncated file that loses every preset at once.
    const std::string temp = storage_path_ + ".tmp";
    {
        std::ofstream out(temp, std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + temp;
            return false;
        }
        out << "# guitar-pedal-cpp saved settings. Delete a block to restore that\n"
               "# preset's built-in values, or delete the file to reset everything.\n";
        for (const Preset& preset : presets_) {
            const bool has_values = preset.has_override && !preset.overrides.empty();
            if (!has_values && preset.note.empty()) continue;
            out << "\n[" << preset.id << "]\n";
            if (!preset.note.empty()) {
                // One line, so the reader stays line-oriented. Real newlines
                // are escaped rather than wrapped.
                std::string flat = preset.note;
                std::string escaped;
                escaped.reserve(flat.size());
                for (char ch : flat) {
                    if (ch == '\n') escaped += "\\n";
                    else if (ch == '\r') continue;
                    else escaped += ch;
                }
                out << "note " << escaped << '\n';
            }
            for (const auto& [param_index, value] : preset.overrides) {
                out << params_[param_index].id << ' ' << value << '\n';
            }
        }
        if (!out) {
            if (error) *error = "write failed";
            return false;
        }
    }

    std::error_code ec;
    std::filesystem::rename(temp, storage_path_, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

bool Pedalboard::save_user_preset(int index, std::string* error) {
    if (index < 0 || index >= static_cast<int>(presets_.size())) {
        if (error) *error = "no such preset";
        return false;
    }

    Preset& preset = presets_[static_cast<std::size_t>(index)];
    preset.overrides.clear();
    // Snapshot exactly the parameters this preset controls. Saving every
    // parameter in the program would bake in the state of stages this preset
    // does not even run, and those values would then leak into it later.
    for (const auto& [param_index, _] : preset.settings) {
        preset.overrides.emplace_back(param_index, params_[param_index].get());
    }
    preset.has_override = true;

    return write_user_presets(error);
}

bool Pedalboard::reset_preset(int index, std::string* error) {
    if (index < 0 || index >= static_cast<int>(presets_.size())) {
        if (error) *error = "no such preset";
        return false;
    }

    Preset& preset = presets_[static_cast<std::size_t>(index)];
    preset.overrides.clear();
    preset.has_override = false;

    // Make it audible immediately if this is the preset being played.
    if (current_.load(std::memory_order_relaxed) == index) select(index);

    return write_user_presets(error);
}

bool Pedalboard::reset_all(std::string* error) {
    for (Preset& preset : presets_) {
        preset.overrides.clear();
        preset.has_override = false;
    }
    select(current_.load(std::memory_order_relaxed));
    return write_user_presets(error);
}

bool Pedalboard::set_note(int index, std::string note, std::string* error) {
    if (index < 0 || index >= static_cast<int>(presets_.size())) {
        if (error) *error = "no such preset";
        return false;
    }
    presets_[static_cast<std::size_t>(index)].note = std::move(note);
    return write_user_presets(error);
}

std::string Pedalboard::note(int index) const {
    if (index < 0 || index >= static_cast<int>(presets_.size())) return {};
    return presets_[static_cast<std::size_t>(index)].note;
}
