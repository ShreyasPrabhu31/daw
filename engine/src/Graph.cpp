#include "daw/Graph.hpp"

// Sleeping is only ever done by the message thread of a native host. Pulling
// <thread> into the WASM build would add wasi_snapshot_preview1 imports that a
// standalone module has no host to satisfy, and the browser never takes this
// path anyway.
#if !defined(__EMSCRIPTEN__)
#include <chrono>
#include <thread>
#endif

namespace daw {

void Graph::prepare(double sampleRate, std::size_t maxBlockSize, std::size_t numChannels) {
    sampleRate_ = sampleRate;
    maxBlockSize_ = maxBlockSize;

    // Sized for the worst case up front so that adding a node later is never
    // an allocation, and so the pointers a live plan holds stay valid.
    pool_.prepare(kMaxNodes, numChannels, maxBlockSize);

    for (std::size_t i = 0; i < numNodes_; ++i) {
        nodes_[i]->prepare(sampleRate, maxBlockSize);
    }
    prepared_ = true;
}

Graph::NodeId Graph::addNode(Node* node) {
    if (node == nullptr || numNodes_ >= kMaxNodes) return kInvalidNode;

    const NodeId id = numNodes_;
    nodes_[numNodes_++] = node;

    // Nodes added after the graph is running still need their one-time setup.
    if (prepared_) node->prepare(sampleRate_, maxBlockSize_);
    return id;
}

bool Graph::connect(NodeId source, NodeId destination) {
    if (source >= numNodes_ || destination >= numNodes_) return false;
    if (source == destination) return false;
    if (numConnections_ >= kMaxConnections) return false;

    for (std::size_t i = 0; i < numConnections_; ++i) {
        const bool duplicate = connections_[i].source == source && connections_[i].destination == destination;
        if (duplicate) return false; // summing a source into the same node twice is a bug, not a feature
    }

    connections_[numConnections_++] = {static_cast<std::uint8_t>(source),
                                       static_cast<std::uint8_t>(destination)};
    return true;
}

bool Graph::disconnect(NodeId source, NodeId destination) {
    for (std::size_t i = 0; i < numConnections_; ++i) {
        if (connections_[i].source != source || connections_[i].destination != destination) continue;
        connections_[i] = connections_[numConnections_ - 1];
        --numConnections_;
        return true;
    }
    return false;
}

// Waits for the audio thread to stop reading a slot before it is rewritten.
// Message thread only: sleeping here is fine, and a graph edit is rare.
bool Graph::retireSlot(int slot) {
    if (slot < 0) return true;
    if (consumedPlan_.load(std::memory_order_acquire) != slot) return true;

#if defined(__EMSCRIPTEN__)
    // In an AudioWorklet, rebuild() would be called from the port's message
    // handler, which shares the audio thread with process(). Blocking there
    // is exactly the dropout this whole design exists to avoid, so refuse the
    // edit and let the caller retry on a later message instead.
    return false;
#else
    // A running stream at 48 kHz with 128-frame blocks produces a block every
    // 2.67 ms, so a 5 ms window with no progress means rendering is stopped
    // and nobody can be inside process().
    constexpr auto kProbe = std::chrono::milliseconds(5);
    constexpr int kMaxAttempts = 200; // one second, then give up rather than hang

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        const std::uint64_t before = renderCount_.load(std::memory_order_acquire);
        std::this_thread::sleep_for(kProbe);

        if (consumedPlan_.load(std::memory_order_acquire) != slot) return true;
        if (renderCount_.load(std::memory_order_acquire) == before) return true;
    }
    return false;
#endif
}

bool Graph::rebuild() {
    // Kahn's algorithm. Everything here is a fixed-size array so the sort has
    // the same shape whether it runs in a test or on a loaded machine.
    std::array<std::uint8_t, kMaxNodes> inDegree{};
    for (std::size_t i = 0; i < numConnections_; ++i) {
        ++inDegree[connections_[i].destination];
    }

    std::array<std::uint8_t, kMaxNodes> ready{};
    std::size_t head = 0;
    std::size_t tail = 0;
    for (std::size_t node = 0; node < numNodes_; ++node) {
        if (inDegree[node] == 0) ready[tail++] = static_cast<std::uint8_t>(node);
    }

    std::array<std::uint8_t, kMaxNodes> order{};
    std::size_t orderCount = 0;

    while (head < tail) {
        const std::uint8_t node = ready[head++];
        order[orderCount++] = node;

        for (std::size_t i = 0; i < numConnections_; ++i) {
            if (connections_[i].source != node) continue;
            const std::uint8_t destination = connections_[i].destination;
            if (--inDegree[destination] == 0) ready[tail++] = destination;
        }
    }

    // Fewer nodes ordered than exist means at least one never reached
    // in-degree zero, which is exactly the definition of a cycle. Refuse the
    // edit and leave the running plan alone rather than publish a graph that
    // cannot be walked.
    if (orderCount != numNodes_) return false;

    if (!retireSlot(writeSlot_)) return false;

    Plan& plan = plans_[static_cast<std::size_t>(writeSlot_)];
    plan.entryCount = 0;
    plan.inputCount = 0;

    for (std::size_t i = 0; i < orderCount; ++i) {
        const std::uint8_t node = order[i];

        Entry entry{};
        entry.node = node;
        entry.inputStart = static_cast<std::uint8_t>(plan.inputCount);
        entry.inputCount = 0;

        for (std::size_t c = 0; c < numConnections_; ++c) {
            if (connections_[c].destination != node) continue;
            plan.inputs[plan.inputCount++] = connections_[c].source;
            ++entry.inputCount;
        }

        plan.entries[plan.entryCount++] = entry;
    }

    plan.hasOutput = outputNode_ < numNodes_;
    plan.output = static_cast<std::uint8_t>(plan.hasOutput ? outputNode_ : 0);

    livePlan_.store(writeSlot_, std::memory_order_release);
    writeSlot_ = 1 - writeSlot_;
    return true;
}

void Graph::process(AudioBuffer& output) noexcept {
    const int slot = livePlan_.load(std::memory_order_acquire);

    // Tell the message thread which slot is in use before touching it, and
    // count the block so it can tell a stalled stream from a busy one. Only
    // this thread writes renderCount_, so a plain store beats a read-modify-
    // write here.
    consumedPlan_.store(slot, std::memory_order_release);
    renderCount_.store(renderCount_.load(std::memory_order_relaxed) + 1, std::memory_order_release);

    const std::size_t frames = output.numFrames();
    if (slot < 0 || frames > maxBlockSize_) {
        output.clear();
        return;
    }

    const Plan& plan = plans_[static_cast<std::size_t>(slot)];

    for (std::size_t i = 0; i < plan.entryCount; ++i) {
        const Entry& entry = plan.entries[i];

        // A node reads whatever its inputs summed to, so the contract is the
        // same for a source (nothing connected, buffer is silent) and an
        // effect (buffer holds the mix of everything upstream).
        AudioBuffer buffer = pool_.view(entry.node, frames);
        buffer.clear();

        for (std::size_t k = 0; k < entry.inputCount; ++k) {
            const AudioBuffer source = pool_.view(plan.inputs[entry.inputStart + k], frames);
            buffer.addFrom(source);
        }

        nodes_[entry.node]->process(buffer);
    }

    if (plan.hasOutput) {
        const AudioBuffer result = pool_.view(plan.output, frames);
        output.copyFrom(result);
    } else {
        output.clear();
    }
}

std::size_t Graph::planLength() const noexcept {
    const int slot = livePlan_.load(std::memory_order_acquire);
    if (slot < 0) return 0;
    return plans_[static_cast<std::size_t>(slot)].entryCount;
}

std::size_t Graph::planOrder(std::size_t position) const noexcept {
    const int slot = livePlan_.load(std::memory_order_acquire);
    if (slot < 0) return kInvalidNode;

    const Plan& plan = plans_[static_cast<std::size_t>(slot)];
    if (position >= plan.entryCount) return kInvalidNode;
    return plan.entries[position].node;
}

} // namespace daw
