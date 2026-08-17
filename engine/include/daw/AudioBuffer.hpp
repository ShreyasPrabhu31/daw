#pragma once

#include <cstddef>

namespace daw {

// Non-owning planar view over host-owned channel memory. Never allocates;
// the caller (CLI renderer, worklet, or a test) owns the backing storage.
class AudioBuffer {
public:
    AudioBuffer() noexcept = default;

    AudioBuffer(float* const* channels, std::size_t numChannels, std::size_t numFrames) noexcept
        : channels_(channels), numChannels_(numChannels), numFrames_(numFrames) {}

    [[nodiscard]] std::size_t numChannels() const noexcept { return numChannels_; }
    [[nodiscard]] std::size_t numFrames() const noexcept { return numFrames_; }

    [[nodiscard]] float* channel(std::size_t index) noexcept { return channels_[index]; }
    [[nodiscard]] const float* channel(std::size_t index) const noexcept { return channels_[index]; }

    void clear() noexcept {
        for (std::size_t ch = 0; ch < numChannels_; ++ch) {
            float* data = channels_[ch];
            for (std::size_t i = 0; i < numFrames_; ++i) {
                data[i] = 0.0f;
            }
        }
    }

private:
    float* const* channels_ = nullptr;
    std::size_t numChannels_ = 0;
    std::size_t numFrames_ = 0;
};

} // namespace daw
