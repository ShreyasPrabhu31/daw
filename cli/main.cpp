#include "daw/AudioBuffer.hpp"
#include "daw/Engine.hpp"
#include "daw/Oscillator.hpp"

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
    float frequency = 440.0f;
    std::string wave = "sine";
    bool benchmark = false;
};

daw::Waveform parseWaveform(const std::string& s) {
    if (s == "saw") return daw::Waveform::Saw;
    if (s == "square") return daw::Waveform::Square;
    return daw::Waveform::Sine;
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

} // namespace

int main(int argc, char** argv) {
    Args args;
    if (argc > 1) args.outputPath = argv[1];

    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc) {
            args.seconds = std::stod(argv[++i]);
        } else if (arg == "--freq" && i + 1 < argc) {
            args.frequency = std::stof(argv[++i]);
        } else if (arg == "--wave" && i + 1 < argc) {
            args.wave = argv[++i];
        } else if (arg == "--benchmark") {
            args.benchmark = true;
        }
    }

    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr std::size_t kNumChannels = 2;

    daw::Engine engine;
    engine.prepare(kSampleRate, kBlockSize);
    engine.oscillator().setWaveform(parseWaveform(args.wave));
    const bool queuedFrequency = engine.pushCommand({daw::CommandType::SetFrequency, args.frequency, 0});
    const bool queuedGain = engine.pushCommand({daw::CommandType::SetGain, 0.8f, 0});
    if (!queuedFrequency || !queuedGain) {
        std::fprintf(stderr, "warning: initial command queue push failed\n");
    }

    const std::size_t totalFrames = static_cast<std::size_t>(args.seconds * kSampleRate);

    std::vector<float> left(kBlockSize), right(kBlockSize);
    float* channelPtrs[kNumChannels] = {left.data(), right.data()};

    std::vector<float> interleaved;
    interleaved.reserve(totalFrames * kNumChannels);

    std::vector<double> blockTimesUs;
    blockTimesUs.reserve((totalFrames / kBlockSize) + 1);

    std::size_t framesRendered = 0;
    while (framesRendered < totalFrames) {
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

    writeWavFile(args.outputPath, interleaved, static_cast<std::uint32_t>(kNumChannels),
                 static_cast<std::uint32_t>(kSampleRate));

    if (args.benchmark && !blockTimesUs.empty()) {
        std::sort(blockTimesUs.begin(), blockTimesUs.end());
        const double p50 = blockTimesUs[blockTimesUs.size() / 2];
        const double p99 = blockTimesUs[static_cast<std::size_t>(blockTimesUs.size() * 0.99)];
        const double maxUs = blockTimesUs.back();
        const double budgetUs = (static_cast<double>(kBlockSize) / kSampleRate) * 1'000'000.0;
        std::printf("blocks=%zu p50=%.2fus p99=%.2fus max=%.2fus budget=%.2fus\n",
                    blockTimesUs.size(), p50, p99, maxUs, budgetUs);
    }

    std::printf("wrote %s (%zu frames, %.2fs)\n", args.outputPath.c_str(), totalFrames, args.seconds);
    return 0;
}
