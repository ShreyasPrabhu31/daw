#pragma once

#include <cmath>

namespace daw {

// One-pole exponential ramp toward a target value. Called once per sample
// from the audio thread so a knob move never produces a stair-step (zipper
// noise) in the output.
class ParameterSmoother {
public:
    void prepare(double sampleRate, float smoothingTimeMs) noexcept {
        const float timeConstantSeconds = smoothingTimeMs * 0.001f;
        coeff_ = timeConstantSeconds > 0.0f
                     ? std::exp(-1.0f / (static_cast<float>(sampleRate) * timeConstantSeconds))
                     : 0.0f;
    }

    void setTarget(float target) noexcept { target_ = target; }

    void reset(float value) noexcept {
        current_ = value;
        target_ = value;
    }

    [[nodiscard]] float next() noexcept {
        current_ = target_ + coeff_ * (current_ - target_);
        return current_;
    }

    [[nodiscard]] float current() const noexcept { return current_; }

private:
    float current_ = 0.0f;
    float target_ = 0.0f;
    float coeff_ = 0.0f;
};

} // namespace daw
