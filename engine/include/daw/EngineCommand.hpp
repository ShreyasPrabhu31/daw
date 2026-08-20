#pragma once

#include <cstdint>

namespace daw {

// Fixed-layout, trivially-copyable message the UI thread pushes into the
// RingBuffer for the audio thread to apply. No pointers, no dynamic size:
// the audio thread must be able to copy this without touching the heap.
enum class CommandType : std::uint8_t {
    NoteOn,  // intValue = MIDI note, floatValue = velocity
    NoteOff, // intValue = MIDI note
    AllNotesOff,
    SetWaveform, // intValue = Waveform
    SetSynthGain,
    SetAttack,  // milliseconds
    SetDecay,   // milliseconds
    SetSustain, // linear level, 0 to 1
    SetRelease, // milliseconds
    SetFilterType, // intValue = Biquad::Type
    SetFilterCutoff,
    SetFilterResonance,

    // Mixer
    SetTrackGain,
    SetTrackPan,
    SetTrackMute, // intValue nonzero to mute
    SetTrackSolo, // intValue nonzero to solo
    SetMasterGain,
    SetMasterSaturation, // intValue nonzero to enable

    // Transport
    TransportPlay,
    TransportStop,
    TransportSetPosition, // intValue = sample position
    TransportSetLoop,     // intValue nonzero to enable looping
};

// Every command here is applied as soon as the audio thread drains it. Timed
// musical events are not commands: they live in the Timeline, which compiles
// them into a sorted schedule the renderer walks with a cursor. Keeping the
// two apart is what stopped a note's position from being a property of the
// message that happened to carry it.
struct EngineCommand {
    CommandType type;
    float floatValue = 0.0f;
    std::int32_t intValue = 0;
    std::uint8_t track = 0;
};

} // namespace daw
