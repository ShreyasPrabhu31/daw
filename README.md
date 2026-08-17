# daw

A browser-based digital audio workstation built around a portable C++20 engine.
See [CLAUDE.md](CLAUDE.md) for the full architecture, roadmap, and the
real-time rules that govern the audio path.

## Status

Phase 0: a single PolyBLEP oscillator, rendering to WAV via the CLI and to
the browser via WASM + AudioWorklet. 38 checks pass under plain, ASan+UBSan,
and TSan builds.

## Quick start

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

./build/cli/daw_render out.wav --seconds 2 --freq 220 --wave saw --benchmark
```

## Browser test bench

Requires [emsdk](https://emscripten.org/docs/getting_started/downloads.html)
on `PATH`.

```sh
./wasm/build.sh
cd web && python3 -m http.server 8080
```

Then open `http://localhost:8080`. Serve over HTTP, not `file://` — the
worklet's `fetch` of `engine.wasm` needs a real origin.
