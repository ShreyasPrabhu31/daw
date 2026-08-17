#pragma once

#include <cstddef>

namespace daw {

// Non-owning planar view over host-owned channel memory. Never allocates;
// the caller (CLI renderer, worklet, or a test) owns the backing storage.
//
// The frame offset is what makes sample-accurate scheduling affordable: a
// sub-block view is produced by copying three scalars, so splitting a block
// at an event boundary costs nothing and needs no scratch pointer array.
class AudioBuffer {
public:
    AudioBuffer() noexcept = default;

    AudioBuffer(float* const* channels, std::size_t numChannels, std::size_t numFrames,
                std::size_t frameOffset = 0) noexcept
        : channels_(channels), numChannels_(numChannels), numFrames_(numFrames), frameOffset_(frameOffset) {}

    [[nodiscard]] std::size_t numChannels() const noexcept { return numChannels_; }
    [[nodiscard]] std::size_t numFrames() const noexcept { return numFrames_; }

    [[nodiscard]] float* channel(std::size_t index) noexcept { return channels_[index] + frameOffset_; }
    [[nodiscard]] const float* channel(std::size_t index) const noexcept { return channels_[index] + frameOffset_; }

    // A window into the same memory, measured from this view's own start.
    [[nodiscard]] AudioBuffer slice(std::size_t offset, std::size_t count) const noexcept {
        return AudioBuffer(channels_, numChannels_, count, frameOffset_ + offset);
    }

    void clear() noexcept {
        for (std::size_t ch = 0; ch < numChannels_; ++ch) {
            float* data = channel(ch);
            for (std::size_t i = 0; i < numFrames_; ++i) {
                data[i] = 0.0f;
            }
        }
    }

    // Sums another view of the same length into this one. This is how the
    // graph merges a node's inputs before running the node itself.
    void addFrom(const AudioBuffer& source) noexcept {
        const std::size_t channels = numChannels_ < source.numChannels_ ? numChannels_ : source.numChannels_;
        const std::size_t frames = numFrames_ < source.numFrames_ ? numFrames_ : source.numFrames_;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            float* destination = channel(ch);
            const float* input = source.channel(ch);
            for (std::size_t i = 0; i < frames; ++i) {
                destination[i] += input[i];
            }
        }
    }

    void copyFrom(const AudioBuffer& source) noexcept {
        const std::size_t channels = numChannels_ < source.numChannels_ ? numChannels_ : source.numChannels_;
        const std::size_t frames = numFrames_ < source.numFrames_ ? numFrames_ : source.numFrames_;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            float* destination = channel(ch);
            const float* input = source.channel(ch);
            for (std::size_t i = 0; i < frames; ++i) {
                destination[i] = input[i];
            }
        }
    }

private:
    float* const* channels_ = nullptr;
    std::size_t numChannels_ = 0;
    std::size_t numFrames_ = 0;
    std::size_t frameOffset_ = 0;
};

} // namespace daw
