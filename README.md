# daw

A browser-based digital audio workstation built around a portable C++20 engine.
See [CLAUDE.md](CLAUDE.md) for the full architecture, roadmap, and the
real-time rules that govern the audio path.

## Status

Phase 1: an 8-voice polyphonic synth with a PolyBLEP oscillator, linear ADSR,
and a resonant biquad filter, playable in the browser and renderable to disk.
Voices come from a fixed preallocated pool with stealing, so nothing is
allocated on the audio thread. 96 checks pass under plain, ASan+UBSan, and
TSan builds.

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Render a single note, then a chord. `--note` takes a MIDI note number and can
be repeated for polyphony.

```sh
./build/cli/daw_render out.wav --seconds 2 --note 69 --wave saw
```

```sh
./build/cli/daw_render chord.wav --seconds 3 --note 60 --note 64 --note 67 --wave saw --cutoff 2500 --benchmark
```

Other flags: `--velocity`, `--attack`, `--decay`, `--sustain`, `--release`,
`--resonance`, `--hold` (fraction of the render before the keys lift), and
`--benchmark` for p50/p99/max block times against the deadline.

## Browser test bench

Requires [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
on `PATH` (or `brew install emscripten`).

```sh
./wasm/build.sh
```

```sh
cd web && python3 -m http.server 8080
```

Open `http://localhost:8080` and hit **Start audio**. Play with the on-screen
keyboard, your computer keyboard (`A`–`;` for white keys, `W E T Y U O P` for
black), or a MIDI controller. The voice counter shows the pool working,
including stealing once you hold more than eight notes.

Serve over HTTP, not `file://` — the page fetches `engine.wasm`, which needs a
real origin.

## Sanitizers

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DDAW_TSAN=ON && cmake --build build-tsan -j && ./build-tsan/tests/daw_tests
```

Swap `-DDAW_TSAN=ON` for `-DDAW_ASAN=ON` to get AddressSanitizer and
UndefinedBehaviorSanitizer instead.
