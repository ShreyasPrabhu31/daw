#pragma once

#include "daw/AudioBuffer.hpp"
#include "daw/Node.hpp"
#include "daw/ParameterSmoother.hpp"
#include "daw/Synth.hpp"

namespace daw {

// One mixer channel: a synth followed by gain and pan.
//
// Pan gains are computed only when pan changes, never per sample. The
// per-sample cost of constant-power panning done naively is two
// transcendentals per frame per track, which is real money at 48 kHz.
class Track final : public Node {
public:
    void prepare(double sampleRate, std::size_t maxBlockSize) override;
    void process(AudioBuffer& buffer) noexcept override;

    [[nodiscard]] Synth& synth() noexcept { return synth_; }
    [[nodiscard]] const Synth& synth() const noexcept { return synth_; }

    void setGain(float gain) noexcept;
    void setPan(float pan) noexcept; // -1 hard left, 0 centre, +1 hard right

    // Resolved by the mixer from mute and solo across all tracks. Routed
    // through the same smoother as gain so muting never clicks.
    void setActive(bool active) noexcept;

    [[nodiscard]] float gain() const noexcept { return gain_; }
    [[nodiscard]] float pan() const noexcept { return pan_; }
    [[nodiscard]] bool isActive() const noexcept { return active_; }

private:
    void updateTargets() noexcept;

    Synth synth_;
    ParameterSmoother leftSmoother_;
    ParameterSmoother rightSmoother_;

    float gain_ = 0.8f;
    float pan_ = 0.0f;
    float panLeft_ = 0.70710678f; // cos(pi/4), centre
    float panRight_ = 0.70710678f;
    bool active_ = true;
};

} // namespace daw
