#pragma once

#include <algorithm>

namespace daw {

// Linear-segment ADSR.
//
// Linear rather than exponential is deliberate. An exponential release
// approaches zero asymptotically and never actually arrives, so "has this
// voice finished?" degrades into a threshold guess, and a guess that is too
// tight leaks voices out of a fixed pool. A linear ramp reaches exactly zero
// on a known sample, which is what lets the voice pool reclaim a voice
// deterministically.
class ADSR {
public:
    struct Parameters {
        float attackMs = 5.0f;
        float decayMs = 120.0f;
        float sustain = 0.7f;
        float releaseMs = 250.0f;
    };

    void prepare(double sampleRate) noexcept {
        sampleRate_ = sampleRate;
        recalculate();
    }

    void setParameters(const Parameters& params) noexcept {
        params_ = params;
        params_.sustain = std::clamp(params_.sustain, 0.0f, 1.0f);
        recalculate();
    }

    [[nodiscard]] const Parameters& parameters() const noexcept { return params_; }

    // Deliberately does not zero level_. Retriggering a voice that is still
    // sounding (the voice-stealing case) ramps from wherever the envelope
    // currently sits; snapping to zero first would put a step discontinuity
    // in the output, which is exactly the click stealing is meant to avoid.
    void noteOn() noexcept { stage_ = Stage::Attack; }

    void noteOff() noexcept {
        if (stage_ == Stage::Idle) return;
        stage_ = Stage::Release;
        // Recomputed from the level actually reached so that release time
        // stays constant whether the key was let go during attack or after
        // an hour of sustain.
        releaseStep_ = stepFor(params_.releaseMs, level_);
    }

    void reset() noexcept {
        stage_ = Stage::Idle;
        level_ = 0.0f;
    }

    [[nodiscard]] float next() noexcept {
        switch (stage_) {
            case Stage::Idle:
                return 0.0f;
            case Stage::Attack:
                level_ += attackStep_;
                if (level_ >= 1.0f) {
                    level_ = 1.0f;
                    stage_ = Stage::Decay;
                }
                break;
            case Stage::Decay:
                level_ -= decayStep_;
                if (level_ <= params_.sustain) {
                    level_ = params_.sustain;
                    // A patch with zero sustain is percussive: the note is
                    // over once decay lands, so free the voice instead of
                    // holding it silent until the key is released.
                    stage_ = params_.sustain > 0.0f ? Stage::Sustain : Stage::Idle;
                }
                break;
            case Stage::Sustain:
                level_ = params_.sustain;
                break;
            case Stage::Release:
                level_ -= releaseStep_;
                if (level_ <= 0.0f) {
                    level_ = 0.0f;
                    stage_ = Stage::Idle;
                }
                break;
        }
        return level_;
    }

    [[nodiscard]] bool isActive() const noexcept { return stage_ != Stage::Idle; }
    [[nodiscard]] bool isReleasing() const noexcept { return stage_ == Stage::Release; }
    [[nodiscard]] float level() const noexcept { return level_; }

private:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    [[nodiscard]] float stepFor(float milliseconds, float range) const noexcept {
        const double samples = static_cast<double>(milliseconds) * 0.001 * sampleRate_;
        if (samples <= 1.0) return range; // zero-length segment: cross it in one sample
        return range / static_cast<float>(samples);
    }

    void recalculate() noexcept {
        attackStep_ = stepFor(params_.attackMs, 1.0f);
        decayStep_ = stepFor(params_.decayMs, 1.0f - params_.sustain);
    }

    double sampleRate_ = 48000.0;
    Parameters params_{};
    Stage stage_ = Stage::Idle;
    float level_ = 0.0f;
    float attackStep_ = 0.0f;
    float decayStep_ = 0.0f;
    float releaseStep_ = 0.0f;
};

} // namespace daw
