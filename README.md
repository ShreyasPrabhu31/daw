# daw

A browser-based digital audio workstation built around a portable C++20 engine.
See [CLAUDE.md](CLAUDE.md) for the full architecture, roadmap, and the
real-time rules that govern the audio path.

## Status

Phase 2: four tracks of 8-voice polyphony running through a node graph into a
saturating master bus, sequenced with sample-accurate events.

- **Graph** — nodes are topologically sorted on the message thread and handed
  to the audio thread as a flat execution plan. The audio thread walks an
  array; it never traverses topology. Cycles are refused and the previously
  published plan keeps running. Plans are swapped through two preallocated
  slots with an atomic publish and acknowledgement, so a plan is never
  rewritten while it is being read.
- **Mixer** — per-track gain, constant-power pan, mute and solo, summed into a
  master bus that saturates with a C1-continuous knee rather than clamping.
- **Timeline** — events carry an absolute sample position and fire exactly
  there, because the render block is split at event boundaries. Looping
  re-arms events inside the loop range.

Nothing on the audio path allocates, and a test that overrides global
`operator new` fails if it ever does. 186 checks pass under plain, ASan+UBSan,
and TSan builds.

Measured on an Apple M2 (Mac14,2), macOS 26.5.2, Release build. 128-frame
blocks at 48 kHz, worst of three runs:

| workload | p50 | p99 | max |
|---|---|---|---|
| 4 tracks, 16-step loop | 9.58 µs | 11.25 µs | 62.54 µs |
| 1 track, 8 voices held | 10.17 µs | 14.25 µs | 48.46 µs |
| 4 tracks, 32 voices held | 38.75 µs | 53.79 µs | 112.88 µs |

Against a 2666.67 µs deadline. Native path, not the WASM path.

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure
```

Render a chord. `--note` takes a MIDI note number and repeats for polyphony.

```sh
./build/cli/daw_render chord.wav --seconds 3 --note 60 --note 64 --note 67 --wave saw --cutoff 2500
```

Render four panned tracks playing a scheduled, looping arpeggio.

```sh
./build/cli/daw_render arp.wav --seconds 5 --tracks 4 --note 48 --note 55 --note 60 --note 67 --arp 16 --bpm 120 --loop --benchmark
```

Other flags: `--velocity`, `--attack`, `--decay`, `--sustain`, `--release`,
`--resonance`, `--hold`, `--no-saturation`, and `--benchmark` for p50/p99/max
block times against the deadline.

## Browser test bench

Requires [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
on `PATH` (or `brew install emscripten`).

```sh
./wasm/build.sh && cd web && python3 -m http.server 8080
```

Open `http://localhost:8080` and hit **Start audio**. Click steps to build a
pattern and press **Play loop**; the playhead lights the current step. The
armed track takes keyboard input (`A`–`;` white keys, `W E T Y U O P` black)
or MIDI. Mixer strips give each track gain, pan, mute and solo, and a live
voice count that shows the pool stealing once you hold more than eight notes.

Serve over HTTP, not `file://` — the page fetches `engine.wasm`, which needs a
real origin.

## Sanitizers

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DDAW_TSAN=ON && cmake --build build-tsan -j && ./build-tsan/tests/daw_tests
```

Swap `-DDAW_TSAN=ON` for `-DDAW_ASAN=ON` to get AddressSanitizer and
UndefinedBehaviorSanitizer instead.
