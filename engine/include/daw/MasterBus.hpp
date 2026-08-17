#pragma once

#include <atomic>
#include <cmath>

#include "daw/AudioBuffer.hpp"
#include "daw/Node.hpp"
#include "daw/ParameterSmoother.hpp"

namespace daw {

// Saturation with a smooth knee: linear below `knee`, tanh above it.
//
// The scaling makes the curve C1 continuous at the knee. The derivative of
// tanh at zero is one, and the (1 - knee) factors in the numerator and the
// argument cancel, so the slope is exactly one on both sides. That matters
// because a kink in the transfer curve is itself a source of harmonics: a
// clipper with a slope discontinuity sounds harsh precisely at the point it
// starts working. The output is bounded by 1 for any finite input, since
// tanh saturates at 1.
//
// This is saturation, not a limiter. There is no lookahead and no gain
// reduction envelope, so it colours the signal rather than transparently
// holding a ceiling.
[[nodiscard]] inline float softClip(float x, float knee = 0.6f) noexcept {
    const float magnitude = std::fabs(x);
    if (magnitude <= knee) return x;

    const float excess = (magnitude - knee) / (1.0f - knee);
    const float shaped = knee + (1.0f - knee) * std::tanh(excess);
    return std::copysign(shaped, x);
}

// Sums whatever the graph routed into it, applies master gain, and keeps the
// result inside the rails.
class MasterBus final : public Node {
public:
    void prepare(double sampleRate, std::size_t maxBlockSize) override;
    void process(AudioBuffer& buffer) noexcept override;

    void setGain(float gain) noexcept;
    void setSaturationEnabled(bool enabled) noexcept { saturate_ = enabled; }

    [[nodiscard]] float gain() const noexcept { return gain_; }
    [[nodiscard]] bool saturationEnabled() const noexcept { return saturate_; }

    // Post-saturation peak, published for the UI meter.
    [[nodiscard]] float peakLevel() const noexcept { return peak_.load(std::memory_order_relaxed); }

private:
    ParameterSmoother gainSmoother_;
    std::atomic<float> peak_{0.0f};
    float gain_ = 0.9f;
    bool saturate_ = true;
};

} // namespace daw
