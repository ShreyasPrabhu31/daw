#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include "daw/AudioBuffer.hpp"
#include "daw/EngineCommand.hpp"
#include "daw/Node.hpp"
#include "daw/RingBuffer.hpp"
#include "daw/Synth.hpp"
#include "daw/Transport.hpp"

namespace daw {

// Owns the synth plus an optional chain of effect Nodes, drains queued
// commands, renders, meters, and clamps. render() is the only method called
// from the audio thread; everything else runs on the message thread before
// rendering starts.
class Engine {
public:
    static constexpr std::size_t kMaxNodes = 32;
    static constexpr std::size_t kCommandQueueCapacity = 256;

    void prepare(double sampleRate, std::size_t maxBlockSize);

    // Message thread only. nodes_ is never resized once rendering starts.
    bool addNode(Node* node) noexcept;

    // Message thread only.
    [[nodiscard]] bool pushCommand(const EngineCommand& command) noexcept;

    // Audio thread. Drains queued commands once at the start of the block,
    // renders the synth and effect chain, then clamps and meters.
    void render(AudioBuffer& buffer) noexcept;

    [[nodiscard]] float peakLevel() const noexcept { return peakLevel_.load(std::memory_order_relaxed); }

    [[nodiscard]] Synth& synth() noexcept { return synth_; }
    [[nodiscard]] Transport& transport() noexcept { return transport_; }

private:
    void drainCommands() noexcept;
    void applyCommand(const EngineCommand& command) noexcept;

    std::array<Node*, kMaxNodes> nodes_{};
    std::size_t numNodes_ = 0;

    Synth synth_;
    Transport transport_;

    RingBuffer<EngineCommand, kCommandQueueCapacity> commandQueue_;

    std::atomic<float> peakLevel_{0.0f};
    bool prepared_ = false;
};

} // namespace daw
