#pragma once

#include "daw/AudioBuffer.hpp"

namespace daw {

// prepare() runs on the message thread before rendering starts and may
// allocate. process() runs on the audio thread and must not.
//
// The graph hands process() a buffer already holding the sum of everything
// connected upstream, and takes whatever is left in it as this node's output.
// That single contract covers both roles: a source sees a cleared buffer
// because nothing feeds it, and an effect sees its input mix. Nodes never
// look up their own inputs, which is what keeps the audio-thread walk a flat
// loop instead of a graph traversal.
class Node {
public:
    virtual ~Node() = default;

    virtual void prepare(double sampleRate, std::size_t maxBlockSize) = 0;
    virtual void process(AudioBuffer& buffer) noexcept = 0;
};

} // namespace daw
