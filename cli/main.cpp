#include "daw/AudioBuffer.hpp"
#include "daw/Engine.hpp"
#include "daw/Timeline.hpp"

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
    std::size_t numTracks = 1;
    std::vector<int> notes;
    float velocity = 0.9f;
    std::string wave = "saw";
    float attackMs = 5.0f;
    float decayMs = 120.0f;
    float sustain = 0.7f;
    float releaseMs = 250.0f;
    float cutoffHz = 4000.0f;
    float resonance = 0.9f;
    double holdFraction = 0.6;
    int arpSteps = 0;
    double bpm = 120.0;
    bool loop = false;
    bool noSaturation = false;
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
        } else if (arg == "--tracks" && hasValue) {
            args.numTracks = static_cast<std::size_t>(std::stoul(argv[++i]));
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
        } else if (arg == "--arp" && hasValue) {
            args.arpSteps = std::stoi(argv[++i]);
        } else if (arg == "--bpm" && hasValue) {
            args.bpm = std::stod(argv[++i]);
        } else if (arg == "--loop") {
            args.loop = true;
        } else if (arg == "--no-saturation") {
            args.noSaturation = true;
        } else if (arg == "--benchmark") {
            args.benchmark = true;
        }
    }

    if (args.notes.empty()) args.notes.push_back(69);
    args.numTracks = std::clamp<std::size_t>(args.numTracks, 1, daw::Engine::kMaxTracks);

    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kBlockSize = 128;
    constexpr std::size_t kNumChannels = 2;

    daw::Engine engine;
    engine.prepare(kSampleRate, kBlockSize, kNumChannels);

    for (std::size_t t = 0; t < args.numTracks; ++t) {
        if (engine.addTrack() == daw::Engine::kMaxTracks) {
            std::fprintf(stderr, "error: could not create track %zu\n", t);
            return 1;
        }
    }

    // Spread the tracks across the image so multi-track output is audibly
    // more than one louder track.
    for (std::size_t t = 0; t < args.numTracks; ++t) {
        const float pan = args.numTracks == 1
                              ? 0.0f
                              : -1.0f + 2.0f * static_cast<float>(t) / static_cast<float>(args.numTracks - 1);
        const auto track = static_cast<std::uint8_t>(t);
        queueOrWarn(engine, {daw::CommandType::SetTrackPan, pan, 0, track}, "pan");
        queueOrWarn(engine, {daw::CommandType::SetWaveform, 0.0f, parseWaveform(args.wave), track}, "waveform");
        queueOrWarn(engine, {daw::CommandType::SetAttack, args.attackMs, 0, track}, "attack");
        queueOrWarn(engine, {daw::CommandType::SetDecay, args.decayMs, 0, track}, "decay");
        queueOrWarn(engine, {daw::CommandType::SetSustain, args.sustain, 0, track}, "sustain");
        queueOrWarn(engine, {daw::CommandType::SetRelease, args.releaseMs, 0, track}, "release");
        queueOrWarn(engine, {daw::CommandType::SetFilterCutoff, args.cutoffHz, 0, track}, "cutoff");
        queueOrWarn(engine, {daw::CommandType::SetFilterResonance, args.resonance, 0, track}, "resonance");
    }

    if (args.noSaturation) {
        queueOrWarn(engine, {daw::CommandType::SetMasterSaturation, 0.0f, 0, 0}, "saturation");
    }

    const std::size_t totalFrames = static_cast<std::size_t>(args.seconds * kSampleRate);

    if (args.arpSteps > 0) {
        // The arrangement is musical: notes live in ticks, and the engine
        // compiles them to sample positions at the transport's tempo. Every
        // step lands on an exact sample regardless of block boundaries.
        engine.transport().setTempo(args.bpm);

        constexpr std::uint32_t kTicksPerStep = daw::Timeline::kTicksPerQuarter / 2; // eighth notes

        for (int step = 0; step < args.arpSteps; ++step) {
            daw::Note note{};
            note.startTick = static_cast<std::uint32_t>(step) * kTicksPerStep;
            note.lengthTicks = (kTicksPerStep * 3) / 4;
            note.pitch = static_cast<std::uint8_t>(args.notes[static_cast<std::size_t>(step) % args.notes.size()]);
            note.track = static_cast<std::uint8_t>(static_cast<std::size_t>(step) % args.numTracks);
            note.velocity = args.velocity;

            if (!engine.timeline().addNote(note)) {
                std::fprintf(stderr, "warning: timeline full at step %d\n", step);
                break;
            }
        }

        if (!engine.compileTimeline()) {
            std::fprintf(stderr, "error: could not compile the arrangement\n");
            return 1;
        }

        if (args.loop) {
            engine.setLoopTicks(0, static_cast<std::uint32_t>(args.arpSteps) * kTicksPerStep);
            queueOrWarn(engine, {daw::CommandType::TransportSetLoop, 0.0f, 1, 0}, "loop");
        }
        queueOrWarn(engine, {daw::CommandType::TransportPlay, 0.0f, 0, 0}, "play");
    } else {
        for (std::size_t n = 0; n < args.notes.size(); ++n) {
            const auto track = static_cast<std::uint8_t>(n % args.numTracks);
            queueOrWarn(engine, {daw::CommandType::NoteOn, args.velocity, args.notes[n], track}, "note on");
        }
    }

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
        if (args.arpSteps == 0 && !released && framesRendered >= releaseFrame) {
            for (std::size_t n = 0; n < args.notes.size(); ++n) {
                const auto track = static_cast<std::uint8_t>(n % args.numTracks);
                queueOrWarn(engine, {daw::CommandType::NoteOff, 0.0f, args.notes[n], track}, "note off");
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
        std::printf("tracks=%zu nodes=%zu blocks=%zu p50=%.2fus p99=%.2fus max=%.2fus budget=%.2fus\n",
                    args.numTracks, engine.graph().numNodes(), blockTimesUs.size(), p50, p99, maxUs, budgetUs);
    }

    std::printf("wrote %s (%zu frames, %.2fs, %zu track(s), %zu note(s))\n",
                args.outputPath.c_str(), totalFrames, args.seconds, args.numTracks, args.notes.size());
    return 0;
}
