# guitar-pedal-cpp

A real-time software guitar pedal running on a Raspberry Pi 5: guitar in → C++ audio DSP chain
(distortion, chorus, delay, reverb, looper) → audio out, played live while actually practicing
guitar. Aimed at a shoegaze-leaning sound (Cocteau Twins, My Bloody Valentine).

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
4. **Preset chaining** — combining stages into a named signal chain (e.g. "shoegaze mode" = fuzz +
   chorus + reverb).
5. **Stretch: GPIO footswitches** — physical control input to switch presets/bypass effects live.

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
- [ ] **8. Stretch: physical footswitches** — wire real footswitches to the Pi's GPIO.
      `GpioButton` (libgpiod v2, Linux-only) is wired into the looper's trigger and now builds and
      runs on the target Pi 5: it opens `gpiochip0`, requests line 17 as a pulled-up input, and
      runs its debounce thread clean under ASan/UBSan. What's left is the physical half — no button
      is wired to the header yet, so a real press has never been observed end to end.
- [x] **9. Stretch: looper overdub** — layer additional passes onto an existing loop

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

Add `-DENABLE_SANITIZERS=ON` to the configure step for an ASan+UBSan build.

Each DSP stage (`Distortion`, `Delay`, `Chorus`, `Reverb`, `Looper`) is a self-contained
`process(float* buffer, size_t n_frames)` unit, offline-testable against synthetic signals without
any audio hardware — `./build/offline_tests` (or `ctest` from the build dir) runs that suite.

### Footswitch wiring (Pi only)

Wire one leg of a momentary pushbutton to GPIO17 (physical pin 11), the other leg to any GND pin
(e.g. physical pin 9) — the code requests the line with an internal pull-up, so no external
resistor is needed. Pressing the button pulls the line to ground; `GpioButton` debounces the
transition on a background thread and fires the looper trigger.

The Pi 5 drives its 40-pin header through the RP1 chip. On this kernel `gpiodetect` reports it as
`gpiochip0` (54 lines), which is what `src/main.cpp` uses; older Pi 5 kernels enumerated it as
`gpiochip4`, so run `gpiodetect` and adjust `kFootswitchChip` / `kFootswitchLine` in `main()` if
your board or kernel differs. `guitar_pedal` prints whether the footswitch armed successfully on
startup either way, and keeps running without it.

Ctrl+C stops the passthrough and prints the xrun count.
