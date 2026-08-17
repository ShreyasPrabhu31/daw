#pragma once

#include <cstddef>
#include <vector>

#include "daw/AudioBuffer.hpp"

namespace daw {

// One output buffer per graph node, all of it allocated once in prepare().
//
// The channel pointers are stored in their own flat array because AudioBuffer
// views a contiguous block of `float*`. Building that array up front means
// handing a node its buffer on the audio thread is pointer arithmetic, not
// an allocation.
class BufferPool {
public:
    void prepare(std::size_t numBuffers, std::size_t numChannels, std::size_t maxBlockSize) {
        numBuffers_ = numBuffers;
        numChannels_ = numChannels;
        maxBlockSize_ = maxBlockSize;

        storage_.assign(numBuffers * numChannels * maxBlockSize, 0.0f);
        channelPointers_.resize(numBuffers * numChannels);

        for (std::size_t buffer = 0; buffer < numBuffers; ++buffer) {
            for (std::size_t ch = 0; ch < numChannels; ++ch) {
                channelPointers_[buffer * numChannels + ch] =
                    storage_.data() + (buffer * numChannels + ch) * maxBlockSize;
            }
        }
    }

    [[nodiscard]] AudioBuffer view(std::size_t bufferIndex, std::size_t numFrames) noexcept {
        return AudioBuffer(channelPointers_.data() + bufferIndex * numChannels_, numChannels_, numFrames);
    }

    [[nodiscard]] std::size_t numBuffers() const noexcept { return numBuffers_; }
    [[nodiscard]] std::size_t numChannels() const noexcept { return numChannels_; }
    [[nodiscard]] std::size_t maxBlockSize() const noexcept { return maxBlockSize_; }

private:
    std::vector<float> storage_;
    std::vector<float*> channelPointers_;
    std::size_t numBuffers_ = 0;
    std::size_t numChannels_ = 0;
    std::size_t maxBlockSize_ = 0;
};

} // namespace daw
