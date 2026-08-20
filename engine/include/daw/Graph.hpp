#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "daw/AudioBuffer.hpp"
#include "daw/BufferPool.hpp"
#include "daw/Node.hpp"
#include "daw/Published.hpp"

namespace daw {

// A directed acyclic graph of Nodes, sorted once on the message thread and
// walked as a flat array on the audio thread.
//
// The audio thread must never run a topological sort, so the sort produces a
// Plan: a linear list of "clear this buffer, sum these inputs into it, then
// run this node". Executing the graph is then a loop over contiguous memory
// with no graph traversal, no recursion, and no branching on topology.
//
// The plan is handed over through Published<Plan>, which owns the acquire /
// release handshake that keeps a plan from being rewritten while it is being
// walked.
class Graph {
public:
    static constexpr std::size_t kMaxNodes = 64;
    static constexpr std::size_t kMaxConnections = 256;
    static constexpr std::size_t kInvalidNode = kMaxNodes;

    using NodeId = std::size_t;

    // Message thread. Sizes the buffer pool for the worst case, so adding
    // nodes later never reallocates.
    void prepare(double sampleRate, std::size_t maxBlockSize, std::size_t numChannels);

    // Message thread. Returns kInvalidNode if the graph is full.
    NodeId addNode(Node* node);

    // Message thread. Both endpoints must already exist. Duplicate edges are
    // rejected so a node cannot accidentally sum the same source twice.
    bool connect(NodeId source, NodeId destination);
    bool disconnect(NodeId source, NodeId destination);

    // The node whose buffer is copied to the host output.
    void setOutputNode(NodeId id) noexcept { outputNode_ = id; }
    [[nodiscard]] NodeId outputNode() const noexcept { return outputNode_; }

    // Message thread. Sorts and publishes a new plan. Returns false and
    // leaves the running plan untouched if the graph contains a cycle or if
    // the retired slot is still in use.
    bool rebuild();

    // Audio thread. Renders into the caller's buffer, which may be a slice.
    void process(AudioBuffer& output) noexcept;

    [[nodiscard]] std::size_t numNodes() const noexcept { return numNodes_; }
    [[nodiscard]] bool hasLivePlan() const noexcept { return plans_.hasLive(); }

    // Execution order of the live plan, for tests and for showing the signal
    // flow in the UI later.
    [[nodiscard]] std::size_t planOrder(std::size_t position) const noexcept;
    [[nodiscard]] std::size_t planLength() const noexcept;

private:
    struct Entry {
        std::uint8_t node;
        std::uint8_t inputStart;
        std::uint8_t inputCount;
    };

    struct Plan {
        std::array<Entry, kMaxNodes> entries{};
        std::array<std::uint8_t, kMaxConnections> inputs{};
        std::size_t entryCount = 0;
        std::size_t inputCount = 0;
        std::uint8_t output = 0;
        bool hasOutput = false;
    };

    struct Connection {
        std::uint8_t source;
        std::uint8_t destination;
    };

    std::array<Node*, kMaxNodes> nodes_{};
    std::size_t numNodes_ = 0;

    std::array<Connection, kMaxConnections> connections_{};
    std::size_t numConnections_ = 0;

    NodeId outputNode_ = kInvalidNode;

    BufferPool pool_;
    double sampleRate_ = 48000.0;
    std::size_t maxBlockSize_ = 0;
    bool prepared_ = false;

    Published<Plan> plans_;
};

} // namespace daw
