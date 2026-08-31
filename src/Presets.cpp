// The preset table.
//
// This file is data, not mechanism — Pedalboard.cpp is the machinery that
// compiles and runs what is written here. It is meant to be read alongside
// docs/TONE_RESEARCH.md, which cites where each rig description came from.
//
// Two honesty notes, because the band names invite a claim this code does not
// make. First, none of this is circuit modelling: there is no component-level
// simulation of a Tone Bender's germanium transistors or a spring tank's
// dispersion. Each preset is an arrangement of generic stages tuned by ear
// towards a described sound. Second, a guitar tone is a guitar, a player, a
// room and a record producer as much as it is a pedal, and only the pedal part
// is reproducible here. "Approximation" is meant literally.
//
// The rule the table has to obey: because every preset shares one instance of
// each stage, a preset must set **every** parameter of every stage it uses.
// The per-stage helpers below exist to make that structurally hard to get
// wrong — you cannot call fuzz() without supplying all six of its values.

#include "Pedalboard.h"

namespace {

using S = Pedalboard::StageId;
using Setting = std::pair<const char*, float>;
using Settings = std::vector<Setting>;

// Reverb modes, matching Reverb::Mode.
constexpr float kRoom = 0.0f;
constexpr float kPlate = 1.0f;
constexpr float kHall = 2.0f;
constexpr float kSpring = 3.0f;

Settings compressor(float threshold, float ratio, float attack, float release, float makeup) {
    return {{"comp.threshold", threshold}, {"comp.ratio", ratio},   {"comp.attack", attack},
            {"comp.release", release},     {"comp.makeup", makeup}};
}

Settings amp(float gain, float bass, float mid, float treble, float presence, float master,
             float volume, float cab) {
    return {{"amp.gain", gain},         {"amp.bass", bass},     {"amp.mid", mid},
            {"amp.treble", treble},     {"amp.presence", presence}, {"amp.master", master},
            {"amp.volume", volume},     {"amp.cab", cab}};
}

Settings fuzz(float drive, float bias, float gate, float tone, float level, float mix) {
    return {{"fuzz.drive", drive}, {"fuzz.bias", bias},   {"fuzz.gate", gate},
            {"fuzz.tone", tone},   {"fuzz.level", level}, {"fuzz.mix", mix}};
}

Settings drive(float amount, float mix) {
    return {{"drive.drive", amount}, {"drive.mix", mix}};
}

Settings tone(float bass, float mid, float mid_hz, float mid_q, float treble) {
    return {{"tone.bass", bass},   {"tone.mid", mid},     {"tone.midhz", mid_hz},
            {"tone.midq", mid_q},  {"tone.treble", treble}};
}

Settings crush(float bits, float rate, float mix) {
    return {{"crush.bits", bits}, {"crush.rate", rate}, {"crush.mix", mix}};
}

Settings ring(float hz, float mix) { return {{"ring.freq", hz}, {"ring.mix", mix}}; }

Settings fold(float drive, float symmetry, float level, float mix) {
    return {{"fold.drive", drive},
            {"fold.symmetry", symmetry},
            {"fold.level", level},
            {"fold.mix", mix}};
}

Settings env(float base, float range, float sens, float q, float attack, float release,
             float mix) {
    return {{"env.base", base},     {"env.range", range},     {"env.sens", sens},
            {"env.q", q},           {"env.attack", attack},   {"env.release", release},
            {"env.mix", mix}};
}

Settings pitch(float semitones, float cents, float mix) {
    return {{"pitch.semitones", semitones}, {"pitch.cents", cents}, {"pitch.mix", mix}};
}

Settings phaser(float rate, float depth, float feedback, float stages, float mix) {
    return {{"phaser.rate", rate},         {"phaser.depth", depth}, {"phaser.feedback", feedback},
            {"phaser.stages", stages},     {"phaser.mix", mix}};
}

Settings flanger(float rate, float depth, float centre, float feedback, float mix) {
    return {{"flanger.rate", rate},         {"flanger.depth", depth}, {"flanger.delay", centre},
            {"flanger.feedback", feedback}, {"flanger.mix", mix}};
}

Settings chorus(float rate, float depth, float mix) {
    return {{"chorus.rate", rate}, {"chorus.depth", depth}, {"chorus.mix", mix}};
}

Settings tremolo(float rate, float depth, float shape) {
    return {{"trem.rate", rate}, {"trem.depth", depth}, {"trem.shape", shape}};
}

Settings delay(float time, float feedback, float repeat_tone, float wow, float mix) {
    return {{"delay.time", time},  {"delay.feedback", feedback}, {"delay.tone", repeat_tone},
            {"delay.mod", wow},    {"delay.mix", mix}};
}

Settings reverb(float mode, float size, float damping, float mix) {
    return {{"reverb.mode", mode}, {"reverb.size", size}, {"reverb.damping", damping},
            {"reverb.mix", mix}};
}

template <typename... Rest>
Settings merge(Settings first, Rest... rest) {
    for (const Settings& next : {rest...}) {
        first.insert(first.end(), next.begin(), next.end());
    }
    return first;
}

const std::vector<Pedalboard::PresetSpec>& table() {
    static const std::vector<Pedalboard::PresetSpec> presets = {

    // ================================================================ default

    {"clean", "Clean", "CLEAN",
     "Nothing in the path. The looper still records and plays back.",
     "—",
     {},
     {}},

    {"amp", "Custom Amp", "AMP",
     "A bare amplifier to dial in by hand: gain, a tone stack between the two "
     "gain stages, master, presence, and a speaker. Save it when you like it.",
     "Generic combo · spring tank",
     {S::Amp, S::Reverb},
     merge(amp(/*gain*/ 6.0f, /*bass*/ 2.0f, /*mid*/ 0.0f, /*treble*/ 2.0f, /*presence*/ 2.0f,
               /*master*/ 3.0f, /*volume*/ 0.7f, /*cab*/ 0.65f),
           reverb(kSpring, 0.45f, 0.5f, 0.18f))},

    // ============================================================= band tones

    // Slowdive. The whole band ran through one preset: the Yamaha FX500's
    // "Soft Focus" program, a symphonic chorus feeding a large reverb, with
    // ordinary distortion pedals in front. Reverb.com's reunion interview has
    // Halstead describing tracking down an FX500 specifically because it was
    // what they used the first time.
    {"slowdive", "Slowdive · Soft Focus", "SLOWDIVE",
     "The Yamaha FX500 'Soft Focus' patch: deep symphonic chorus into an "
     "enormous, barely damped hall, light grit in front, long modulated repeats "
     "underneath.",
     "Yamaha FX500 (Soft Focus) · Boss CE-2 · Marshall Shredmaster · Roland RE-201",
     {S::Distortion, S::Chorus, S::Delay, S::Reverb},
     merge(drive(2.2f, 0.35f),
           chorus(0.35f, 6.0f, 0.75f),
           delay(0.42f, 0.42f, 0.55f, 3.0f, 0.30f),
           reverb(kHall, 0.95f, 0.25f, 0.62f))},

    // Elliott Smith. Almost no pedals: the sound is doubled takes, a light
    // valve overdrive, and heavy compression to tape. The chorus here is not a
    // chorus pedal — it stands in for the natural detune of two takes.
    {"elliott", "Elliott Smith · Double-Tracked", "ELLIOTT",
     "Compressed clean with a whisper of valve overdrive, and just enough "
     "chorus to imitate two takes played a fraction apart. Slap delay, small "
     "room, nothing showy.",
     "Budda Phatman · Line 6 DL-4 · doubled / high-strung parts · compression to tape",
     {S::Compressor, S::Distortion, S::Chorus, S::Delay, S::Reverb},
     merge(compressor(-24.0f, 4.0f, 12.0f, 180.0f, 6.0f),
           drive(1.6f, 0.22f),
           chorus(0.9f, 1.6f, 0.30f),
           delay(0.11f, 0.18f, 0.70f, 0.6f, 0.18f),
           reverb(kRoom, 0.35f, 0.60f, 0.16f))},

    // The Strokes. Two Fender Hot Rod DeVilles doing the raunch, a RAT / DS-1 /
    // Jekyll & Hyde in front and an MXR Micro Amp for lifts. Mid-forward and
    // audibly limited before it ever reaches the distortion — part of why the
    // rhythm parts never move in level.
    {"strokes", "The Strokes · Transporterraum", "STROKES",
     "Thin, mid-forward, hard-limited crunch with almost no ambience — the "
     "deliberately small, radio-like tone of Is This It.",
     "ProCo RAT · Boss DS-1 · Visual Sound Jekyll & Hyde · MXR Micro Amp · Fender Hot Rod DeVille",
     {S::Compressor, S::Distortion, S::Amp, S::Reverb},
     merge(compressor(-20.0f, 6.0f, 6.0f, 90.0f, 5.0f),
           drive(7.0f, 0.92f),
           amp(5.0f, -6.0f, 6.0f, 4.0f, 4.0f, 2.5f, 0.75f, 0.55f),
           reverb(kRoom, 0.22f, 0.60f, 0.08f))},

    // Arctic Monkeys. Turner's Coopersonic Valveslapper and Cook's Demeter
    // Fuzzulator are the Humbug-through-AM fuzz voice; the Fulltone Mini
    // DejaVibe is the wobble on top of it.
    {"arctic", "Arctic Monkeys · Valveslapper", "ARCTIC",
     "Warm, creamy valve fuzz through a big old head, with a slow DejaVibe "
     "phase over it. Not a buzzsaw — the note survives.",
     "Coopersonic Valveslapper · Demeter FUZ-1 Fuzzulator · Fulltone Mini DejaVibe · Simms Watts",
     {S::Fuzz, S::Amp, S::Phaser, S::Delay, S::Reverb},
     merge(fuzz(16.0f, 0.12f, 0.0f, 0.45f, 0.55f, 0.90f),
           amp(3.5f, 3.0f, 2.0f, 1.0f, 1.0f, 4.0f, 0.72f, 0.70f),
           phaser(0.55f, 0.80f, 0.45f, 4.0f, 0.42f),
           delay(0.28f, 0.25f, 0.60f, 1.2f, 0.18f),
           reverb(kRoom, 0.45f, 0.50f, 0.20f))},

    // The Voidz. Guitars that sound like they have been through a failing
    // sampler: gated fuzz, low bit depth, a little ring modulation, and an
    // octave down grinding underneath.
    {"voidz", "The Voidz · Tyranny", "VOIDZ",
     "Deliberately broken: gated fuzz into five-and-a-half bits, decimated, "
     "ring-modulated, with a sub-octave grinding under it.",
     "Heavily processed / bit-reduced guitar chains · Boss DB-5 Driver",
     {S::Fuzz, S::BitCrusher, S::RingMod, S::Pitch, S::Delay, S::Reverb},
     merge(fuzz(55.0f, 0.30f, 0.35f, 0.70f, 0.45f, 1.0f),
           crush(5.5f, 6.0f, 0.60f),
           ring(140.0f, 0.22f),
           pitch(-12.0f, 0.0f, 0.25f),
           delay(0.19f, 0.40f, 0.50f, 4.0f, 0.24f),
           reverb(kPlate, 0.50f, 0.35f, 0.22f))},

    // Wednesday. Karly Hartzman plays a RAT into a Hot Rod DeVille and little
    // else; Xandy Chelmis stacks Rats, Muffs and Swollen Pickles on the lap
    // steel, and MJ Lenderman parks a Cry Baby as a fixed filter rather than
    // sweeping it. The parked wah is the Tone stage's narrow mid peak.
    {"wednesday", "Wednesday · Bull Believer", "WEDNSDAY",
     "A RAT with a Muff stacked on top, through a parked wah, into a big "
     "room — the blown-out alt-country wall from Rat Saw God.",
     "ProCo RAT · Death By Audio Interstellar Overdrive · Big Muff / Swollen Pickle · parked Cry Baby",
     {S::Distortion, S::Fuzz, S::Tone, S::Amp, S::Delay, S::Reverb},
     merge(drive(6.5f, 0.85f),
           fuzz(30.0f, 0.18f, 0.0f, 0.50f, 0.50f, 0.55f),
           tone(-3.0f, 9.0f, 850.0f, 3.2f, 2.0f),
           amp(3.0f, 2.0f, 1.0f, 3.0f, 3.0f, 3.0f, 0.70f, 0.62f),
           delay(0.33f, 0.30f, 0.50f, 1.5f, 0.20f),
           reverb(kHall, 0.85f, 0.35f, 0.42f))},

    // Led Zeppelin. Sola Sound Tone Bender MkII for the saturated leads, a
    // Vox V846 parked toe-down as a treble booster rather than swept, and an
    // Echoplex EP-3 used both for its tape echo and for its preamp.
    {"zeppelin", "Led Zeppelin · Tone Bender", "ZEPPELIN",
     "Asymmetric germanium fuzz, a wah parked toe-down as a treble booster, "
     "and dark Echoplex repeats that wow noticeably.",
     "Sola Sound Tone Bender MkII · Vox V846 (parked) · Maestro Echoplex EP-3",
     {S::Fuzz, S::Tone, S::Amp, S::Delay, S::Reverb},
     merge(fuzz(24.0f, 0.28f, 0.0f, 0.42f, 0.60f, 0.95f),
           tone(-5.0f, 4.0f, 1800.0f, 1.6f, 6.0f),
           amp(4.0f, 1.0f, 2.0f, 3.0f, 5.0f, 5.0f, 0.70f, 0.60f),
           delay(0.31f, 0.32f, 0.35f, 5.0f, 0.24f),
           reverb(kRoom, 0.50f, 0.45f, 0.16f))},

    // Deftones. Down-tuned, mid-scooped rhythm from Bogner Uberschalls with an
    // MXR 6-band EQ, and the ambient half from an Eventide H9 — a pitched
    // layer sitting inside a long reverb.
    {"deftones", "Deftones · Around the Fur", "DEFTONES",
     "Scooped high-gain rhythm with an octave-up shimmer folded into a long "
     "hall — heavy and atmospheric at the same time, which is the whole trick.",
     "Bogner Uberschall · MXR M109 6-band EQ · Eventide H9 · Boss DD",
     {S::Distortion, S::Tone, S::Amp, S::Pitch, S::Delay, S::Reverb},
     merge(drive(12.0f, 1.0f),
           tone(6.0f, -9.0f, 700.0f, 1.0f, 4.0f),
           amp(8.0f, 4.0f, -4.0f, 4.0f, 6.0f, 4.0f, 0.65f, 0.75f),
           pitch(12.0f, 0.0f, 0.16f),
           delay(0.50f, 0.45f, 0.60f, 0.8f, 0.26f),
           reverb(kHall, 0.90f, 0.40f, 0.40f))},

    // The Velvet Underground. Barely any pedals — a Vox AC100 fitted with a
    // mid-range booster, and a Vox distortion booster and tremolo unit built
    // into Lou Reed's Gretsch. The tremolo is the identifiable effect.
    {"velvet", "Velvet Underground · Ostrich", "VELVET",
     "Raw mid-boosted amp with a hard, choppy 1960s tremolo over it and a "
     "short spring tank behind. Almost no pedals, on purpose.",
     "Vox AC100 + mid booster · Vox distortion booster · on-board tremolo · Vox V828 Tone Bender",
     {S::Distortion, S::Amp, S::Tremolo, S::Reverb},
     merge(drive(5.5f, 0.80f),
           amp(7.0f, -4.0f, 7.0f, 1.0f, 2.0f, 3.0f, 0.72f, 0.58f),
           tremolo(6.5f, 0.70f, 0.55f),
           reverb(kSpring, 0.40f, 0.50f, 0.22f))},

    // Cocteau Twins. Robin Guthrie's own description is banks of chorus and
    // digital reverb "until the guitar stopped sounding like a guitar":
    // Eventide SP2016, Lexicon 480L pitch-shifted +/-10 cents for width, a
    // broken Boss BF-2 oscillating, and Watkins Copicat tape echo.
    {"cocteau", "Cocteau Twins · Cherry-Coloured", "COCTEAU",
     "Chorus into an inverted flanger into a ten-cent detune into tape echo "
     "into an enormous bright hall. Ten cents is not a slider's worth — it is "
     "the Lexicon trick that makes this sound stereo in mono.",
     "Eventide SP2016 · Lexicon 480L (±10 cents) · Boss BF-2 · Watkins Copicat · Roland Dimension D",
     {S::Chorus, S::Flanger, S::Pitch, S::Delay, S::Reverb},
     merge(chorus(0.28f, 7.5f, 0.80f),
           flanger(0.12f, 3.0f, 4.5f, -0.55f, 0.45f),
           pitch(0.0f, 10.0f, 0.45f),
           delay(0.38f, 0.50f, 0.45f, 3.5f, 0.32f),
           reverb(kHall, 1.0f, 0.20f, 0.70f))},

    // Nirvana. A Japanese Boss DS-1 for the distortion and an Electro-Harmonix
    // Small Clone for the watery clean parts, in that order, into a big clean
    // amp. The Small Clone has no rate control on the pedal — it is fixed and
    // slow, which is why every use of it sounds the same.
    {"nirvana", "Nirvana · Small Clone", "NIRVANA",
     "DS-1 into a Small Clone: serrated distortion with the fixed, slow, very "
     "wet chorus that made the Come As You Are intro.",
     "Boss DS-1 (1980s Japan) · Electro-Harmonix Small Clone · Fender Bassman / Mesa Studio .22",
     {S::Distortion, S::Chorus, S::Amp, S::Reverb},
     merge(drive(9.0f, 1.0f),
           chorus(1.1f, 5.5f, 0.55f),
           amp(3.0f, 1.0f, -2.0f, 5.0f, 3.0f, 2.5f, 0.72f, 0.60f),
           reverb(kRoom, 0.30f, 0.55f, 0.12f))},

    // Radiohead. The Marshall ShredMaster is the OK Computer distortion; the
    // DigiTech Whammy is on both Jonny Greenwood's and Ed O'Brien's boards and
    // is what makes Paranoid Android and My Iron Lung sound the way they do.
    // Small Stone phaser and a Demeter Tremulator finish it.
    {"radiohead", "Radiohead · ShredMaster", "RADIOHED",
     "ShredMaster distortion with a Whammy octave sitting under it, a Small "
     "Stone breathing over the top, and a Tremulator ticking underneath.",
     "Marshall ShredMaster · DigiTech Whammy · EHX Small Stone · Demeter Tremulator",
     {S::Distortion, S::Pitch, S::Phaser, S::Amp, S::Tremolo, S::Delay, S::Reverb},
     merge(drive(8.5f, 0.95f),
           pitch(12.0f, 0.0f, 0.22f),
           phaser(0.40f, 0.85f, 0.50f, 4.0f, 0.40f),
           amp(4.0f, 1.0f, 1.0f, 3.0f, 4.0f, 3.0f, 0.68f, 0.62f),
           tremolo(4.5f, 0.25f, 0.20f),
           delay(0.36f, 0.35f, 0.65f, 1.0f, 0.22f),
           reverb(kHall, 0.70f, 0.40f, 0.28f))},

    // =========================================================== single effects
    //
    // The plain versions, so each stage can be heard on its own — which is how
    // the band presets above were dialled in, and how a fault gets isolated to
    // one stage when something sounds wrong.

    {"shoegaze", "Shoegaze", "SHOEGAZE",
     "The original milestone-7 chain: fuzz, chorus and reverb stacked.",
     "—",
     {S::Distortion, S::Chorus, S::Reverb},
     merge(drive(2.5f, 0.40f), chorus(0.6f, 3.0f, 0.50f), reverb(kHall, 0.70f, 0.40f, 0.40f))},

    {"overdrive", "Overdrive", "DRIVE",
     "Soft tanh saturation. Keeps the note's envelope, unlike the fuzz.",
     "—", {S::Distortion}, drive(6.0f, 1.0f)},

    {"fuzz", "Fuzz", "FUZZ",
     "Hard, squared-off clipping with adjustable bias asymmetry and sputter gate.",
     "—", {S::Fuzz}, fuzz(30.0f, 0.15f, 0.0f, 0.5f, 0.55f, 1.0f)},

    {"compressor", "Compressor", "COMP",
     "Feed-forward peak compressor. Watch the gain-reduction readout.",
     "—", {S::Compressor}, compressor(-24.0f, 4.0f, 8.0f, 150.0f, 8.0f)},

    {"eq", "Tone / EQ", "EQ",
     "Low shelf, sweepable mid peak, high shelf. Set a high Mid Q for a parked wah.",
     "—", {S::Tone}, tone(0.0f, 0.0f, 800.0f, 0.9f, 0.0f)},

    {"chorus", "Chorus", "CHORUS",
     "Modulated 15 ms delay: one detuned copy alongside the dry signal.",
     "—", {S::Chorus}, chorus(0.8f, 4.0f, 0.5f)},

    {"flanger", "Flanger", "FLANGER",
     "Short modulated delay with feedback. Try negative feedback for the hollow one.",
     "—", {S::Flanger}, flanger(0.25f, 2.0f, 3.0f, 0.6f, 0.5f)},

    {"phaser", "Phaser", "PHASER",
     "Four swept allpass stages summed with the dry signal.",
     "—", {S::Phaser}, phaser(0.5f, 0.8f, 0.4f, 4.0f, 0.5f)},

    {"tremolo", "Tremolo", "TREMOLO",
     "Amplitude modulation. Shape sweeps the LFO from sine to near-square.",
     "—", {S::Tremolo}, tremolo(5.0f, 0.6f, 0.0f)},

    {"delay", "Digital Delay", "DELAY",
     "Clean repeats: no wow, no filtering in the feedback path.",
     "—", {S::Delay}, delay(0.35f, 0.4f, 1.0f, 0.0f, 0.35f)},

    {"tape", "Tape Echo", "TAPE",
     "The same delay line with a lowpass in the feedback loop and a wobbling "
     "transport: each repeat darker and slightly out of tune.",
     "—", {S::Delay}, delay(0.32f, 0.55f, 0.35f, 6.0f, 0.35f)},

    {"room", "Reverb · Room", "ROOM",
     "Short, neutral, a little damped. The default space.",
     "—", {S::Reverb}, reverb(kRoom, 0.45f, 0.55f, 0.30f)},

    {"plate", "Reverb · Plate", "PLATE",
     "Bright and dense, with a fast build — a steel plate, not a space.",
     "—", {S::Reverb}, reverb(kPlate, 0.65f, 0.25f, 0.35f)},

    {"hall", "Reverb · Hall", "HALL",
     "Long, dark, slow to arrive. The one the shoegaze presets lean on.",
     "—", {S::Reverb}, reverb(kHall, 0.90f, 0.40f, 0.40f)},

    {"spring", "Reverb · Spring", "SPRING",
     "Short, resonant and band-limited to roughly 320 Hz - 3 kHz, which is most "
     "of why a tank sounds like a tank rather than a room.",
     "—", {S::Reverb}, reverb(kSpring, 0.55f, 0.45f, 0.30f)},

    {"octave-up", "Octave Up", "OCT UP",
     "Full-wet +12 semitones. The warble is the algorithm, not a bug.",
     "—", {S::Pitch}, pitch(12.0f, 0.0f, 0.5f)},

    {"octave-down", "Octave Down", "OCT DOWN",
     "Full-wet -12 semitones.",
     "—", {S::Pitch}, pitch(-12.0f, 0.0f, 0.5f)},

    {"detune", "Detune", "DETUNE",
     "Twelve cents sharp against the dry signal — width without an audible interval.",
     "—", {S::Pitch}, pitch(0.0f, 12.0f, 0.5f)},

    {"crusher", "Bit Crusher", "CRUSH",
     "Bit-depth quantisation and sample-rate decimation, as separate knobs.",
     "—", {S::BitCrusher}, crush(6.0f, 4.0f, 1.0f)},

    {"ringmod", "Ring Mod", "RINGMOD",
     "Multiplication by a sine carrier. Inharmonic by construction.",
     "—", {S::RingMod}, ring(220.0f, 0.6f)},

    {"folder", "Wave Folder", "FOLDER",
     "Folds instead of clipping: past the threshold the wave turns around and "
     "travels back. Harmonics that do not fall off with frequency, and a timbre "
     "set by how hard you pick rather than by the knob.",
     "—", {S::WaveFolder}, fold(6.0f, 0.0f, 0.55f, 1.0f)},

    {"autowah", "Env Filter", "AUTOWAH",
     "A resonant lowpass dragged around by your picking. Dig in and it opens; "
     "back off and it closes. The only effect here your hands control directly.",
     "—", {S::EnvFilter}, env(220.0f, 2600.0f, 2.2f, 5.0f, 6.0f, 180.0f, 1.0f)},

    {"downwah", "Env Filter · Inverted", "DOWNWAH",
     "The same follower with negative sensitivity, so the filter shuts as you "
     "dig in. Strange and synthetic — nothing acoustic behaves this way.",
     "—", {S::EnvFilter}, env(1400.0f, 2400.0f, -1.6f, 6.0f, 5.0f, 260.0f, 1.0f)},

    {"funkfilter", "Funk Filter", "FUNK",
     "Compressor into the envelope filter, which is the order that makes an "
     "auto-wah usable: even dynamics mean the filter sweeps the same distance "
     "on every note instead of only on the hard ones.",
     "—", {S::Compressor, S::EnvFilter},
     merge(compressor(-22.0f, 4.0f, 6.0f, 90.0f, 6.0f),
           env(180.0f, 2800.0f, 2.6f, 6.5f, 4.0f, 140.0f, 1.0f))},
    };
    return presets;
}

}  // namespace

const std::vector<Pedalboard::PresetSpec>& preset_table() { return table(); }
