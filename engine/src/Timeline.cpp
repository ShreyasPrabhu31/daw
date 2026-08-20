#include "daw/Timeline.hpp"

#include <algorithm>

namespace daw {

void Timeline::prepare() {
    published_.prepare();
}

bool Timeline::addNote(const Note& note) noexcept {
    if (noteCount_ >= kMaxNotes) return false;
    if (note.lengthTicks == 0) return false; // a zero-length note would never release
    notes_[noteCount_++] = note;
    return true;
}

bool Timeline::removeNote(std::size_t index) noexcept {
    if (index >= noteCount_) return false;
    notes_[index] = notes_[noteCount_ - 1];
    --noteCount_;
    return true;
}

void Timeline::clearTrack(std::uint8_t track) noexcept {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < noteCount_; ++i) {
        if (notes_[i].track == track) continue;
        notes_[kept++] = notes_[i];
    }
    noteCount_ = kept;
}

std::uint64_t Timeline::ticksToSamples(std::uint64_t ticks, double sampleRate, double bpm) noexcept {
    if (bpm <= 0.0) return 0;
    const double secondsPerQuarter = 60.0 / bpm;
    const double seconds = static_cast<double>(ticks) / static_cast<double>(kTicksPerQuarter) * secondsPerQuarter;
    return static_cast<std::uint64_t>(seconds * sampleRate + 0.5);
}

bool Timeline::compile(double sampleRate, double bpm) {
    Schedule* staging = published_.beginEdit();
    if (staging == nullptr) return false;

    Schedule& schedule = *staging;
    schedule.count = 0;

    for (std::size_t i = 0; i < noteCount_ && schedule.count + 2 <= kMaxEvents; ++i) {
        const Note& source = notes_[i];

        EngineCommand on{};
        on.type = CommandType::NoteOn;
        on.floatValue = source.velocity;
        on.intValue = source.pitch;
        on.track = source.track;

        EngineCommand off{};
        off.type = CommandType::NoteOff;
        off.intValue = source.pitch;
        off.track = source.track;

        const std::uint64_t startTick = source.startTick;
        schedule.events[schedule.count++] = {ticksToSamples(startTick, sampleRate, bpm), on};
        schedule.events[schedule.count++] = {
            ticksToSamples(startTick + source.lengthTicks, sampleRate, bpm), off};
    }

    // std::sort rather than stable_sort: stable_sort allocates a temporary
    // buffer, and the ordering that actually matters is made explicit in the
    // comparator instead of being left to input order.
    std::sort(schedule.events.begin(), schedule.events.begin() + static_cast<std::ptrdiff_t>(schedule.count),
              [](const Event& a, const Event& b) {
                  if (a.time != b.time) return a.time < b.time;

                  // A note ending exactly where the next one begins has to
                  // release first. Run the note-on first and the note-off that
                  // follows it lands on the voice that just started, cutting
                  // the new note dead on repeated same-pitch notes.
                  const int aRank = a.command.type == CommandType::NoteOff ? 0 : 1;
                  const int bRank = b.command.type == CommandType::NoteOff ? 0 : 1;
                  return aRank < bRank;
              });

    published_.commit();
    return true;
}

std::size_t Timeline::scheduledEventCount() const noexcept {
    const Schedule* schedule = published_.peek();
    return schedule == nullptr ? 0 : schedule->count;
}

} // namespace daw
