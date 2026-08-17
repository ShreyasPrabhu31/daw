# Project context

A browser-based digital audio workstation built around a portable C++20 engine.

## Why this project exists

Portfolio piece and shippable product, aimed at audio and systems roles (Core
Audio, real-time C++). The point is to demonstrate real-time correctness,
performance work, and the discipline of shipping, not to compete with Logic Pro.
When a tradeoff comes up between "more features" and "one thing done to a
professional standard," pick the latter.

## Architecture

One C++ core, three host adapters. The core has zero platform dependencies,
which is what lets the same code be a website, a server-side renderer, and later
an audio plugin.

```
UI (main thread)
  |  postMessage commands
AudioWorkletProcessor (audio thread, 128 frames)
  |  calls into
engine.wasm  <-- same source as -->  native CLI renderer
                                     JUCE AU/VST3 wrapper (Phase 5)
```

Core pieces, all under `engine/`:

- `AudioBuffer` non-owning planar view over host-owned memory
- `Node` abstract base, `prepare()` on the message thread, `process()` on audio
- `Engine` owns nodes, drains commands, renders, meters, clamps
- `Transport` sample position, tempo, loop points
- `RingBuffer<T, N>` wait-free SPSC queue, power-of-two capacity, acquire/release
- `ParameterSmoother` one-pole ramp, prevents zipper noise on knob moves
- `Oscillator` PolyBLEP sine/saw/square

## The rule that governs everything

`Engine::render` and every `Node::process` run on the audio thread. They must
not allocate, lock, throw, log, or call into the OS. At 48 kHz with 128-frame
blocks there are ~2.67 ms between callbacks, and one slow-path `malloc` can miss
that deadline and produce an audible dropout.

Consequences to enforce in every review:

- No `new`, `malloc`, `std::vector::push_back`, `std::string`, or container
  resize inside `process()` or anything it calls
- No `std::mutex`, no `std::shared_ptr` copies (the refcount is an atomic RMW
  with unbounded contention), no `printf`, no file or network I/O
- All UI-to-audio traffic goes through `RingBuffer`, drained once at the start
  of each block, never mid-block
- Anything the audio thread needs is allocated in `prepare()` and handed over
- Nodes are added only before rendering starts; `nodes_` is never resized on the
  audio thread

## Current state: Phase 0 complete

A single oscillator rendering in the browser and to disk. Verified: builds
clean, 38 checks passing, clean under ASan, UBSan, and TSan. The WASM path
has been run end to end in a real browser (fetch on the main thread, transfer
to the worklet, `_initialize` then `daw_init`, live parameter changes via
`daw_set_frequency`/`daw_set_waveform`/`daw_set_gain`) with no console errors.

Local timings on a shared VM (a floor, not a result, re-measure on real
hardware): p50 2.21 us, p99 2.29 us, max 2.96 us, budget 2666.67 us.

Known gap: the browser run above only proves the wiring — nobody has listened
to the output yet or measured worklet block times from inside the browser
(the CLI numbers above are the native render path, not the worklet). That is
the next thing to verify before trusting this under real playback.

## Layout

```
engine/include/daw/   headers, the portable core
engine/src/           Oscillator.cpp, Engine.cpp
cli/                  offline WAV renderer + benchmark harness
wasm/                 Emscripten bindings and build.sh
web/                  worklet.js, index.html test bench, _headers
tests/                dependency-free runner
.github/workflows/    3-OS matrix + sanitizer jobs
```

## Commands

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/daw_render out.wav --seconds 2 --freq 220 --wave saw

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DDAW_TSAN=ON
cmake --build build-tsan -j && ./build-tsan/daw_tests

./wasm/build.sh                      # needs emsdk on PATH
cd web && python3 -m http.server 8080
```

## Conventions

- C++20, `-Wall -Wextra -Wpedantic -Wshadow`, warnings treated as real
- CMake only, no Projucer `.jucer` files even after JUCE lands
- `daw::` namespace, headers under `engine/include/daw/`
- `noexcept` on everything on the audio path, `[[nodiscard]]` on pure getters
- Comments explain *why*, especially where a choice is dictated by the real-time
  constraint. Do not narrate what the code already says.
- Debug DSP through the CLI renderer and a WAV file first. Debugging audio
  through browser devtools is miserable and slow.
- Every new DSP node needs a test that asserts on buffer contents, not just that
  it compiles

## Testing approach

`tests/test_engine.cpp` is a plain assert-based runner, no third-party deps.
Existing coverage: ring buffer FIFO ordering, a real two-thread producer and
consumer (this is the test TSan needs to prove the memory ordering), silence
before `prepare()`, sine frequency accuracy via zero-crossing count, stereo
mirroring, output clamping, variable block sizes, and graceful queue overflow.

Add to this file rather than starting a new framework, unless the suite outgrows
it, in which case move to Catch2.

## Roadmap

- **Phase 1** ADSR envelope, biquad filter, `Voice` class, 8-voice polyphony
  with a preallocated pool and voice stealing, Web MIDI input
- **Phase 2** node graph with topological sort computed on the message thread
  and walked as a flat array on the audio thread, multiple tracks into a master
  bus, sample-accurate event scheduling, play/stop/loop
- **Phase 3** canvas timeline with draggable clips, mixer strips, level meters,
  waveform rendering with min/max peak caching
- **Phase 4** backend service (Drogon or Crow), project save/load, offline render
  endpoint running the same engine faster than real time
- **Phase 5** JUCE wrapper for AU/VST3, Google Benchmark, published numbers

Phase 2 is where `SharedArrayBuffer` becomes necessary, which is why the
COOP/COEP headers in `web/_headers` are already in place.

## Things to be careful about

- An AudioWorklet has no `fetch` and no DOM. WASM bytes are fetched on the main
  thread and transferred via `postMessage`. Do not "simplify" this.
- The WASM build is standalone (`-sSTANDALONE_WASM --no-entry`) with no JS glue.
  Static constructors run via the exported `_initialize`, which the worklet must
  call before `daw_init`.
- `ALLOW_MEMORY_GROWTH=0` is deliberate: growth detaches the `ArrayBuffer` and
  invalidates the `Float32Array` views the audio thread reuses.
- Hosts do not promise a constant block size. Never assume 128.
- Measure p99 and max block time, never the mean. The mean hides exactly the
  outliers a listener hears as a click.
- Do not put a number in the README or on a resume that has not been measured on
  named hardware.
