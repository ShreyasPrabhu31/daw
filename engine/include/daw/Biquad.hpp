#pragma once

#include <algorithm>
#include <cmath>

namespace daw {

// RBJ cookbook biquad in transposed direct form II.
//
// Coefficients are recomputed only when a parameter actually changes, which
// on the audio thread means at most once per block when the command queue is
// drained. The sin/cos pair costs far more than the four multiply-adds that
// process() does per sample, so it must never end up inside the sample loop.
class Biquad {
public:
    enum class Type { LowPass, HighPass, BandPass };

    void prepare(double sampleRate) noexcept {
        sampleRate_ = sampleRate;
        reset();
        updateCoefficients();
    }

    void reset() noexcept {
        z1_ = 0.0f;
        z2_ = 0.0f;
    }

    void setType(Type type) noexcept {
        type_ = type;
        updateCoefficients();
    }

    void setCutoff(float hz) noexcept {
        cutoffHz_ = hz;
        updateCoefficients();
    }

    void setResonance(float q) noexcept {
        resonance_ = q;
        updateCoefficients();
    }

    [[nodiscard]] float process(float input) noexcept {
        const float output = b0_ * input + z1_;
        z1_ = b1_ * input - a1_ * output + z2_;
        z2_ = b2_ * input - a2_ * output;
        return output;
    }

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] float cutoff() const noexcept { return cutoffHz_; }
    [[nodiscard]] float resonance() const noexcept { return resonance_; }

private:
    static constexpr float kPi = 3.14159265358979323846f;

    void updateCoefficients() noexcept {
        // These clamps are stability, not taste. A cutoff at or above Nyquist
        // makes the bilinear transform blow up, and a Q of zero divides by
        // zero in alpha.
        const float nyquist = static_cast<float>(sampleRate_ * 0.5);
        const float f0 = std::clamp(cutoffHz_, 20.0f, nyquist * 0.99f);
        const float q = std::clamp(resonance_, 0.05f, 20.0f);

        const float w0 = 2.0f * kPi * f0 / static_cast<float>(sampleRate_);
        const float cosW0 = std::cos(w0);
        const float sinW0 = std::sin(w0);
        const float alpha = sinW0 / (2.0f * q);

        float numer0 = 0.0f;
        float numer1 = 0.0f;
        float numer2 = 0.0f;

        switch (type_) {
            case Type::LowPass:
                numer0 = (1.0f - cosW0) * 0.5f;
                numer1 = 1.0f - cosW0;
                numer2 = numer0;
                break;
            case Type::HighPass:
                numer0 = (1.0f + cosW0) * 0.5f;
                numer1 = -(1.0f + cosW0);
                numer2 = numer0;
                break;
            case Type::BandPass:
                numer0 = alpha;
                numer1 = 0.0f;
                numer2 = -alpha;
                break;
        }

        const float denom0 = 1.0f + alpha;
        const float denom1 = -2.0f * cosW0;
        const float denom2 = 1.0f - alpha;

        const float inv = 1.0f / denom0;
        b0_ = numer0 * inv;
        b1_ = numer1 * inv;
        b2_ = numer2 * inv;
        a1_ = denom1 * inv;
        a2_ = denom2 * inv;
    }

    double sampleRate_ = 48000.0;
    Type type_ = Type::LowPass;
    float cutoffHz_ = 12000.0f;
    float resonance_ = 0.707f;

    float b0_ = 1.0f;
    float b1_ = 0.0f;
    float b2_ = 0.0f;
    float a1_ = 0.0f;
    float a2_ = 0.0f;

    float z1_ = 0.0f;
    float z2_ = 0.0f;
};

} // namespace daw
