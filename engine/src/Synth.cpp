#include "daw/Synth.hpp"

#include <cmath>

namespace daw {

void Synth::prepare(double sampleRate, std::size_t maxBlockSize) {
    for (std::size_t i = 0; i < noteFrequencies_.size(); ++i) {
        const double semitonesFromA440 = static_cast<double>(i) - 69.0;
        noteFrequencies_[i] = static_cast<float>(440.0 * std::pow(2.0, semitonesFromA440 / 12.0));
    }

    for (Voice& voice : voices_) {
        voice.prepare(sampleRate, maxBlockSize);
        voice.setEnvelopeParameters(envelopeParams_);
    }
}

// Preference order is about what a listener notices. Reusing a silent voice
// costs nothing; cutting off a voice that is already fading out is nearly
// inaudible; interrupting a held note is the last resort.
Voice* Synth::allocateVoice() noexcept {
    for (Voice& voice : voices_) {
        if (!voice.isActive()) return &voice;
    }

    Voice* oldestReleasing = nullptr;
    for (Voice& voice : voices_) {
        if (!voice.isReleasing()) continue;
        if (oldestReleasing == nullptr || voice.timestamp() < oldestReleasing->timestamp()) {
            oldestReleasing = &voice;
        }
    }
    if (oldestReleasing != nullptr) return oldestReleasing;

    Voice* oldest = &voices_[0];
    for (Voice& voice : voices_) {
        if (voice.timestamp() < oldest->timestamp()) oldest = &voice;
    }
    return oldest;
}

void Synth::noteOn(int midiNote, float velocity) noexcept {
    if (midiNote < 0 || midiNote > 127) return;

    Voice* voice = allocateVoice();
    voice->noteOn(midiNote, noteFrequencies_[static_cast<std::size_t>(midiNote)], velocity, nextTimestamp_++);
}

void Synth::noteOff(int midiNote) noexcept {
    // Releases every voice holding this note, not just the first. The same
    // note can be sounding twice after a steal, and leaving one held would
    // strand it until the pool wrapped around to steal it back.
    for (Voice& voice : voices_) {
        if (voice.isActive() && !voice.isReleasing() && voice.midiNote() == midiNote) {
            voice.noteOff();
        }
    }
}

void Synth::allNotesOff() noexcept {
    for (Voice& voice : voices_) {
        voice.reset();
    }
}

void Synth::setWaveform(Waveform waveform) noexcept {
    for (Voice& voice : voices_) voice.setWaveform(waveform);
}

void Synth::setEnvelopeParameters(const ADSR::Parameters& params) noexcept {
    envelopeParams_ = params;
    for (Voice& voice : voices_) voice.setEnvelopeParameters(params);
}

void Synth::setFilterType(Biquad::Type type) noexcept {
    for (Voice& voice : voices_) voice.setFilterType(type);
}

void Synth::setFilterCutoff(float hz) noexcept {
    for (Voice& voice : voices_) voice.setFilterCutoff(hz);
}

void Synth::setFilterResonance(float q) noexcept {
    for (Voice& voice : voices_) voice.setFilterResonance(q);
}

std::size_t Synth::activeVoiceCount() const noexcept {
    std::size_t count = 0;
    for (const Voice& voice : voices_) {
        if (voice.isActive()) ++count;
    }
    return count;
}

void Synth::process(AudioBuffer& buffer) noexcept {
    for (Voice& voice : voices_) {
        voice.renderAdd(buffer, masterGain_);
    }
}

} // namespace daw
