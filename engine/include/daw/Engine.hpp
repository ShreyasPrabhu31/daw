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
#include "daw/Timeline.hpp"
#include "daw/Track.hpp"
#include "daw/Transport.hpp"

namespace daw {

// Owns the mixer (tracks into a master bus, wired through the graph), the
// transport, and the timeline. render() is the only method the audio thread
// calls; everything else is message thread.
class Engine {
public:
    static constexpr std::size_t kMaxTracks = 8;
    static constexpr std::size_t kCommandQueueCapacity = 256;
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
    [[nodiscard]] Timeline& timeline() noexcept { return timeline_; }

    // Message thread. Recompiles the arrangement at the transport's current
    // tempo. Returns false if the audio thread still holds the slot, in which
    // case the previously compiled schedule keeps playing.
    bool compileTimeline();

    // Message thread. Loop points are musical, so they survive a tempo change.
    void setLoopTicks(std::uint32_t startTick, std::uint32_t endTick) noexcept;

    // Message thread. Returns false if the queue is full.
    [[nodiscard]] bool pushCommand(const EngineCommand& command) noexcept;

    // Audio thread.
    void render(AudioBuffer& output) noexcept;

    [[nodiscard]] float peakLevel() const noexcept { return peakLevel_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t scheduledEventCount() const noexcept { return timeline_.scheduledEventCount(); }

private:
    void drainCommands() noexcept;
    void applyCommand(const EngineCommand& command) noexcept;
    void resolveTrackActivity() noexcept;

    // Positions the cursor at the first event at or after `position`. Called
    // whenever the playhead stops being where the cursor expects it: a new
    // schedule, a loop wrap, or a seek.
    void seekCursor(std::uint64_t position) noexcept;

    void fireDueEvents(std::uint64_t now) noexcept;
    [[nodiscard]] std::uint64_t nextEventTime() const noexcept;

    // Any jump in the playhead can strand a note whose note-off is now behind
    // the cursor, so the pool is released rather than left holding it.
    void releaseAllVoices() noexcept;

    std::array<Track, kMaxTracks> tracks_{};
    std::size_t numTracks_ = 0;
    std::array<bool, kMaxTracks> muted_{};
    std::array<bool, kMaxTracks> soloed_{};

    MasterBus master_;
    Graph graph_;
    Transport transport_;
    Timeline timeline_;

    RingBuffer<EngineCommand, kCommandQueueCapacity> commandQueue_;

    // Audio-thread cursor state over the published schedule.
    const Timeline::Schedule* activeSchedule_ = nullptr;
    std::size_t cursor_ = 0;
    bool cursorValid_ = false;

    std::atomic<float> peakLevel_{0.0f};
    bool prepared_ = false;
};

} // namespace daw
