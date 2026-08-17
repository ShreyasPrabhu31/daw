#include "daw/Engine.hpp"

#include <algorithm>
#include <cmath>

namespace daw {

void Engine::prepare(double sampleRate, std::size_t maxBlockSize) {
    oscillator_.prepare(sampleRate, maxBlockSize);
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
    switch (command.type) {
        case CommandType::SetFrequency:
            oscillator_.setFrequency(command.floatValue);
            break;
        case CommandType::SetWaveform:
            oscillator_.setWaveform(static_cast<Waveform>(command.intValue));
            break;
        case CommandType::SetGain:
            oscillator_.setGain(command.floatValue);
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

    oscillator_.process(buffer);
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
