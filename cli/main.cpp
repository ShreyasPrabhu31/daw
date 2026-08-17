#include "daw/AudioBuffer.hpp"
#include "daw/Engine.hpp"
#include "daw/Synth.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Args {
    std::string outputPath = "out.wav";
    double seconds = 2.0;
    std::vector<int> notes;
    float velocity = 0.9f;
    std::string wave = "saw";
    float attackMs = 5.0f;
    float decayMs = 120.0f;
    float sustain = 0.7f;
    float releaseMs = 250.0f;
    float cutoffHz = 4000.0f;
    float resonance = 0.9f;
    // Fraction of the render at which the keys are lifted, so the file
    // captures the release tail instead of being cut off mid-sustain.
    double holdFraction = 0.6;
    bool benchmark = false;
};

int parseWaveform(const std::string& s) {
    if (s == "saw") return 1;
    if (s == "square") return 2;
    return 0; // sine
}

bool writeWavFile(const std::string& path, const std::vector<float>& interleaved,
                   std::uint32_t numChannels, std::uint32_t sampleRate) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;

    const std::uint32_t numFrames = static_cast<std::uint32_t>(interleaved.size() / numChannels);
    const std::uint16_t bitsPerSample = 16;
    const std::uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(numChannels * (bitsPerSample / 8));
    const std::uint32_t dataSize = numFrames * blockAlign;
    const std::uint32_t riffSize = 36 + dataSize;

    auto writeStr = [&](const char* s) { file.write(s, 4); };
    auto writeU32 = [&](std::uint32_t v) { file.write(reinterpret_cast<const char*>(&v), 4); };
    auto writeU16 = [&](std::uint16_t v) { file.write(reinterpret_cast<const char*>(&v), 2); };

    writeStr("RIFF");
    writeU32(riffSize);
    writeStr("WAVE");
    writeStr("fmt ");
    writeU32(16);
    writeU16(1); // PCM
    writeU16(static_cast<std::uint16_t>(numChannels));
    writeU32(sampleRate);
    writeU32(byteRate);
    writeU16(blockAlign);
    writeU16(bitsPerSample);
    writeStr("data");
    writeU32(dataSize);

    for (float sample : interleaved) {
        const float clamped = std::clamp(sample, -1.0f, 1.0f);
        const std::int16_t pcm = static_cast<std::int16_t>(clamped * 32767.0f);
        file.write(reinterpret_cast<const char*>(&pcm), 2);
    }

    return true;
}

void queueOrWarn(daw::Engine& engine, const daw::EngineCommand& command, const char* what) {
    if (!engine.pushCommand(command)) {
        std::fprintf(stderr, "warning: could not queue %s\n", what);
    }
}

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (argc > 1 && argv[1][0] != '-') args.outputPath = argv[1];

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--seconds" && hasValue) {
            args.seconds = std::stod(argv[++i]);
        } else if (arg == "--note" && hasValue) {
            args.notes.push_back(std::stoi(argv[++i]));
        } else if (arg == "--velocity" && hasValue) {
            args.velocity = std::stof(argv[++i]);
        } else if (arg == "--wave" && hasValue) {
            args.wave = argv[++i];
        } else if (arg == "--attack" && hasValue) {
            args.attackMs = std::stof(argv[++i]);
        } else if (arg == "--decay" && hasValue) {
            args.decayMs = std::stof(argv[++i]);
        } else if (arg == "--sustain" && hasValue) {
            args.sustain = std::stof(argv[++i]);
        } else if (arg == "--release" && hasValue) {
            args.releaseMs = std::stof(argv[++i]);
        } else if (arg == "--cutoff" && hasValue) {
            args.cutoffHz = std::stof(argv[++i]);
        } else if (arg == "--resonance" && hasValue) {
            args.resonance = std::stof(argv[++i]);
        } else if (arg == "--hold" && hasValue) {
            args.holdFraction = std::stod(argv[++i]);
        } else if (arg == "--benchmark") {
            args.benchmark = true;
        }
    }

    // A bare invocation should still make a sound: default to a single A440.
    if (args.notes.empty()) args.notes.push_back(69);

    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr std::size_t kNumChannels = 2;

    daw::Engine engine;
    engine.prepare(kSampleRate, kBlockSize);

    queueOrWarn(engine, {daw::CommandType::SetWaveform, 0.0f, parseWaveform(args.wave)}, "waveform");
    queueOrWarn(engine, {daw::CommandType::SetAttack, args.attackMs, 0}, "attack");
    queueOrWarn(engine, {daw::CommandType::SetDecay, args.decayMs, 0}, "decay");
    queueOrWarn(engine, {daw::CommandType::SetSustain, args.sustain, 0}, "sustain");
    queueOrWarn(engine, {daw::CommandType::SetRelease, args.releaseMs, 0}, "release");
    queueOrWarn(engine, {daw::CommandType::SetFilterCutoff, args.cutoffHz, 0}, "cutoff");
    queueOrWarn(engine, {daw::CommandType::SetFilterResonance, args.resonance, 0}, "resonance");

    for (int note : args.notes) {
        queueOrWarn(engine, {daw::CommandType::NoteOn, args.velocity, note}, "note on");
    }

    const std::size_t totalFrames = static_cast<std::size_t>(args.seconds * kSampleRate);
    const std::size_t releaseFrame = static_cast<std::size_t>(static_cast<double>(totalFrames) * args.holdFraction);

    std::vector<float> left(kBlockSize), right(kBlockSize);
    float* channelPtrs[kNumChannels] = {left.data(), right.data()};

    std::vector<float> interleaved;
    interleaved.reserve(totalFrames * kNumChannels);

    std::vector<double> blockTimesUs;
    blockTimesUs.reserve((totalFrames / kBlockSize) + 1);

    std::size_t framesRendered = 0;
    bool released = false;
    while (framesRendered < totalFrames) {
        if (!released && framesRendered >= releaseFrame) {
            for (int note : args.notes) {
                queueOrWarn(engine, {daw::CommandType::NoteOff, 0.0f, note}, "note off");
            }
            released = true;
        }

        const std::size_t framesThisBlock = std::min(kBlockSize, totalFrames - framesRendered);
        daw::AudioBuffer blockView(channelPtrs, kNumChannels, framesThisBlock);

        const auto start = std::chrono::steady_clock::now();
        engine.render(blockView);
        const auto end = std::chrono::steady_clock::now();
        blockTimesUs.push_back(std::chrono::duration<double, std::micro>(end - start).count());

        for (std::size_t i = 0; i < framesThisBlock; ++i) {
            interleaved.push_back(left[i]);
            interleaved.push_back(right[i]);
        }
        framesRendered += framesThisBlock;
    }

    if (!writeWavFile(args.outputPath, interleaved, static_cast<std::uint32_t>(kNumChannels),
                      static_cast<std::uint32_t>(kSampleRate))) {
        std::fprintf(stderr, "error: could not write %s\n", args.outputPath.c_str());
        return 1;
    }

    if (args.benchmark && !blockTimesUs.empty()) {
        std::sort(blockTimesUs.begin(), blockTimesUs.end());
        const double p50 = blockTimesUs[blockTimesUs.size() / 2];
        const double p99 = blockTimesUs[static_cast<std::size_t>(blockTimesUs.size() * 0.99)];
        const double maxUs = blockTimesUs.back();
        const double budgetUs = (static_cast<double>(kBlockSize) / kSampleRate) * 1'000'000.0;
        std::printf("voices=%zu blocks=%zu p50=%.2fus p99=%.2fus max=%.2fus budget=%.2fus\n",
                    args.notes.size(), blockTimesUs.size(), p50, p99, maxUs, budgetUs);
    }

    std::printf("wrote %s (%zu frames, %.2fs, %zu note(s))\n",
                args.outputPath.c_str(), totalFrames, args.seconds, args.notes.size());
    return 0;
}
