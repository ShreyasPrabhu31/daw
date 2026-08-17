#include "daw/Engine.hpp"

#include <algorithm>
#include <cmath>

namespace daw {

void Engine::prepare(double sampleRate, std::size_t maxBlockSize, std::size_t numChannels) {
    graph_.prepare(sampleRate, maxBlockSize, numChannels);
    transport_.prepare(sampleRate);

    const Graph::NodeId masterId = graph_.addNode(&master_);
    graph_.setOutputNode(masterId);
    (void)graph_.rebuild();

    prepared_ = true;
}

std::size_t Engine::addTrack() {
    if (!prepared_ || numTracks_ >= kMaxTracks) return kMaxTracks;

    const std::size_t index = numTracks_;
    Track& newTrack = tracks_[index];

    const Graph::NodeId id = graph_.addNode(&newTrack);
    if (id == Graph::kInvalidNode) return kMaxTracks;

    if (!graph_.connect(id, graph_.outputNode())) return kMaxTracks;

    ++numTracks_;
    muted_[index] = false;
    soloed_[index] = false;
    resolveTrackActivity();

    // Publishing is what actually makes the new track audible; until the plan
    // is republished the audio thread keeps walking the previous one.
    if (!graph_.rebuild()) {
        --numTracks_;
        (void)graph_.disconnect(id, graph_.outputNode());
        return kMaxTracks;
    }

    return index;
}

bool Engine::pushCommand(const EngineCommand& command) noexcept {
    return commandQueue_.push(command);
}

// Mute is per track; solo is relative to every other track. Resolving both
// into a single audible flag keeps that logic off the audio thread's inner
// loop, where it would otherwise be a branch per sample.
void Engine::resolveTrackActivity() noexcept {
    bool anySolo = false;
    for (std::size_t i = 0; i < numTracks_; ++i) {
        if (soloed_[i]) anySolo = true;
    }

    for (std::size_t i = 0; i < numTracks_; ++i) {
        const bool audible = anySolo ? (soloed_[i] && !muted_[i]) : !muted_[i];
        tracks_[i].setActive(audible);
    }
}

void Engine::schedule(const EngineCommand& command) noexcept {
    for (ScheduledEvent& event : scheduled_) {
        if (event.occupied) continue;
        event.command = command;
        event.occupied = true;
        event.fired = false;
        return;
    }
    // Table full. Dropping the newest event is the honest failure: silently
    // evicting an older one would make the timeline change under the user.
}

void Engine::drainCommands() noexcept {
    EngineCommand command{};
    while (commandQueue_.pop(command)) {
        if (command.scheduled) {
            schedule(command);
        } else {
            applyCommand(command);
        }
    }
}

void Engine::applyCommand(const EngineCommand& command) noexcept {
    const std::size_t index = command.track;
    const bool hasTrack = index < numTracks_;

    switch (command.type) {
        case CommandType::NoteOn:
            if (hasTrack) tracks_[index].synth().noteOn(command.intValue, command.floatValue);
            break;
        case CommandType::NoteOff:
            if (hasTrack) tracks_[index].synth().noteOff(command.intValue);
            break;
        case CommandType::AllNotesOff:
            for (std::size_t i = 0; i < numTracks_; ++i) tracks_[i].synth().allNotesOff();
            break;
        case CommandType::SetWaveform:
            if (hasTrack) tracks_[index].synth().setWaveform(static_cast<Waveform>(command.intValue));
            break;
        case CommandType::SetSynthGain:
            if (hasTrack) tracks_[index].synth().setMasterGain(command.floatValue);
            break;

        case CommandType::SetAttack:
        case CommandType::SetDecay:
        case CommandType::SetSustain:
        case CommandType::SetRelease: {
            if (!hasTrack) break;
            Synth& synth = tracks_[index].synth();
            ADSR::Parameters envelope = synth.envelopeParameters();
            if (command.type == CommandType::SetAttack) envelope.attackMs = command.floatValue;
            if (command.type == CommandType::SetDecay) envelope.decayMs = command.floatValue;
            if (command.type == CommandType::SetSustain) envelope.sustain = command.floatValue;
            if (command.type == CommandType::SetRelease) envelope.releaseMs = command.floatValue;
            synth.setEnvelopeParameters(envelope);
            break;
        }

        case CommandType::SetFilterType:
            if (hasTrack) tracks_[index].synth().setFilterType(static_cast<Biquad::Type>(command.intValue));
            break;
        case CommandType::SetFilterCutoff:
            if (hasTrack) tracks_[index].synth().setFilterCutoff(command.floatValue);
            break;
        case CommandType::SetFilterResonance:
            if (hasTrack) tracks_[index].synth().setFilterResonance(command.floatValue);
            break;

        case CommandType::SetTrackGain:
            if (hasTrack) tracks_[index].setGain(command.floatValue);
            break;
        case CommandType::SetTrackPan:
            if (hasTrack) tracks_[index].setPan(command.floatValue);
            break;
        case CommandType::SetTrackMute:
            if (hasTrack) {
                muted_[index] = command.intValue != 0;
                resolveTrackActivity();
            }
            break;
        case CommandType::SetTrackSolo:
            if (hasTrack) {
                soloed_[index] = command.intValue != 0;
                resolveTrackActivity();
            }
            break;

        case CommandType::SetMasterGain:
            master_.setGain(command.floatValue);
            break;
        case CommandType::SetMasterSaturation:
            master_.setSaturationEnabled(command.intValue != 0);
            break;

        case CommandType::TransportPlay:
            transport_.play();
            break;
        case CommandType::TransportStop:
            transport_.stop();
            for (std::size_t i = 0; i < numTracks_; ++i) tracks_[i].synth().allNotesOff();
            break;
        case CommandType::TransportSetPosition:
            transport_.setPosition(static_cast<std::uint64_t>(std::max(command.intValue, 0)));
            break;
        case CommandType::TransportSetLoop:
            transport_.setLoopEnabled(command.intValue != 0);
            break;
        case CommandType::ClearScheduledEvents:
            for (ScheduledEvent& event : scheduled_) {
                event.occupied = false;
                event.fired = false;
            }
            break;
    }
}

void Engine::fireDueEvents(std::uint64_t now) noexcept {
    for (ScheduledEvent& event : scheduled_) {
        if (!event.occupied || event.fired) continue;
        if (event.command.time > now) continue;
        event.fired = true;
        applyCommand(event.command);
    }
}

std::uint64_t Engine::nextEventTime(std::uint64_t after) const noexcept {
    std::uint64_t soonest = kNever;
    for (const ScheduledEvent& event : scheduled_) {
        if (!event.occupied || event.fired) continue;
        if (event.command.time <= after) continue;
        soonest = std::min(soonest, event.command.time);
    }
    return soonest;
}

void Engine::rearmLoopedEvents() noexcept {
    const std::uint64_t start = transport_.loopStart();
    const std::uint64_t end = transport_.loopEnd();

    for (ScheduledEvent& event : scheduled_) {
        if (!event.occupied) continue;
        const std::uint64_t time = event.command.time;
        if (time >= start && time < end) event.fired = false;
    }
}

std::size_t Engine::scheduledEventCount() const noexcept {
    std::size_t count = 0;
    for (const ScheduledEvent& event : scheduled_) {
        if (event.occupied) ++count;
    }
    return count;
}

void Engine::render(AudioBuffer& output) noexcept {
    if (!prepared_) {
        output.clear();
        return;
    }

    drainCommands();

    const std::size_t frames = output.numFrames();
    std::size_t offset = 0;

    // The block is walked in chunks that end at the next thing which must
    // land on an exact sample: a scheduled event, or the loop point. When
    // nothing is pending this is a single pass and costs one extra compare.
    while (offset < frames) {
        const std::uint64_t now = transport_.position();
        fireDueEvents(now);

        std::size_t chunk = frames - offset;

        if (transport_.isPlaying()) {
            const std::uint64_t next = nextEventTime(now);
            if (next != kNever) {
                const std::uint64_t untilEvent = next - now;
                if (untilEvent < chunk) chunk = static_cast<std::size_t>(untilEvent);
            }

            const std::uint64_t untilLoop = transport_.framesUntilLoopEnd();
            if (untilLoop > 0 && untilLoop < chunk) chunk = static_cast<std::size_t>(untilLoop);
        }

        if (chunk == 0) chunk = 1; // never stall, even on a degenerate loop range

        AudioBuffer slice = output.slice(offset, chunk);
        graph_.process(slice);

        const std::uint64_t before = transport_.position();
        transport_.advance(chunk);
        if (transport_.position() < before) rearmLoopedEvents();

        offset += chunk;
    }

    // The master bus already saturated; this is the last line of defence
    // against a node added downstream of it or a NaN leaking through.
    float peak = 0.0f;
    for (std::size_t ch = 0; ch < output.numChannels(); ++ch) {
        float* data = output.channel(ch);
        for (std::size_t i = 0; i < frames; ++i) {
            const float clamped = std::clamp(data[i], -1.0f, 1.0f);
            data[i] = clamped;
            peak = std::max(peak, std::fabs(clamped));
        }
    }
    peakLevel_.store(peak, std::memory_order_relaxed);
}

} // namespace daw
