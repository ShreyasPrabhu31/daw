#pragma once

#include <array>
#include <cstdint>

#include "daw/Node.hpp"
#include "daw/Voice.hpp"

namespace daw {

// Fixed pool of voices summed into one block. The pool is a plain array of
// fully constructed Voices: note-on claims one, note-off releases it, and
// nothing is ever created or destroyed on the audio thread.
class Synth final : public Node {
public:
    static constexpr std::size_t kNumVoices = 8;

    void prepare(double sampleRate, std::size_t maxBlockSize) override;
    void process(AudioBuffer& buffer) noexcept override;

    void noteOn(int midiNote, float velocity) noexcept;
    void noteOff(int midiNote) noexcept;
    void allNotesOff() noexcept;

    void setWaveform(Waveform waveform) noexcept;
    void setEnvelopeParameters(const ADSR::Parameters& params) noexcept;
    void setFilterType(Biquad::Type type) noexcept;
    void setFilterCutoff(float hz) noexcept;
    void setFilterResonance(float q) noexcept;
    void setMasterGain(float gain) noexcept { masterGain_ = gain; }

    [[nodiscard]] const ADSR::Parameters& envelopeParameters() const noexcept { return envelopeParams_; }
    [[nodiscard]] std::size_t activeVoiceCount() const noexcept;

    // Read-only view of the pool, for tests now and a voice-activity meter
    // in Phase 3.
    [[nodiscard]] const Voice& voice(std::size_t index) const noexcept { return voices_[index]; }
    [[nodiscard]] float masterGain() const noexcept { return masterGain_; }

private:
    [[nodiscard]] Voice* allocateVoice() noexcept;

    std::array<Voice, kNumVoices> voices_{};

    // MIDI note to Hz is a pow() per note-on otherwise. 128 floats is
    // cheaper than doing that on the audio thread, and it is exact.
    std::array<float, 128> noteFrequencies_{};

    ADSR::Parameters envelopeParams_{};

    // Eight voices at unity would sum to 8.0 and slam the output clamp, so
    // the default leaves headroom for a four-note chord at full velocity.
    float masterGain_ = 0.25f;

    // Monotonic, so "oldest" is a comparison rather than a wall-clock read.
    std::uint64_t nextTimestamp_ = 1;
};

} // namespace daw
