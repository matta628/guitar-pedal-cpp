#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "Amp.h"
#include "BitCrusher.h"
#include "Chorus.h"
#include "Compressor.h"
#include "Delay.h"
#include "Distortion.h"
#include "EnvFilter.h"
#include "Effect.h"
#include "Flanger.h"
#include "Freeze.h"
#include "Fuzz.h"
#include "Phaser.h"
#include "PitchShifter.h"
#include "Reverb.h"
#include "RingMod.h"
#include "Tone.h"
#include "Tremolo.h"
#include "WaveFolder.h"

// Every effect this pedal can produce, and the presets that wire them into
// chains.
//
// The important structural decision: **one instance of each stage exists for
// the lifetime of the program**, and a preset is nothing but an ordered list of
// pointers into them plus a list of parameter values. Switching presets never
// constructs, destroys, resizes or reorders anything — it applies a fixed
// number of atomic stores and then publishes one integer. That is what makes it
// safe to drive from a footswitch interrupt and a browser at the same time
// while the audio thread is mid-buffer.
//
// The alternative — building a chain of `unique_ptr<Effect>` per preset and
// swapping the vector — would allocate on whichever thread switched, and worse,
// would free the old chain out from under an audio callback that might still be
// walking it. That is the bug this design exists to make impossible.
//
// One consequence worth knowing: because presets share stage instances, a
// preset must specify **every** parameter of every stage it uses. Anything left
// unspecified keeps whatever the previously selected preset set it to, which
// makes the sound depend on history. The preset table treats that as a rule,
// not a convenience.
class Pedalboard {
public:
    enum class StageId {
        Compressor = 0,
        Amp,
        Fuzz,
        Distortion,
        BitCrusher,
        WaveFolder,
        RingMod,
        EnvFilter,
        Freeze,
        Tone,
        Pitch,
        Phaser,
        Flanger,
        Chorus,
        Tremolo,
        Delay,
        Reverb,
        Count,
    };

    // The longest chain any preset needs (Radiohead's is seven), rounded up.
    static constexpr std::size_t kMaxChain = 10;

    // One tweakable value, with the accessors needed to read and write it.
    // The web UI renders these generically and never learns what a reverb is.
    struct Param {
        std::string id;      // "reverb.mix"
        std::string group;   // "Reverb" — matches a StageId's display name
        std::string label;   // "Mix"
        float min = 0.0f;
        float max = 1.0f;
        std::function<float()> get;
        std::function<void(float)> set;
    };

    // What a preset looks like in the table, before it is compiled.
    struct PresetSpec {
        const char* id;
        const char* name;
        const char* short_name;   // <= 10 chars, for the LCD's second line
        const char* blurb;        // what this is trying to sound like
        const char* gear;         // the hardware it is approximating
        std::vector<StageId> chain;
        std::vector<std::pair<const char*, float>> settings;
    };

    // The compiled form the audio thread walks.
    struct Preset {
        std::string id;
        std::string name;
        std::string short_name;
        std::string blurb;
        std::string gear;
        std::vector<std::string> groups;   // display names of the active stages
        std::array<Effect*, kMaxChain> chain{};
        std::size_t chain_length = 0;
        // Resolved at build time so selecting a preset is a tight loop with no
        // string comparisons in it.
        std::vector<std::pair<std::size_t, float>> settings;
        // The user's saved edits to this preset, applied after the defaults.
        // Kept separate rather than overwriting `settings` so "reset to
        // default" is a deletion, not a second table of remembered values.
        std::vector<std::pair<std::size_t, float>> overrides;
        bool has_override = false;
        // Free text the player writes while auditioning: "way too loud",
        // "great for verses". Deliberately NOT part of has_override -- a note
        // is an observation about a preset, not an edit to it, so writing one
        // must not make the UI claim the preset has been modified.
        std::string note;
    };

    explicit Pedalboard(float sample_rate);

    // ---- control threads (footswitch, web, main) ----
    // Applies the preset's parameter values, then publishes the index. The
    // audio thread may observe the new values one buffer before the new
    // routing; that is inaudible and it is the right way round, because the
    // reverse would briefly run a stage with another preset's settings.
    void select(int index);
    int cycle();
    int current() const { return current_.load(std::memory_order_relaxed); }

    // ---- user settings (control threads only: these touch the filesystem) ----
    // Where saved edits live. Set before load_user_presets().
    void set_storage_path(std::string path);
    // Reads saved edits, if any. Unknown preset ids and parameter names are
    // skipped with a warning rather than being fatal — a settings file written
    // by an older build must not stop the pedal from starting.
    void load_user_presets();

    // Captures the current value of every parameter this preset controls, and
    // rewrites the whole settings file.
    bool save_user_preset(int index, std::string* error = nullptr);
    // Drops the saved edits for one preset (or all of them) and reapplies the
    // table values if that preset is the one currently selected.
    bool reset_preset(int index, std::string* error = nullptr);
    bool reset_all(std::string* error = nullptr);
    bool has_override(int index) const;

    // Notes persist in the same file as saved edits. Setting one rewrites it.
    bool set_note(int index, std::string note, std::string* error = nullptr);
    std::string note(int index) const;

    const std::vector<Preset>& presets() const { return presets_; }
    const std::vector<Param>& params() const { return params_; }
    int preset_count() const { return static_cast<int>(presets_.size()); }

    // ---- audio thread ----
    void process(float* buffer, std::size_t n_frames);

    // Direct access for anything that needs a specific stage (the UI's
    // gain-reduction meter reads the compressor).
    const Compressor& compressor() const { return compressor_; }

private:
    void build_params();
    void build_presets();
    Effect* stage(StageId id);
    bool write_user_presets(std::string* error);

    Compressor compressor_;
    Amp amp_;
    Fuzz fuzz_;
    Distortion distortion_;
    BitCrusher bit_crusher_;
    WaveFolder wave_folder_;
    EnvFilter env_filter_;
    Freeze freeze_;
    RingMod ring_mod_;
    Tone tone_;
    PitchShifter pitch_;
    Phaser phaser_;
    Flanger flanger_;
    Chorus chorus_;
    Tremolo tremolo_;
    Delay delay_;
    Reverb reverb_;

    std::vector<Param> params_;
    std::vector<Preset> presets_;
    std::string storage_path_;
    std::atomic<int> current_{0};
};

// Display name for a stage, used to group parameters in the UI.
const char* stage_group_name(Pedalboard::StageId id);

// The preset table. Defined in Presets.cpp, which is data rather than
// mechanism and is meant to be read as the tone-research document's other half.
const std::vector<Pedalboard::PresetSpec>& preset_table();
