# daw

A browser-based digital audio workstation built around a portable C++20 engine.
See [CLAUDE.md](CLAUDE.md) for the full architecture, roadmap, and the
real-time rules that govern the audio path.

## Status

Phase 3: four tracks of 8-voice polyphony through a node graph into a
saturating master bus, arranged as clips on a timeline, playable in the
browser and renderable to disk.

- **Graph** — nodes are topologically sorted on the message thread and handed
  to the audio thread as a flat execution plan. The audio thread walks an
  array; it never traverses topology. Cycles are refused and the previously
  published plan keeps running.
- **Timeline** — the arrangement lives in the engine, in musical ticks.
  Compiling expands notes into a sorted schedule of sample-timed events that
  the renderer walks with a cursor, so firing is a compare and an increment
  and a seek is a binary search. Changing tempo recompiles the same notes
  rather than the host resending them.
- **Mixer** — per-track gain, constant-power pan, mute, solo, and a decaying
  peak meter, summed into a bus that saturates with a C1-continuous knee
  instead of clamping.
- **Handover** — `Published<T>` carries a large value from the message thread
  to the audio thread with two slots, a release store, and an echo of the slot
  the reader took, so a value is never rewritten while it is being read. Both
  the graph plan and the event schedule use it.

Nothing on the audio path allocates, and a test that overrides global
`operator new` fails if it ever does. 233 checks pass under plain, ASan+UBSan,
and TSan builds.

Measured on an Apple M2 (Mac14,2), macOS 26.5.2, Release build. 128-frame
blocks at 48 kHz, worst of three runs:

| workload | p50 | p99 | max |
|---|---|---|---|
| 4 tracks, 16-step loop | 9.42 µs | 17.38 µs | 225.58 µs |
| 4 tracks, 32 voices held | 41.58 µs | 90.21 µs | 955.71 µs |

Against a 2666.67 µs deadline. The max column swings by a factor of six across
identical runs while p99 stays put, which is the shape of an unpinned laptop
descheduling the process rather than the DSP getting slower. Native path, not
the WASM path.

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

Open `http://localhost:8080` and hit **Start audio**.

- Click an empty lane to add a clip; drag it between lanes and along time,
  drag its right edge to resize, `Delete` to remove.
- The selected clip opens in the note editor below; click cells to add and
  remove notes.
- **Play loop** runs the arrangement with the playhead tracking the grid.
- **Bounce** renders the whole arrangement in a Worker, using a second
  instance of the same engine, and draws the result. A four-bar arrangement
  renders in tens of milliseconds — a few hundred times faster than real time.
- Mixer strips give each track gain, pan, mute, solo, a level meter, and a
  voice count that shows the pool stealing once you hold more than eight notes.

Serve over HTTP, not `file://` — the page fetches `engine.wasm`, which needs a
real origin.

Note that nothing persists yet: reloading the page loses the arrangement.
Save and load are Phase 4.

## Sanitizers

```sh
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DDAW_TSAN=ON && cmake --build build-tsan -j && ./build-tsan/tests/daw_tests
```

Swap `-DDAW_TSAN=ON` for `-DDAW_ASAN=ON` to get AddressSanitizer and
UndefinedBehaviorSanitizer instead.
