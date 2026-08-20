#include "daw/Track.hpp"

#include <algorithm>
#include <cmath>

namespace daw {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

void Track::prepare(double sampleRate, std::size_t maxBlockSize) {
    synth_.prepare(sampleRate, maxBlockSize);

    leftSmoother_.prepare(sampleRate, 8.0f);
    rightSmoother_.prepare(sampleRate, 8.0f);

    setPan(pan_);
    leftSmoother_.reset(gain_ * panLeft_);
    rightSmoother_.reset(gain_ * panRight_);
}

void Track::setGain(float gain) noexcept {
    gain_ = std::clamp(gain, 0.0f, 2.0f);
    updateTargets();
}

void Track::setPan(float pan) noexcept {
    pan_ = std::clamp(pan, -1.0f, 1.0f);

    // Constant power: the two gains square-sum to one, so sweeping across the
    // image keeps perceived loudness steady instead of dipping in the middle
    // the way a linear crossfade does.
    const float angle = (pan_ + 1.0f) * 0.25f * kPi;
    panLeft_ = std::cos(angle);
    panRight_ = std::sin(angle);
    updateTargets();
}

void Track::setActive(bool active) noexcept {
    active_ = active;
    updateTargets();
}

void Track::updateTargets() noexcept {
    const float effective = active_ ? gain_ : 0.0f;
    leftSmoother_.setTarget(effective * panLeft_);
    rightSmoother_.setTarget(effective * panRight_);
}

void Track::process(AudioBuffer& buffer) noexcept {
    // The graph hands over a cleared buffer for a node with no inputs, so the
    // synth summing its voices in is the same as writing them.
    synth_.process(buffer);

    const std::size_t frames = buffer.numFrames();
    const std::size_t channels = buffer.numChannels();
    if (channels == 0) return;

    // Decay the previous reading rather than starting from zero, so a peak
    // that happened between two UI paints still shows up as a falling meter
    // instead of vanishing.
    float peak = peak_.load(std::memory_order_relaxed) * decayPerBlock_;

    if (channels == 1) {
        float* mono = buffer.channel(0);
        for (std::size_t i = 0; i < frames; ++i) {
            // Mono output collapses the pair, otherwise a hard pan would
            // silence a mono render entirely.
            mono[i] *= leftSmoother_.next() + rightSmoother_.next();
            peak = std::max(peak, std::fabs(mono[i]));
        }
        peak_.store(peak, std::memory_order_relaxed);
        return;
    }

    float* left = buffer.channel(0);
    float* right = buffer.channel(1);
    for (std::size_t i = 0; i < frames; ++i) {
        left[i] *= leftSmoother_.next();
        right[i] *= rightSmoother_.next();
        peak = std::max(peak, std::max(std::fabs(left[i]), std::fabs(right[i])));
    }

    peak_.store(peak, std::memory_order_relaxed);
}

} // namespace daw
