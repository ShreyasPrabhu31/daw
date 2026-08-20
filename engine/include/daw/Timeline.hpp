#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "daw/EngineCommand.hpp"
#include "daw/Published.hpp"

namespace daw {

// A note as the arrangement stores it: musical position, not samples.
//
// Ticks rather than samples so that changing the tempo is a recompile of the
// same data instead of the host having to resend every note at new positions.
// It also keeps the arrangement independent of sample rate, which is what lets
// the browser and an offline render at a different rate agree on the music.
struct Note {
    std::uint32_t startTick = 0;
    std::uint32_t lengthTicks = 0;
    std::uint8_t pitch = 60;
    std::uint8_t track = 0;
    float velocity = 0.85f;
};

// The arrangement, and the compiled schedule the audio thread actually reads.
//
// Editing happens on the message thread. compile() expands notes into sorted
// note-on/note-off events at absolute sample positions and publishes them; the
// audio thread walks that array with a cursor, so firing an event is a compare
// and an increment rather than a scan over every event in the song.
class Timeline {
public:
    static constexpr std::uint32_t kTicksPerQuarter = 960;
    static constexpr std::size_t kMaxNotes = 512;
    static constexpr std::size_t kMaxEvents = kMaxNotes * 2;

    struct Event {
        std::uint64_t time = 0; // absolute samples
        EngineCommand command{};
    };

    struct Schedule {
        std::array<Event, kMaxEvents> events{};
        std::size_t count = 0;
    };

    // Message thread. Allocates the published slots.
    void prepare();

    // --- editing, message thread ---
    void clear() noexcept { noteCount_ = 0; }
    bool addNote(const Note& note) noexcept;
    bool removeNote(std::size_t index) noexcept;
    void clearTrack(std::uint8_t track) noexcept;

    [[nodiscard]] std::size_t noteCount() const noexcept { return noteCount_; }
    [[nodiscard]] const Note& note(std::size_t index) const noexcept { return notes_[index]; }

    // Message thread. Returns false if the audio thread has not released the
    // slot yet, in which case the caller should retry; the running schedule is
    // left alone either way.
    bool compile(double sampleRate, double bpm);

    // --- audio thread ---
    [[nodiscard]] const Schedule* acquire() noexcept { return published_.acquire(); }

    [[nodiscard]] const Schedule* peek() const noexcept { return published_.peek(); }
    [[nodiscard]] std::size_t scheduledEventCount() const noexcept;

    [[nodiscard]] static std::uint64_t ticksToSamples(std::uint64_t ticks, double sampleRate,
                                                      double bpm) noexcept;

private:
    std::array<Note, kMaxNotes> notes_{};
    std::size_t noteCount_ = 0;
    Published<Schedule> published_;
};

} // namespace daw
