#pragma once

#include <cstdint>

namespace daw {

// Fixed-layout, trivially-copyable message the UI thread pushes into the
// RingBuffer for the audio thread to apply. No pointers, no dynamic size:
// the audio thread must be able to copy this without touching the heap.
enum class CommandType : std::uint8_t {
    SetFrequency,
    SetWaveform,
    SetGain,
    TransportPlay,
    TransportStop,
};

struct EngineCommand {
    CommandType type;
    float floatValue = 0.0f;
    std::int32_t intValue = 0;
};

} // namespace daw
