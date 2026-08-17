#pragma once

#include "daw/AudioBuffer.hpp"

namespace daw {

// prepare() runs on the message thread before rendering starts and may
// allocate. process() runs on the audio thread and must not.
class Node {
public:
    virtual ~Node() = default;

    virtual void prepare(double sampleRate, std::size_t maxBlockSize) = 0;
    virtual void process(AudioBuffer& buffer) noexcept = 0;
};

} // namespace daw
