#pragma once

#include <cstdint>

namespace daw {

// Sample-accurate playhead. advance() is called once per block from the
// audio thread after rendering; it never allocates or blocks.
class Transport {
public:
    void prepare(double sampleRate) noexcept { sampleRate_ = sampleRate; }

    void advance(std::size_t numFrames) noexcept {
        if (!playing_) return;
        position_ += numFrames;
        if (loopEnabled_ && loopEnd_ > loopStart_ && position_ >= loopEnd_) {
            const std::uint64_t length = loopEnd_ - loopStart_;
            position_ = loopStart_ + ((position_ - loopStart_) % length);
        }
    }

    void play() noexcept { playing_ = true; }
    void stop() noexcept { playing_ = false; }
    [[nodiscard]] bool isPlaying() const noexcept { return playing_; }

    void setPosition(std::uint64_t samples) noexcept { position_ = samples; }
    [[nodiscard]] std::uint64_t position() const noexcept { return position_; }

    void setTempo(double bpm) noexcept { tempoBpm_ = bpm; }
    [[nodiscard]] double tempo() const noexcept { return tempoBpm_; }

    void setLoop(std::uint64_t startSamples, std::uint64_t endSamples) noexcept {
        loopStart_ = startSamples;
        loopEnd_ = endSamples;
    }
    void setLoopEnabled(bool enabled) noexcept { loopEnabled_ = enabled; }
    [[nodiscard]] bool loopEnabled() const noexcept { return loopEnabled_; }

    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

private:
    double sampleRate_ = 48000.0;
    double tempoBpm_ = 120.0;
    std::uint64_t position_ = 0;
    std::uint64_t loopStart_ = 0;
    std::uint64_t loopEnd_ = 0;
    bool loopEnabled_ = false;
    bool playing_ = false;
};

} // namespace daw
