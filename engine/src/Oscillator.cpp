#include "daw/Oscillator.hpp"

#include <cmath>

namespace daw {

namespace {
constexpr double kTwoPi = 6.283185307179586476925286766559;
}

void Oscillator::prepare(double sampleRate, std::size_t /*maxBlockSize*/) {
    sampleRate_ = sampleRate;
    freqSmoother_.prepare(sampleRate, 5.0f);
    gainSmoother_.prepare(sampleRate, 5.0f);
    freqSmoother_.reset(440.0f);
    gainSmoother_.reset(1.0f);
}

void Oscillator::setFrequency(float hz) noexcept {
    freqSmoother_.setTarget(hz);
}

void Oscillator::setGain(float gain) noexcept {
    gainSmoother_.setTarget(gain);
}

void Oscillator::setFrequencyImmediate(float hz) noexcept {
    freqSmoother_.reset(hz);
}

void Oscillator::setGainImmediate(float gain) noexcept {
    gainSmoother_.reset(gain);
}

// Bandlimited step correction applied at the discontinuity of a naive saw or
// square wave, evaluated over a window of one sample period on either side
// of the discontinuity.
float Oscillator::polyBlep(float phase) const noexcept {
    const float dt = static_cast<float>(phaseIncrement_);
    if (dt <= 0.0f) return 0.0f;

    if (phase < dt) {
        const float x = phase / dt;
        return x + x - x * x - 1.0f;
    }
    if (phase > 1.0f - dt) {
        const float x = (phase - 1.0f) / dt;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

float Oscillator::nextSample() noexcept {
    const float freq = freqSmoother_.next();
    phaseIncrement_ = static_cast<double>(freq) / sampleRate_;

    const float t = static_cast<float>(phase_);
    float sample = 0.0f;

    switch (waveform_) {
        case Waveform::Sine:
            sample = static_cast<float>(std::sin(phase_ * kTwoPi));
            break;
        case Waveform::Saw:
            sample = static_cast<float>(2.0 * phase_ - 1.0);
            sample -= polyBlep(t);
            break;
        case Waveform::Square: {
            sample = phase_ < 0.5 ? 1.0f : -1.0f;
            sample += polyBlep(t);
            const float shifted = static_cast<float>(std::fmod(phase_ + 0.5, 1.0));
            sample -= polyBlep(shifted);
            break;
        }
    }

    phase_ += phaseIncrement_;
    if (phase_ >= 1.0) phase_ -= 1.0;

    return sample * gainSmoother_.next();
}

void Oscillator::process(AudioBuffer& buffer) noexcept {
    const std::size_t numFrames = buffer.numFrames();
    const std::size_t numChannels = buffer.numChannels();

    for (std::size_t i = 0; i < numFrames; ++i) {
        const float sample = nextSample();
        for (std::size_t ch = 0; ch < numChannels; ++ch) {
            buffer.channel(ch)[i] = sample;
        }
    }
}

} // namespace daw
