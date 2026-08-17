#!/usr/bin/env bash
# Builds engine.wasm as a standalone module: no JS glue, no ALLOW_MEMORY_GROWTH
# (growth would detach the ArrayBuffer and invalidate the Float32Array views
# the audio thread reuses every block). Requires emsdk on PATH.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/web"

if ! command -v em++ >/dev/null 2>&1; then
    echo "error: em++ not found on PATH. Install emsdk: https://emscripten.org/docs/getting_started/downloads.html" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"

em++ \
    "$ROOT_DIR/engine/src/Engine.cpp" \
    "$ROOT_DIR/engine/src/Oscillator.cpp" \
    "$SCRIPT_DIR/bindings.cpp" \
    -I "$ROOT_DIR/engine/include" \
    -std=c++20 \
    -O3 \
    -sSTANDALONE_WASM \
    --no-entry \
    -sALLOW_MEMORY_GROWTH=0 \
    -sINITIAL_MEMORY=16MB \
    -sEXPORTED_FUNCTIONS=_daw_init,_daw_get_channel_ptr,_daw_set_frequency,_daw_set_waveform,_daw_set_gain,_daw_render \
    -o "$OUT_DIR/engine.wasm"

echo "wrote $OUT_DIR/engine.wasm"
