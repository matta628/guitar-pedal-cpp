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

Each DSP stage (`Distortion`, `Delay`, `Chorus`, `Reverb`, `Looper`) is a self-contained
`process(float* buffer, size_t n_frames)` unit, offline-testable against synthetic signals without
any audio hardware — `./build/offline_tests` (or `ctest` from the build dir) runs that suite.

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

The preset cycle is `shoegaze → clean → fuzz → chorus → reverb → …`. Clean is a real preset rather
than a bypass flag — the looper runs in every preset, so a loop recorded through fuzz still plays
back after switching to clean.

Single-click detection is inherently late: it can't fire until the 350 ms double-click window
closes. That's why the gesture pair lives on the utility switch and never on the looper switch,
where the press instant defines the loop boundary.

The Pi 5 drives its 40-pin header through the RP1 chip. On this kernel `gpiodetect` reports it as
`gpiochip0` (54 lines), which is what `src/main.cpp` uses; older Pi 5 kernels enumerated it as
`gpiochip4`, so run `gpiodetect` and adjust `kGpioChip` and the line constants in `main()` if your
board or kernel differs. `guitar_pedal` prints which of the switches, LEDs and LCD armed
successfully on startup, and keeps running without any of them.

Ctrl+C stops the passthrough and prints the xrun count.
