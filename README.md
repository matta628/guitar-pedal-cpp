# guitar-pedal-cpp

A real-time software guitar pedal running on a Raspberry Pi 5: guitar in → C++ audio DSP chain
→ audio out, played live while actually practicing guitar. Fourteen effect stages (compression,
amp modelling, overdrive, fuzz, bit crushing, ring modulation, EQ, pitch shifting, phaser, flanger,
chorus, tremolo, delay, reverb) plus a looper, wired into 34 presets — a dozen of which are
attempts at specific records. Aimed at a shoegaze-leaning sound (Cocteau Twins, My Bloody
Valentine).

No pre-recorded processing — the point is playing live through the chain, with the same
fixed-deadline-per-buffer discipline (no allocation, no locks, no syscalls in the hot path) that
low-latency audio and low-latency trading systems both live by. Miss the deadline and you get an
audible glitch (an "xrun") — a concrete, demoable failure mode.

This is my first substantial production C++ project alongside
[matching-engine-cpp](https://github.com/matta628/matching-engine-cpp) (prior experience was
academic/coursework only), so expect commit history that includes fundamentals as they're
introduced, not just DSP-domain code.

## Background

- **Distortion/fuzz** — clips/squashes the waveform, adding harsh harmonics. Core of My Bloody
  Valentine's wall-of-sound.
- **Chorus / flanger / phaser** — mixes the signal with a slightly delayed, pitch-wobbled copy of
  itself (a few ms, modulated by a slow oscillator/LFO). Most of what makes Cocteau Twins sound
  like Cocteau Twins.
- **Delay** — a longer, discrete echo (tens–hundreds of ms), often with feedback so it repeats and
  decays.
- **Reverb** — simulates a room/space via hundreds of tiny decaying echoes (e.g. a
  Schroeder/Freeverb-style feedback delay network) — the "cathedral wash" in both bands' sound.
- **Looper** — records a phrase into a buffer, then plays it back on repeat while you play over it;
  a differentiator from the Mustang Micro, which has no looper logic at all.
- **Why latency matters** — the core constraint is finishing each audio buffer's processing before
  the next one arrives, typically a few milliseconds of budget. That's why allocation, locks, and
  syscalls in the audio callback matter so much — any of them can blow the deadline unpredictably
  and produce an audible glitch.

## Signal path / hardware

Guitar → Fender Mustang Micro (clean/flat amp model, onboard effects off — it's the I/O interface,
not a competitor to this project) → USB-C (standard USB Audio Class device) → Raspberry Pi 5 →
C++ audio engine (ALSA/JACK for I/O) → headphones or back out to an amp, for live monitoring while
playing.

## Architecture

1. **Audio I/O layer** — ALSA/JACK (or a portable wrapper like RtAudio) capture/playback, fixed
   buffer size, real-time-safe callback.
2. **DSP chain** — each effect (distortion, delay, chorus, reverb) as its own processing stage with
   a uniform "process this buffer" interface, so stages can be composed/reordered.
3. **Parameter control** — knobs/settings per effect (gain, mix, delay time, feedback, rate/depth),
   changeable without allocating or blocking in the audio thread.
4. **Preset chaining** — one instance of every stage lives for the whole run, and a preset is an
   ordered array of pointers into them plus a table of parameter values. Switching a preset
   allocates nothing and frees nothing; it is a fixed number of atomic stores and one published
   integer.
5. **Control surface** — GPIO footswitches for looper transport and preset switching, with LED and
   LCD1602 status output. All of it runs on one 50 Hz indicator thread; none of it touches the
   audio callback.
6. **Telemetry + dev UI** — the audio callback publishes levels, timing and a waveform window
   through lock-free structures; a dependency-free HTTP server on its own thread serves a browser
   UI over the network for bench work.

## Roadmap

- [x] **0. Toolchain + fundamentals** — CMake project skeleton, `-Wall -Wextra`, a sanitizer build
      (ASan/UBSan)
- [ ] **1. Clean passthrough** — guitar → Pi → out, near-zero added latency; the foundational
      "no allocation/locks in the audio callback" milestone everything else builds on
- [x] **2. Distortion / fuzz** — waveshaping/clipping
- [x] **3. Delay** — feedback-driven echo
- [x] **4. Chorus** — LFO-modulated short delay line
- [x] **5. Reverb** — algorithmic reverb (Schroeder/Freeverb-style feedback delay network)
- [x] **6. Looper** — single mono track, fixed max buffer length, no overdub for v1: record → loop
      playback → stop/clear on one control. Builds on the ring-buffer foundation from Delay.
- [ ] **7. "Shoegaze mode"** — chain fuzz + chorus + reverb into one preset, play through it live
- [ ] **8. Stretch: physical footswitches + status display** — two momentary footswitches on the
      Pi's GPIO, plus LED and LCD status output. Switch 1 drives the looper
      (record → play → overdub); switch 2 clears the loop on a single click and cycles the effect
      preset on a double click. `GpioButton`, `GpioLed` and `Lcd1602` (libgpiod v2, Linux-only) all
      build and run on the target Pi 5. What's left is the physical half — nothing is wired to the
      header yet, so a real press has never been observed end to end.
- [x] **9. Stretch: looper overdub** — layer additional passes onto an existing loop
- [x] **10. Dev UI** — the pedal serves its own bench console over HTTP: live input/output metering,
      a waveform scope, callback time against the buffer deadline, direct preset selection, looper
      transport, and live parameter tweaking. Read path is lock-free from the audio thread; write
      path reuses the same atomics the footswitches use.
- [x] **11. Effect library + preset system** — ten more stages (compressor, amp, fuzz, bit crusher,
      ring modulator, EQ, pitch shifter, phaser, flanger, tremolo) and tape controls on the delay,
      arranged by a `Pedalboard` that owns every stage for the life of the program. 34 presets,
      saveable per-preset from the browser. See [Effects and presets](#effects-and-presets).

## Dev environment

Primary development and the deployment target are the same box: a Raspberry Pi 5 (4-core Cortex-A76,
2GB RAM, no SMT, no CUDA — a genuinely constrained ARM target). That's also a shared, noisy box
(runs other always-on services), which makes for an honest "measured under real background load"
number rather than a sterile one.

## Build

Dependencies: [RtAudio](https://github.com/thestk/rtaudio) (`brew install rtaudio` on macOS,
`apt install librtaudio-dev` on the Pi). On the Pi, also `apt install libgpiod-dev` for footswitch
support — CMake links it automatically when present and defines `PEDAL_HAVE_GPIO`; without it (e.g.
on macOS) the build just skips GPIO entirely and the looper has no physical trigger.

`GpioButton` targets the **libgpiod v2** API (Debian 13 / Raspberry Pi OS trixie ship v2 only; it
is not source-compatible with v1). Built and tested against RtAudio 6.0.1 and libgpiod 2.2.1 on
GCC 14.

```
cmake -S . -B build
cmake --build build
./build/guitar_pedal
```

`guitar_pedal` takes a few options, all optional:

| Flag | Meaning |
|---|---|
| `--device <id\|name>` | capture device, by id or case-insensitive name substring |
| `--out-device <id\|name>` | playback device (defaults to the capture device) |
| `--rate <hz>` / `--frames <n>` | sample rate and buffer size (default: device preference, 256) |
| `--port <n>` | dev UI port (default 8080) |
| `--no-web` | don't serve the dev UI |
| `--simulate` | start with the built-in signal generator on |
| `--list` | print the audio devices and exit |

Without `--device` it picks the first duplex device rather than the system default, which on a Pi
is HDMI or nothing. **The stream failing to open is not fatal** — the dev UI, footswitches and LCD
still come up and the UI reports why audio isn't running, which is a more useful screen than an
exit code when the board isn't in the room.

Add `-DENABLE_SANITIZERS=ON` to the configure step for an ASan+UBSan build.

On the Pi the build also produces `./build/gpio_check`, a bench diagnostic that exercises one piece
of hardware at a time (`gpio_check led 22`, `gpio_check button 17`, `gpio_check clicks 27`,
`gpio_check lcd`, `gpio_check all`). It needs no audio interface, so wiring can be verified before
the pedal itself is in the picture.

Every DSP stage implements one interface — `process(float* buffer, size_t n_frames)` — which is
what lets the pedalboard treat them as interchangeable and what makes them testable against
synthetic signals with no audio hardware present. `./build/offline_tests` (or `ctest` from the
build dir) runs that suite: 116 assertions covering the stages, the looper state machine, the
telemetry seqlock, and the preset table.

### Dev UI

`http://<pi>:8080/` while the pedal is running. It exists because the LCD is two lines of sixteen
characters and a lot of what matters while building this — how hot the input actually is, how much
of the buffer deadline the chain is eating, what the waveform looks like after the reverb — doesn't
fit there.

| Panel | What it answers |
|---|---|
| Levels | Is the guitar reaching the Pi at a sane level, and is the output clipping? Peak + RMS in dBFS, with a latched clip count. |
| Waveform | What the signal looks like before and after the chain, on one ~43 ms window. |
| Deadline | Callback time (last / average / worst) against the `frames / rate` budget, plus the xrun count. This is the number the whole project is about. |
| Effect | Direct preset selection, rather than cycling the footswitch to get back to the one you wanted. |
| Looper | State, loop length, and a live playhead — plus trigger/clear, so looper logic can be tested with no switch wired. |
| Parameters | Live sliders for every DSP knob, so a tone gets tuned by ear instead of by rebuild. |
| Control surface | A mirror of the LCD and which GPIO devices actually opened. |

Design notes, since this is the part most likely to be got wrong:

- **The audio thread never blocks on it.** `Telemetry` is written with relaxed atomic stores into
  preallocated members. The waveform window is too big for one atomic, so it uses a **seqlock**:
  the writer bumps a counter to odd, copies, bumps to even, and never waits; a reader that sees a
  torn read simply retries and drops a UI frame. A stalled reader costs a dropped frame; a stalled
  writer would cost an audible glitch, so the asymmetry runs the right way.
- **Commands take the paths that already existed** — an atomic preset index and the looper's
  pending-action flags. The browser is just another control thread alongside the footswitches; it
  gets no privileged access to the engine.
- **No third-party HTTP or JSON library.** One thread, one `poll()` loop, six routes. Small state
  is *pushed* over server-sent events at 25 Hz (one-directional and periodic, so a WebSocket frame
  codec and upgrade handshake would buy nothing); the bulkier waveform is *pulled* as raw float32
  at ~15 Hz, so a slow client throttles itself instead of backing up the server.
- **The page is compiled into the binary** from `web/index.html` by `tools/embed_html.cmake`, so
  deployment stays one file with no runtime path to get wrong.

Measured on the Pi 5 with the shoegaze chain (fuzz → chorus → reverb → looper) at 48 kHz / 256
frames, driven by the built-in generator with no interface attached: **~120 µs average per buffer
against a 5333 µs deadline** — ~185 µs with the looper overdubbing — so 2–3.5% of budget, while
serving three concurrent UI clients. Worth re-measuring against real capture once the interface is
in, since ALSA's own callback overhead isn't in that number.

### Effects and presets

Fourteen stages, each a self-contained `Effect`:

| Stage | What it does |
|---|---|
| Compressor | Feed-forward peak compressor, hard knee, separate attack and release. Reports live gain reduction to the UI. |
| Amp | Two gain stages with a passive-style tone stack between them, then a master stage, a presence shelf and a speaker lowpass. |
| Distortion | Soft `tanh` saturation — keeps the note's envelope. |
| Fuzz | Hard clipping with adjustable bias asymmetry, a sputter gate and a post-clip tone control. |
| Bit Crusher | Bit-depth quantisation and sample-rate decimation as independent knobs. |
| Ring Mod | Multiplication by a sine carrier. Inharmonic by construction. |
| Tone | Low shelf, sweepable mid peak with adjustable Q, high shelf. A high Mid Q is a parked wah. |
| Pitch Shifter | Two crossfaded delay taps read at a rate other than the write rate. ±24 semitones plus cents. |
| Phaser | 2–8 swept allpass stages with feedback, summed against the dry signal. |
| Flanger | Short modulated delay with feedback, positive or negative. |
| Chorus | Modulated ~15 ms delay — one detuned copy alongside the dry signal. |
| Tremolo | Amplitude modulation whose shape sweeps from sine to near-square. |
| Delay | Feedback echo, plus a lowpass *inside* the feedback loop and a wow/flutter LFO on the delay time — the two things that separate a tape echo from a digital one. |
| Reverb | Freeverb-style comb and allpass network. |

`Biquad` (the RBJ cookbook formulas, Direct Form I) is shared by `Tone` and `Amp` rather than
written twice. Direct Form I specifically, because coefficients change while the filter is running
whenever a knob moves, and DF1 keeps input and output history separate, which is far better
behaved under that than DF2.

#### Why the pedalboard owns every stage

`Pedalboard` holds one instance of each stage for the entire run. A preset is an ordered
`std::array<Effect*, 10>` into those instances, plus a list of `(parameter index, value)` pairs
resolved to indices at startup. Selecting a preset applies the values and publishes one integer;
the audio thread's whole job is a relaxed load and a walk over a fixed array of pointers.

The obvious alternative — building a `std::vector<std::unique_ptr<Effect>>` per preset and swapping
it on switch — is what this design exists to avoid. It would allocate on whichever thread switched
(a footswitch interrupt, or an HTTP request), and it would free the outgoing chain while an audio
callback might still be walking it. Neither is recoverable at 48 kHz.

The cost of sharing instances is real and worth stating: **a preset must set every parameter of
every stage it uses.** Anything left unspecified keeps whatever the previously selected preset put
there, so the sound would depend on which preset you were on before — the kind of bug that is
maddening to chase by ear, because the preset is only wrong sometimes. The preset table treats
exhaustive specification as a rule, and `offline_tests` enforces it: a preset that omits a
parameter of a stage in its chain fails the suite by name.

Two related decisions run deliberately opposite ways. A shipped preset naming a parameter that does
not exist **throws at startup**, because that is a typo in this repo and a silent skip would
resurface as the inheritance bug above. A line in the *user's* saved-settings file that no longer
resolves is **skipped with a warning**, because a settings file written by an older build must
never stop the pedal from booting.

#### The presets

Twelve are attempts at specific records, worked out by ear against them and against what the
players were actually using. The rest are single-effect presets for hearing one stage on its own,
plus four reverb voicings and a `clean` baseline.

| Preset | Chain | After |
|---|---|---|
| Slowdive · Soft Focus | Drive → Chorus → Delay → Reverb | Yamaha FX500 "Soft Focus" patch, Boss CE-2, Roland RE-201 |
| Elliott Smith · Double-Tracked | Compressor → Drive → Chorus → Delay → Reverb | Budda Phatman, Line 6 DL-4, doubled parts |
| The Strokes · Transporterraum | Compressor → Drive → Amp → Reverb | ProCo RAT, Boss DS-1, Fender Hot Rod DeVille |
| Arctic Monkeys · Valveslapper | Fuzz → Amp → Phaser → Delay → Reverb | Coopersonic Valveslapper, Fulltone Mini DejaVibe |
| The Voidz · Tyranny | Fuzz → Bit Crusher → Ring Mod → Pitch → Delay → Reverb | Bit-reduced chains, Boss DB-5 |
| Wednesday · Bull Believer | Drive → Fuzz → Tone → Amp → Delay → Reverb | RAT into a Big Muff, parked Cry Baby |
| Led Zeppelin · Tone Bender | Fuzz → Tone → Amp → Delay → Reverb | Sola Sound Tone Bender MkII, parked Vox V846, Echoplex EP-3 |
| Deftones · Around the Fur | Drive → Tone → Amp → Pitch → Delay → Reverb | Bogner Uberschall, MXR M109, Eventide H9 |
| Velvet Underground · Ostrich | Drive → Amp → Tremolo → Reverb | Vox AC100 with mid booster, on-board tremolo |
| Cocteau Twins · Cherry-Coloured | Chorus → Flanger → Pitch → Delay → Reverb | Lexicon 480L, Boss BF-2, Watkins Copicat, Dimension D |
| Nirvana · Small Clone | Drive → Chorus → Amp → Reverb | Boss DS-1 into an EHX Small Clone |
| Radiohead · ShredMaster | Drive → Pitch → Phaser → Amp → Tremolo → Delay → Reverb | Marshall ShredMaster, DigiTech Whammy, EHX Small Stone |

A few of these encode something specific rather than a general vibe. The Cocteau Twins preset
detunes by **ten cents**, not by a slider's worth — that is the Lexicon 480L trick that makes the
part sound stereo even in mono. The Zeppelin preset parks a wah toe-down as a treble booster
instead of sweeping it, and leans on the Echoplex's audible wow, which is why `Delay` grew a
modulation control. The Voidz preset is the reason `BitCrusher` takes fractional bit depths: 5.5
bits is a real setting there, and an integer-only knob would step through fifteen audible jumps
instead of sweeping.

Preset order is also the footswitch cycle order, which is why `clean` sits first.

#### Saving

Knob positions can be saved per preset from the dev UI, to
`~/.config/guitar-pedal-cpp/presets.conf`. Saved edits are stored as a diff against the table
rather than as a replacement for it, so "reset to default" is a deletion rather than a second table
of remembered originals, and a preset that gains a new parameter in a later build still gets a
sensible value for it. The file is plain text on purpose: it needs no parser beyond a stream
extraction, it diffs readably, and it can be fixed in a text editor over SSH when a preset has been
saved unlistenable. Writes go to a temporary file and are renamed over the target, so a crash
halfway through leaves the previous settings intact rather than a truncated file.

### Control surface wiring (Pi only)

| Function | GPIO | Header pin |
|---|---|---|
| Looper switch — record → play → overdub | 17 | 11 |
| Utility switch — click: clear · double-click: cycle preset | 27 | 13 |
| Looper LED — solid: recording · slow blink: playing · fast blink: overdubbing | 22 | 15 |
| Preset LED — flashes the preset number; one long flash on clear | 23 | 16 |
| LCD1602 RS / E / D4 / D5 / D6 / D7 | 5 / 6 / 13 / 19 / 26 / 16 | 29 / 31 / 33 / 35 / 37 / 36 |

Wire one leg of each momentary pushbutton to its GPIO line and the other to GND — the code requests
the lines with an internal pull-up, so no external resistor is needed. Each LED needs a series
resistor (220Ω is fine) between the GPIO pin and its anode, with the cathode to GND. The LCD runs in
4-bit mode with `RW` tied to GND: the panel is write-only, so no 5V logic line ever drives a
(non-5V-tolerant) Pi input, and the driver waits out fixed datasheet delays rather than polling the
busy flag.

A double-click cycles through all 34 presets in table order, starting from `clean`. That is a lot
of presses to cross by foot, which is the honest cost of putting a whole library behind one switch
— the dev UI's direct selection exists because cycling does not scale, and the LCD's second line
shows the current preset's short name so you can see where you are. Clean is a real preset rather
than a bypass flag: the looper sits after the pedalboard and runs in every preset, so a loop
recorded through fuzz still plays back after switching to clean.

Single-click detection is inherently late: it can't fire until the 350 ms double-click window
closes. That's why the gesture pair lives on the utility switch and never on the looper switch,
where the press instant defines the loop boundary.

The Pi 5 drives its 40-pin header through the RP1 chip. On this kernel `gpiodetect` reports it as
`gpiochip0` (54 lines), which is what `src/main.cpp` uses; older Pi 5 kernels enumerated it as
`gpiochip4`, so run `gpiodetect` and adjust `kGpioChip` and the line constants in `main()` if your
board or kernel differs. `guitar_pedal` prints which of the switches, LEDs and LCD armed
successfully on startup, and keeps running without any of them.

Ctrl+C stops the passthrough and prints the xrun count.
