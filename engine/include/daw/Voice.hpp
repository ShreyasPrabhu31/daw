#pragma once

#include <cstdint>

#include "daw/ADSR.hpp"
#include "daw/AudioBuffer.hpp"
#include "daw/Biquad.hpp"
#include "daw/Oscillator.hpp"

namespace daw {

// One sounding note: oscillator into filter into amplitude envelope. Every
// voice in the pool is constructed once up front, so a voice is "allocated"
// by claiming an existing object, never by creating one.
class Voice {
public:
    void prepare(double sampleRate, std::size_t maxBlockSize);

    void noteOn(int midiNote, float frequencyHz, float velocity, std::uint64_t timestamp) noexcept;
    void noteOff() noexcept;
    void reset() noexcept;

    [[nodiscard]] bool isActive() const noexcept { return envelope_.isActive(); }
    [[nodiscard]] bool isReleasing() const noexcept { return envelope_.isReleasing(); }
    [[nodiscard]] int midiNote() const noexcept { return midiNote_; }
    [[nodiscard]] std::uint64_t timestamp() const noexcept { return timestamp_; }

    void setWaveform(Waveform waveform) noexcept { oscillator_.setWaveform(waveform); }
    void setEnvelopeParameters(const ADSR::Parameters& params) noexcept { envelope_.setParameters(params); }
    void setFilterType(Biquad::Type type) noexcept { filter_.setType(type); }
    void setFilterCutoff(float hz) noexcept { filter_.setCutoff(hz); }
    void setFilterResonance(float q) noexcept { filter_.setResonance(q); }

    // Sums into the buffer instead of overwriting it: voices are additive,
    // and the caller has already cleared the block.
    void renderAdd(AudioBuffer& buffer, float masterGain) noexcept;

private:
    Oscillator oscillator_;
    ADSR envelope_;
    Biquad filter_;
    int midiNote_ = -1;
    float velocity_ = 0.0f;
    std::uint64_t timestamp_ = 0;
};

} // namespace daw
