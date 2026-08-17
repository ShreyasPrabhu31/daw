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
    ClearScheduledEvents,
};

// `scheduled` is a separate flag rather than a sentinel value of `time`.
// Overloading "time == 0" to mean immediate looks tidy until you try to place
// an event on the very first sample of a loop: it silently becomes a live
// gesture, fires once, and never repeats. Sample zero is a perfectly ordinary
// position on the timeline and has to stay addressable.
//
// When `scheduled` is set, `time` is an absolute transport position in
// samples, honoured exactly by splitting the render block at that boundary.
struct EngineCommand {
    CommandType type;
    float floatValue = 0.0f;
    std::int32_t intValue = 0;
    std::uint8_t track = 0;
    bool scheduled = false;
    std::uint64_t time = 0;
};

} // namespace daw
