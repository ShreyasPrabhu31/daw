#include "daw/MasterBus.hpp"

#include <algorithm>

namespace daw {

void MasterBus::prepare(double sampleRate, std::size_t /*maxBlockSize*/) {
    gainSmoother_.prepare(sampleRate, 8.0f);
    gainSmoother_.reset(gain_);
    peak_.store(0.0f, std::memory_order_relaxed);
}

void MasterBus::setGain(float gain) noexcept {
    gain_ = std::clamp(gain, 0.0f, 2.0f);
    gainSmoother_.setTarget(gain_);
}

void MasterBus::process(AudioBuffer& buffer) noexcept {
    const std::size_t frames = buffer.numFrames();
    const std::size_t channels = buffer.numChannels();

    float peak = 0.0f;

    for (std::size_t i = 0; i < frames; ++i) {
        // One smoother step per frame, shared across channels, so the two
        // sides cannot drift apart and shift the stereo image.
        const float gain = gainSmoother_.next();

        for (std::size_t ch = 0; ch < channels; ++ch) {
            float* data = buffer.channel(ch);
            const float scaled = data[i] * gain;
            const float shaped = saturate_ ? softClip(scaled) : std::clamp(scaled, -1.0f, 1.0f);
            data[i] = shaped;
            peak = std::max(peak, std::fabs(shaped));
        }
    }

    peak_.store(peak, std::memory_order_relaxed);
}

} // namespace daw
