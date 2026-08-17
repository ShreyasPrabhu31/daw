#pragma once

#include "daw/AudioBuffer.hpp"
#include "daw/Node.hpp"
#include "daw/ParameterSmoother.hpp"

namespace daw {

enum class Waveform { Sine, Saw, Square };

// PolyBLEP-corrected oscillator. Sine has no discontinuity so it needs no
// correction; saw and square get a polynomial patch at the discontinuity to
// suppress the aliasing a naive implementation would produce.
class Oscillator final : public Node {
public:
    void prepare(double sampleRate, std::size_t maxBlockSize) override;
    void process(AudioBuffer& buffer) noexcept override;

    void setFrequency(float hz) noexcept;
    void setWaveform(Waveform waveform) noexcept { waveform_ = waveform; }
    void setGain(float gain) noexcept;

    // Bypass the smoother. A voice starting a new note must land on the new
    // pitch immediately; ramping there would be portamento, not a note.
    void setFrequencyImmediate(float hz) noexcept;
    void setGainImmediate(float gain) noexcept;
    void resetPhase() noexcept { phase_ = 0.0; }

    // Exposed so a Voice can run its own per-sample chain (oscillator into
    // filter into envelope) and sum the result, rather than having the
    // oscillator overwrite the shared block.
    [[nodiscard]] float nextSample() noexcept;

private:
    [[nodiscard]] float polyBlep(float phase) const noexcept;

    double sampleRate_ = 48000.0;
    double phase_ = 0.0;
    double phaseIncrement_ = 0.0;
    Waveform waveform_ = Waveform::Sine;
    ParameterSmoother freqSmoother_;
    ParameterSmoother gainSmoother_;
};

} // namespace daw
