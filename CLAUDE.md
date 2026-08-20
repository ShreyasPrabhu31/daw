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
- `Engine` owns the synth and nodes, drains commands, renders, meters, clamps
- `Transport` sample position, tempo, loop points
- `RingBuffer<T, N>` wait-free SPSC queue, power-of-two capacity, acquire/release
- `ParameterSmoother` one-pole ramp, prevents zipper noise on knob moves
- `Oscillator` PolyBLEP sine/saw/square
- `Published<T>` two-slot handover, release publish, reader echo for retirement
- `BufferPool` one preallocated output buffer per graph node
- `Graph` topological sort on the message thread, flat plan walked on audio
- `Timeline` notes in ticks, compiled to a sorted sample-timed schedule
- `Track` synth into gain and constant-power pan, publishes a decaying peak
- `MasterBus` sums the mix, saturates with a C1-continuous knee
- `ADSR` linear-segment envelope, exact termination so voices free reliably
- `Biquad` RBJ cookbook low/high/band pass, transposed direct form II
- `Voice` oscillator into filter into envelope, sums into the block
- `Synth` fixed pool of 8 voices, MIDI frequency table, voice stealing

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
- Graph topology is sorted on the message thread and handed over as a flat
  plan. The audio thread walks an array; it never traverses a graph, recurses,
  or allocates a visit set
- `Graph::rebuild` may block waiting to retire a plan slot, so it is message
  thread only. It is compiled to a non-blocking refusal under Emscripten,
  where the caller shares the audio thread

## Current state: Phase 3 complete

Four tracks of 8-voice polyphony through a node graph into a saturating
master bus, arranged as clips on a timeline, played in the browser and
rendered to disk. 233 checks passing, clean under ASan, UBSan, and TSan.

The arrangement lives in the engine, in musical time. `Timeline` stores notes
in ticks and compiles them into a sorted schedule of sample-timed events; the
audio thread walks that schedule with a cursor, so firing an event is a
compare and an increment and a seek is a binary search. A tempo change is a
recompile of the same notes rather than the host resending them.

`Published<T>` owns the acquire/release handshake that hands a large value
from the message thread to the audio thread: two slots, a release store to
publish, and an echo of the slot the reader took so the writer knows when a
retired slot is safe to overwrite. Both the graph plan and the event schedule
go through it, so that ordering is written and tested once.

The no-allocation rule is enforced, not merely asserted.
`testAudioThreadNeverAllocates` overrides global `operator new`, renders 400
blocks while starting notes, ending them, forcing voice stealing, changing
parameters and varying the block size, and fails if the counter moves. It
also asserts the counter is non-zero beforehand, so the test cannot pass by
having dead instrumentation.

Measured on an Apple M2 (Mac14,2, 8 cores), macOS 26.5.2, Apple clang 21,
`CMAKE_BUILD_TYPE=Release`. Native render path, 128-frame blocks at 48 kHz,
three runs of 1875 blocks each. Worst value of the three shown:

| workload | p50 | p99 | max |
|---|---|---|---|
| 4 tracks, 16-step loop | 9.42 us | 17.38 us | 225.58 us |
| 4 tracks, 32 voices held | 41.58 us | 90.21 us | 955.71 us |

Budget is 2666.67 us. The max column is much noisier than p50 or p99 and
moves by a factor of six between otherwise identical runs, which is the
signature of the process being descheduled rather than of the engine getting
slower: p99 stays put while max jumps. Treat max as a property of an
unpinned laptop, not of the DSP, and quote the worst column regardless.

The browser renders the same arrangement offline in a Worker, using a second
instance of the same wasm module. A four-bar arrangement at 110 BPM, 9.7
seconds of audio, renders in 38 to 44 ms, which is roughly 220 to 260 times
faster than real time. That is a browser measurement and a preview of the
Phase 4 render endpoint.

Known gaps, in the order they matter:

- Nobody has listened to the output yet. Every claim above is structural:
  tests assert on buffer contents and timing, not on whether it sounds good.
- Nothing persists. Reload the page and the arrangement is gone. There is no
  save, no load, no undo, and no audio export from the browser. Phase 4 is
  where the project stops being a demo and starts being something a musician
  could keep work in.
- Worklet block times have never been measured from inside the browser. The
  table above is the native path, which is not the same code path.
- `daw_timeline_compile` runs `std::sort` over up to 1024 events, and in the
  browser it runs on the worklet thread, because a worklet's message handler
  shares that thread with `process()`. It allocates nothing and is bounded,
  but it is real work on the audio thread and has not been measured there.
  Compiling on the main thread and shipping the finished schedule across
  would remove the question entirely.
- Web MIDI has only been exercised with no device attached, where it
  correctly reports unavailable. The note decoding path has never seen real
  controller traffic.
- Graph topology is fixed once rendering starts in the browser, so all four
  tracks are created in `daw_init`. Adding a track live needs the retire path
  to be non-blocking.
- A loop wrap or a seek releases every voice, which prevents stuck notes but
  also cuts a note that legitimately sustains across the loop point.

## Layout

```
engine/include/daw/   headers, the portable core
engine/src/           Engine, Graph, MasterBus, Oscillator, Synth, Timeline, Track, Voice
cli/                  offline WAV renderer + benchmark harness
wasm/                 Emscripten bindings and build.sh
web/                  worklet.js, arrangement/pianoroll/waveform, bounce worker
tests/                dependency-free runner
.github/workflows/    3-OS matrix + sanitizer jobs
```

## Commands

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

# A single note, then a chord. Repeat --note for polyphony.
./build/cli/daw_render out.wav --seconds 2 --note 69 --wave saw
./build/cli/daw_render chord.wav --seconds 3 --note 60 --note 64 --note 67 \
    --wave saw --cutoff 2500 --benchmark

# Four panned tracks, a scheduled arpeggio, looping over the pattern.
./build/cli/daw_render arp.wav --seconds 5 --tracks 4 \
    --note 48 --note 55 --note 60 --note 67 --arp 16 --bpm 120 --loop --benchmark

cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DDAW_TSAN=ON
cmake --build build-tsan -j && ./build-tsan/tests/daw_tests

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

Phase 1 added: envelope stage transitions and exact release termination,
release time staying constant regardless of the level it started from,
percussive (zero sustain) patches freeing their voice, filter magnitude
response per type, filter stability at out-of-range cutoff and resonance,
voices summing rather than overwriting, full polyphony, stealing preferring a
releasing voice over a held one, note off hitting only the matching note, and
MIDI note 69 sounding at 440 Hz, and a global operator new override proving
the render path allocates nothing.

Phase 3 added: tick to sample conversion across tempos, a compiled schedule
being sorted, a note-off ordering before a note-on at the same sample so
back-to-back same-pitch notes both sound, capacity and zero-length refusal,
per-track clearing, a seek repositioning the cursor, a tempo change moving
notes without the host resending them, and per-track meters rising and
decaying independently.

Phase 2 added: topological ordering of dependencies, fan-in summing across a
diamond, cycles being refused while the previously published plan keeps
running, unrouted nodes contributing nothing, a plan republished twenty times
under a concurrently rendering thread, soft clip boundedness and slope
continuity at the knee, constant-power panning, mute and solo resolution,
tracks summing into the bus, a scheduled note landing on its exact sample
rather than the next block boundary, events staying pending under a stopped
playhead, and loop passes re-arming events including one on sample zero.

Add to this file rather than starting a new framework, unless the suite outgrows
it, in which case move to Catch2.

## Roadmap

- **Phase 1** (done) ADSR envelope, biquad filter, `Voice` class, 8-voice
  polyphony with a preallocated pool and voice stealing, Web MIDI input
- **Phase 2** (done) node graph with topological sort computed on the message
  thread and walked as a flat array on the audio thread, multiple tracks into a
  master bus, sample-accurate event scheduling, play/stop/loop
- **Phase 3** (done) canvas timeline with draggable clips, mixer strips, level
  meters, waveform rendering with min/max peak caching
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
