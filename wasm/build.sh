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
    "$ROOT_DIR/engine/src/Graph.cpp" \
    "$ROOT_DIR/engine/src/MasterBus.cpp" \
    "$ROOT_DIR/engine/src/Track.cpp" \
    "$ROOT_DIR/engine/src/Oscillator.cpp" \
    "$ROOT_DIR/engine/src/Synth.cpp" \
    "$ROOT_DIR/engine/src/Voice.cpp" \
    "$SCRIPT_DIR/bindings.cpp" \
    -I "$ROOT_DIR/engine/include" \
    -std=c++20 \
    -O3 \
    -sSTANDALONE_WASM \
    --no-entry \
    -sALLOW_MEMORY_GROWTH=0 \
    -sINITIAL_MEMORY=16MB \
    -sEXPORTED_FUNCTIONS=_daw_init,_daw_get_channel_ptr,_daw_note_on,_daw_note_off,_daw_all_notes_off,_daw_schedule_note_on,_daw_schedule_note_off,_daw_clear_scheduled_events,_daw_set_waveform,_daw_set_attack,_daw_set_decay,_daw_set_sustain,_daw_set_release,_daw_set_filter_type,_daw_set_filter_cutoff,_daw_set_filter_resonance,_daw_set_track_gain,_daw_set_track_pan,_daw_set_track_mute,_daw_set_track_solo,_daw_set_master_gain,_daw_set_master_saturation,_daw_transport_play,_daw_transport_stop,_daw_transport_set_position,_daw_transport_set_loop,_daw_set_loop_range,_daw_transport_position,_daw_transport_is_playing,_daw_active_voice_count,_daw_peak_level,_daw_num_tracks,_daw_render \
    -o "$OUT_DIR/engine.wasm"

echo "wrote $OUT_DIR/engine.wasm"
