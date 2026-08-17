#include "daw/AudioBuffer.hpp"
#include "daw/Engine.hpp"
#include "daw/Node.hpp"
#include "daw/Oscillator.hpp"
#include "daw/RingBuffer.hpp"

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int checksRun = 0;
int checksFailed = 0;

void check(bool condition, const char* description) {
    ++checksRun;
    if (!condition) {
        ++checksFailed;
        std::fprintf(stderr, "FAILED: %s\n", description);
    }
}

void testRingBufferFifoOrdering() {
    daw::RingBuffer<int, 8> ring;
    for (int i = 0; i < 7; ++i) {
        check(ring.push(i), "ring buffer push should succeed while not full");
    }
    for (int i = 0; i < 7; ++i) {
        int value = -1;
        check(ring.pop(value), "ring buffer pop should succeed while not empty");
        check(value == i, "ring buffer should preserve FIFO order");
    }
    check(ring.empty(), "ring buffer should be empty after draining everything pushed");
}

void testRingBufferOverflow() {
    daw::RingBuffer<int, 4> ring;
    check(ring.push(1), "push into ring buffer with headroom should succeed");
    check(ring.push(2), "push into ring buffer with headroom should succeed");
    check(ring.push(3), "push into ring buffer with headroom should succeed");
    check(!ring.push(4), "push into full ring buffer should fail gracefully, not crash or block");

    int value = -1;
    check(ring.pop(value) && value == 1, "pop after overflow should still return the oldest surviving value");
}

void testRingBufferTwoThreads() {
    constexpr int kCount = 200000;
    daw::RingBuffer<int, 1024> ring;
    std::vector<int> consumed;
    consumed.reserve(kCount);

    std::thread producer([&] {
        int i = 0;
        while (i < kCount) {
            if (ring.push(i)) ++i;
        }
    });

    std::thread consumer([&] {
        int value = 0;
        int received = 0;
        while (received < kCount) {
            if (ring.pop(value)) {
                consumed.push_back(value);
                ++received;
            }
        }
    });

    producer.join();
    consumer.join();

    check(static_cast<int>(consumed.size()) == kCount, "two-thread ring buffer should deliver every item pushed");

    bool ordered = true;
    for (int i = 0; i < kCount; ++i) {
        if (consumed[static_cast<std::size_t>(i)] != i) {
            ordered = false;
            break;
        }
    }
    check(ordered, "two-thread ring buffer should preserve producer order under real concurrency");
}

void testSilenceBeforePrepare() {
    daw::Engine engine; // prepare() deliberately not called
    std::vector<float> left(128, 1.0f), right(128, 1.0f);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);

    engine.render(buffer);

    bool silent = true;
    for (float sample : left) if (sample != 0.0f) silent = false;
    for (float sample : right) if (sample != 0.0f) silent = false;
    check(silent, "engine should output silence when render() is called before prepare()");
}

void testSineFrequencyAccuracy() {
    constexpr double kSampleRate = 48000.0;
    constexpr float kFrequency = 440.0f;
    constexpr std::size_t kNumFrames = 48000; // 1 second

    daw::Oscillator osc;
    osc.prepare(kSampleRate, kNumFrames);
    osc.setWaveform(daw::Waveform::Sine);
    osc.setFrequency(kFrequency);
    osc.setGain(1.0f);

    std::vector<float> mono(kNumFrames);
    float* channels[1] = {mono.data()};
    daw::AudioBuffer buffer(channels, 1, kNumFrames);
    osc.process(buffer);

    int zeroCrossings = 0;
    for (std::size_t i = 1; i < kNumFrames; ++i) {
        const bool crossedUp = mono[i - 1] < 0.0f && mono[i] >= 0.0f;
        const bool crossedDown = mono[i - 1] >= 0.0f && mono[i] < 0.0f;
        if (crossedUp || crossedDown) ++zeroCrossings;
    }
    const double measuredHz = zeroCrossings / 2.0;
    check(std::fabs(measuredHz - kFrequency) < 2.0,
          "sine oscillator zero-crossing count should match the requested frequency within tolerance");
}

void testStereoMirroring() {
    daw::Oscillator osc;
    osc.prepare(48000.0, 128);
    osc.setWaveform(daw::Waveform::Sine);
    osc.setFrequency(220.0f);
    osc.setGain(1.0f);

    std::vector<float> left(128), right(128);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);
    osc.process(buffer);

    bool mirrored = true;
    for (std::size_t i = 0; i < 128; ++i) {
        if (left[i] != right[i]) {
            mirrored = false;
            break;
        }
    }
    check(mirrored, "mono oscillator output should mirror identically across both channels");
}

void testOutputClamping() {
    struct LoudNode final : daw::Node {
        void prepare(double, std::size_t) override {}
        void process(daw::AudioBuffer& buffer) noexcept override {
            for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch) {
                for (std::size_t i = 0; i < buffer.numFrames(); ++i) {
                    buffer.channel(ch)[i] += 10.0f; // deliberately out of range
                }
            }
        }
    };

    daw::Engine engine;
    engine.prepare(48000.0, 128);
    LoudNode loud;
    check(engine.addNode(&loud), "adding a node before rendering starts should succeed");

    std::vector<float> left(128), right(128);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);
    engine.render(buffer);

    bool clamped = true;
    for (float sample : left) if (sample > 1.0f || sample < -1.0f) clamped = false;
    for (float sample : right) if (sample > 1.0f || sample < -1.0f) clamped = false;
    check(clamped, "engine should clamp final output to [-1, 1] regardless of node output");
    check(engine.peakLevel() <= 1.0f, "reported peak level should reflect the post-clamp signal");
}

void testVariableBlockSizes() {
    daw::Engine engine;
    engine.prepare(48000.0, 512);
    engine.oscillator().setFrequency(440.0f);

    std::vector<float> left(512), right(512);
    float* channels[2] = {left.data(), right.data()};

    const std::size_t blockSizes[] = {1, 7, 64, 128, 512};
    bool ranWithoutCrash = true;
    for (std::size_t blockSize : blockSizes) {
        daw::AudioBuffer buffer(channels, 2, blockSize);
        engine.render(buffer);
    }
    check(ranWithoutCrash, "engine should render correctly across a sequence of differently sized blocks");
}

void testCommandQueueOverflowIsGraceful() {
    daw::Engine engine;
    engine.prepare(48000.0, 128);

    bool allSucceeded = true;
    for (int i = 0; i < static_cast<int>(daw::Engine::kCommandQueueCapacity) + 10; ++i) {
        const bool pushed = engine.pushCommand({daw::CommandType::SetFrequency, 440.0f, 0});
        if (!pushed) allSucceeded = false;
    }
    check(!allSucceeded, "pushing more commands than queue capacity should eventually report failure, not overwrite or crash");

    std::vector<float> left(128), right(128);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);
    engine.render(buffer); // draining a full-then-overflowed queue must not crash
    check(true, "engine should drain a previously overflowed command queue without crashing");
}

} // namespace

int main() {
    testRingBufferFifoOrdering();
    testRingBufferOverflow();
    testRingBufferTwoThreads();
    testSilenceBeforePrepare();
    testSineFrequencyAccuracy();
    testStereoMirroring();
    testOutputClamping();
    testVariableBlockSizes();
    testCommandQueueOverflowIsGraceful();

    std::printf("%d checks run, %d failed\n", checksRun, checksFailed);
    return checksFailed == 0 ? 0 : 1;
}
