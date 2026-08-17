#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "daw/AudioBuffer.hpp"
#include "daw/EngineCommand.hpp"
#include "daw/Graph.hpp"
#include "daw/MasterBus.hpp"
#include "daw/RingBuffer.hpp"
#include "daw/Track.hpp"
#include "daw/Transport.hpp"

namespace daw {

// Owns the mixer (tracks into a master bus, wired through the graph), the
// transport, and the scheduler. render() is the only method the audio thread
// calls; everything else is message thread.
class Engine {
public:
    static constexpr std::size_t kMaxTracks = 8;
    static constexpr std::size_t kCommandQueueCapacity = 256;
    static constexpr std::size_t kMaxScheduledEvents = 128;
    static constexpr std::uint64_t kNever = ~std::uint64_t{0};

    void prepare(double sampleRate, std::size_t maxBlockSize, std::size_t numChannels = 2);

    // Message thread. Creates a track, wires it to the master bus, and
    // republishes the graph plan. Returns kMaxTracks if the mixer is full.
    std::size_t addTrack();

    [[nodiscard]] std::size_t numTracks() const noexcept { return numTracks_; }
    [[nodiscard]] Track& track(std::size_t index) noexcept { return tracks_[index]; }
    [[nodiscard]] MasterBus& master() noexcept { return master_; }
    [[nodiscard]] Graph& graph() noexcept { return graph_; }
    [[nodiscard]] Transport& transport() noexcept { return transport_; }

    // Message thread. Returns false if the queue is full.
    [[nodiscard]] bool pushCommand(const EngineCommand& command) noexcept;

    // Audio thread.
    void render(AudioBuffer& output) noexcept;

    [[nodiscard]] float peakLevel() const noexcept { return peakLevel_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t scheduledEventCount() const noexcept;

private:
    struct ScheduledEvent {
        EngineCommand command{};
        bool occupied = false;
        bool fired = false;
    };

    void drainCommands() noexcept;
    void applyCommand(const EngineCommand& command) noexcept;
    void schedule(const EngineCommand& command) noexcept;

    // Fires everything due at or before `now` and reports when the next one
    // is, so the renderer knows where to split the block.
    void fireDueEvents(std::uint64_t now) noexcept;
    [[nodiscard]] std::uint64_t nextEventTime(std::uint64_t after) const noexcept;

    // A loop pass makes events inside the loop range eligible again.
    void rearmLoopedEvents() noexcept;

    void resolveTrackActivity() noexcept;

    std::array<Track, kMaxTracks> tracks_{};
    std::size_t numTracks_ = 0;
    std::array<bool, kMaxTracks> muted_{};
    std::array<bool, kMaxTracks> soloed_{};

    MasterBus master_;
    Graph graph_;
    Transport transport_;

    RingBuffer<EngineCommand, kCommandQueueCapacity> commandQueue_;
    std::array<ScheduledEvent, kMaxScheduledEvents> scheduled_{};

    std::atomic<float> peakLevel_{0.0f};
    bool prepared_ = false;
};

} // namespace daw
