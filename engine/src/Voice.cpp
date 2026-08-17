#include "daw/Voice.hpp"

namespace daw {

void Voice::prepare(double sampleRate, std::size_t maxBlockSize) {
    oscillator_.prepare(sampleRate, maxBlockSize);
    envelope_.prepare(sampleRate);
    filter_.prepare(sampleRate);
    reset();
}

void Voice::noteOn(int midiNote, float frequencyHz, float velocity, std::uint64_t timestamp) noexcept {
    midiNote_ = midiNote;
    velocity_ = velocity;
    timestamp_ = timestamp;

    // Jump straight to the new pitch. The smoother exists to stop knob moves
    // from zippering, not to glide between separate notes, so a fresh note
    // must not portamento up from whatever the stolen voice was playing.
    oscillator_.setFrequencyImmediate(frequencyHz);

    // Amplitude belongs to the envelope and velocity, so the oscillator's own
    // gain stage stays out of the way at unity.
    oscillator_.setGainImmediate(1.0f);
    oscillator_.resetPhase();

    // Filter state is deliberately left alone. Clearing it would put a step
    // in the signal when stealing a still-sounding voice, and the residue
    // from the previous note decays within a few samples anyway.
    envelope_.noteOn();
}

void Voice::noteOff() noexcept {
    envelope_.noteOff();
}

void Voice::reset() noexcept {
    envelope_.reset();
    filter_.reset();
    oscillator_.resetPhase();
    midiNote_ = -1;
    velocity_ = 0.0f;
    timestamp_ = 0;
}

void Voice::renderAdd(AudioBuffer& buffer, float masterGain) noexcept {
    if (!envelope_.isActive()) return;

    const std::size_t numFrames = buffer.numFrames();
    const std::size_t numChannels = buffer.numChannels();
    const float amplitude = velocity_ * masterGain;

    for (std::size_t i = 0; i < numFrames; ++i) {
        const float env = envelope_.next();
        const float sample = filter_.process(oscillator_.nextSample()) * env * amplitude;

        for (std::size_t ch = 0; ch < numChannels; ++ch) {
            buffer.channel(ch)[i] += sample;
        }

        // The envelope just reached zero, so every remaining frame in this
        // block would add silence.
        if (!envelope_.isActive()) break;
    }
}

} // namespace daw
