#include "daw/Engine.hpp"

#include <algorithm>
#include <cmath>

namespace daw {

void Engine::prepare(double sampleRate, std::size_t maxBlockSize) {
    synth_.prepare(sampleRate, maxBlockSize);
    transport_.prepare(sampleRate);
    prepared_ = true;
}

bool Engine::addNode(Node* node) noexcept {
    if (numNodes_ >= kMaxNodes) return false;
    nodes_[numNodes_++] = node;
    return true;
}

bool Engine::pushCommand(const EngineCommand& command) noexcept {
    return commandQueue_.push(command);
}

void Engine::applyCommand(const EngineCommand& command) noexcept {
    // The envelope is set as a whole struct, so a single-field change reads
    // the current parameters back out of the synth and rewrites one member.
    ADSR::Parameters envelope = synth_.envelopeParameters();

    switch (command.type) {
        case CommandType::NoteOn:
            synth_.noteOn(command.intValue, command.floatValue);
            break;
        case CommandType::NoteOff:
            synth_.noteOff(command.intValue);
            break;
        case CommandType::AllNotesOff:
            synth_.allNotesOff();
            break;
        case CommandType::SetWaveform:
            synth_.setWaveform(static_cast<Waveform>(command.intValue));
            break;
        case CommandType::SetMasterGain:
            synth_.setMasterGain(command.floatValue);
            break;
        case CommandType::SetAttack:
            envelope.attackMs = command.floatValue;
            synth_.setEnvelopeParameters(envelope);
            break;
        case CommandType::SetDecay:
            envelope.decayMs = command.floatValue;
            synth_.setEnvelopeParameters(envelope);
            break;
        case CommandType::SetSustain:
            envelope.sustain = command.floatValue;
            synth_.setEnvelopeParameters(envelope);
            break;
        case CommandType::SetRelease:
            envelope.releaseMs = command.floatValue;
            synth_.setEnvelopeParameters(envelope);
            break;
        case CommandType::SetFilterType:
            synth_.setFilterType(static_cast<Biquad::Type>(command.intValue));
            break;
        case CommandType::SetFilterCutoff:
            synth_.setFilterCutoff(command.floatValue);
            break;
        case CommandType::SetFilterResonance:
            synth_.setFilterResonance(command.floatValue);
            break;
        case CommandType::TransportPlay:
            transport_.play();
            break;
        case CommandType::TransportStop:
            transport_.stop();
            break;
    }
}

void Engine::drainCommands() noexcept {
    EngineCommand command{};
    while (commandQueue_.pop(command)) {
        applyCommand(command);
    }
}

void Engine::render(AudioBuffer& buffer) noexcept {
    buffer.clear();

    if (!prepared_) return;

    drainCommands();

    synth_.process(buffer);
    for (std::size_t i = 0; i < numNodes_; ++i) {
        nodes_[i]->process(buffer);
    }

    transport_.advance(buffer.numFrames());

    float peak = 0.0f;
    for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch) {
        float* data = buffer.channel(ch);
        for (std::size_t i = 0; i < buffer.numFrames(); ++i) {
            const float clamped = std::clamp(data[i], -1.0f, 1.0f);
            data[i] = clamped;
            peak = std::max(peak, std::fabs(clamped));
        }
    }
    peakLevel_.store(peak, std::memory_order_relaxed);
}

} // namespace daw
