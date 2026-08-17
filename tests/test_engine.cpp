#include "daw/ADSR.hpp"
#include "daw/AudioBuffer.hpp"
#include "daw/Biquad.hpp"
#include "daw/Engine.hpp"
#include "daw/Graph.hpp"
#include "daw/MasterBus.hpp"
#include "daw/Track.hpp"
#include "daw/Node.hpp"
#include "daw/Oscillator.hpp"
#include "daw/RingBuffer.hpp"
#include "daw/Synth.hpp"
#include "daw/Voice.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <utility>
#include <vector>

// The central claim of this codebase is that nothing on the audio path
// allocates. Counting every trip through global operator new turns that from
// a design intention into something a test can fail on.
//
// ASan and TSan ship their own operator new, so the override is compiled out
// under those builds; the plain build is the one that enforces this.
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__) && \
    !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer)
#define DAW_TRACK_ALLOCATIONS 1
#else
#define DAW_TRACK_ALLOCATIONS 0
#endif

#if DAW_TRACK_ALLOCATIONS
static std::atomic<long> gAllocationCount{0};

void* operator new(std::size_t size) {
    gAllocationCount.fetch_add(1, std::memory_order_relaxed);
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) throw std::bad_alloc();
    return memory;
}

void* operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
#endif

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

// Root mean square over a span, used to compare filter output energy at
// different input frequencies.
double rms(const std::vector<float>& samples, std::size_t from) {
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t i = from; i < samples.size(); ++i) {
        sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
        ++count;
    }
    return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
}

// Steps the envelope forward without reading the value. next() is
// [[nodiscard]] because dropping a sample is a bug in the audio path; in a
// test that only wants to reach a later stage, discarding is the intent.
void advance(daw::ADSR& env, int samples) {
    for (int i = 0; i < samples; ++i) {
        (void)env.next();
    }
}

// Runs a sine of the given frequency through a filter and reports the energy
// of the steady-state portion of the result.
double filteredRms(daw::Biquad::Type type, float cutoffHz, float inputHz) {
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kNumSamples = 9600;

    daw::Biquad filter;
    filter.prepare(kSampleRate);
    filter.setType(type);
    filter.setCutoff(cutoffHz);
    filter.setResonance(0.707f);

    std::vector<float> output(kNumSamples);
    const double increment = 2.0 * 3.14159265358979323846 * static_cast<double>(inputHz) / kSampleRate;
    for (std::size_t i = 0; i < kNumSamples; ++i) {
        const float input = static_cast<float>(std::sin(increment * static_cast<double>(i)));
        output[i] = filter.process(input);
    }

    // Skip the first half so the filter's startup transient does not count.
    return rms(output, kNumSamples / 2);
}

// ---------------------------------------------------------------------------
// Phase 0: ring buffer, engine plumbing, oscillator
// ---------------------------------------------------------------------------

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
    const daw::Graph::NodeId loudId = engine.graph().addNode(&loud);
    check(loudId != daw::Graph::kInvalidNode, "adding a node to the graph should succeed");
    check(engine.graph().connect(loudId, engine.graph().outputNode()), "connecting a node to the master bus should succeed");
    check(engine.graph().rebuild(), "publishing a plan for an acyclic graph should succeed");

    std::vector<float> left(128), right(128);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);
    engine.render(buffer);

    bool clamped = true;
    for (float sample : left) if (sample > 1.0f || sample < -1.0f) clamped = false;
    for (float sample : right) if (sample > 1.0f || sample < -1.0f) clamped = false;
    check(clamped, "engine should bound final output to [-1, 1] regardless of node output");
    check(engine.peakLevel() <= 1.0f, "reported peak level should reflect the post-clamp signal");
}

void testVariableBlockSizes() {
    daw::Engine engine;
    engine.prepare(48000.0, 512);
    const std::size_t trackIndex = engine.addTrack();
    check(trackIndex == 0, "the first track should be index zero");
    engine.track(0).synth().noteOn(60, 1.0f);

    std::vector<float> left(512), right(512);
    float* channels[2] = {left.data(), right.data()};

    const std::size_t blockSizes[] = {1, 7, 64, 128, 512};
    for (std::size_t blockSize : blockSizes) {
        daw::AudioBuffer buffer(channels, 2, blockSize);
        engine.render(buffer);
    }
    check(true, "engine should render correctly across a sequence of differently sized blocks");
}

void testCommandQueueOverflowIsGraceful() {
    daw::Engine engine;
    engine.prepare(48000.0, 128);

    bool allSucceeded = true;
    for (int i = 0; i < static_cast<int>(daw::Engine::kCommandQueueCapacity) + 10; ++i) {
        const bool pushed = engine.pushCommand({daw::CommandType::SetMasterGain, 0.5f, 0, 0, 0});
        if (!pushed) allSucceeded = false;
    }
    check(!allSucceeded, "pushing more commands than queue capacity should eventually report failure, not overwrite or crash");

    std::vector<float> left(128), right(128);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);
    engine.render(buffer); // draining a full-then-overflowed queue must not crash
    check(true, "engine should drain a previously overflowed command queue without crashing");
}

// ---------------------------------------------------------------------------
// Phase 1: ADSR
// ---------------------------------------------------------------------------

void testEnvelopeIdleIsSilent() {
    daw::ADSR env;
    env.prepare(48000.0);
    check(!env.isActive(), "a fresh envelope should be inactive");

    bool silent = true;
    for (int i = 0; i < 1000; ++i) {
        if (env.next() != 0.0f) silent = false;
    }
    check(silent, "an idle envelope should output exactly zero, not a denormal trickle");
}

void testEnvelopeAttackReachesPeak() {
    constexpr double kSampleRate = 48000.0;
    daw::ADSR env;
    env.prepare(kSampleRate);
    env.setParameters({10.0f, 100.0f, 0.5f, 100.0f});
    env.noteOn();

    // 10 ms at 48 kHz is 480 samples.
    advance(env, 479);
    check(env.level() > 0.9f, "envelope should be near peak just before the attack time elapses");

    advance(env, 1);
    check(env.level() >= 0.999f, "envelope should reach full level at the end of the attack segment");
}

void testEnvelopeDecaysToSustainAndHolds() {
    constexpr double kSampleRate = 48000.0;
    daw::ADSR env;
    env.prepare(kSampleRate);
    env.setParameters({1.0f, 20.0f, 0.4f, 100.0f});
    env.noteOn();

    // Attack (48 samples) plus decay (960 samples), with slack.
    advance(env, 2000);
    check(std::fabs(env.level() - 0.4f) < 0.001f, "envelope should settle at the sustain level after decay");

    // Sustain must hold indefinitely while the key is down.
    advance(env, 48000);
    check(std::fabs(env.level() - 0.4f) < 0.001f, "envelope should hold the sustain level until note off");
    check(env.isActive(), "a sustaining envelope should still report active");
}

void testEnvelopeReleaseTerminatesExactly() {
    constexpr double kSampleRate = 48000.0;
    daw::ADSR env;
    env.prepare(kSampleRate);
    env.setParameters({1.0f, 10.0f, 0.6f, 50.0f}); // 50 ms release = 2400 samples
    env.noteOn();
    advance(env, 1000);

    env.noteOff();
    check(env.isReleasing(), "envelope should report releasing after note off");

    int samplesUntilIdle = 0;
    while (env.isActive() && samplesUntilIdle < 10000) {
        advance(env, 1);
        ++samplesUntilIdle;
    }

    check(!env.isActive(), "release must terminate so the voice pool can reclaim the voice");
    check(env.level() == 0.0f, "envelope should land on exactly zero, not merely close to it");
    check(samplesUntilIdle > 2300 && samplesUntilIdle < 2500,
          "release should take approximately the configured release time");
}

void testEnvelopeReleaseTimeIndependentOfLevel() {
    constexpr double kSampleRate = 48000.0;

    auto samplesToRelease = [&](int samplesHeld) {
        daw::ADSR env;
        env.prepare(kSampleRate);
        env.setParameters({100.0f, 100.0f, 0.5f, 50.0f});
        env.noteOn();
        advance(env, samplesHeld);
        env.noteOff();

        int count = 0;
        while (env.isActive() && count < 10000) {
            advance(env, 1);
            ++count;
        }
        return count;
    };

    // Released early in the attack (low level) versus fully sustained.
    const int shortHold = samplesToRelease(100);
    const int longHold = samplesToRelease(20000);
    check(std::abs(shortHold - longHold) < 50,
          "release should take the same time whether the key was let go during attack or after sustain");
}

void testEnvelopeZeroSustainFreesVoice() {
    daw::ADSR env;
    env.prepare(48000.0);
    env.setParameters({1.0f, 10.0f, 0.0f, 100.0f});
    env.noteOn();

    int count = 0;
    while (env.isActive() && count < 48000) {
        advance(env, 1);
        ++count;
    }
    check(!env.isActive(),
          "a zero-sustain (percussive) patch should free its voice after decay without waiting for note off");
}

void testEnvelopeRetriggerDoesNotSnapToZero() {
    daw::ADSR env;
    env.prepare(48000.0);
    env.setParameters({50.0f, 100.0f, 0.8f, 100.0f});
    env.noteOn();
    advance(env, 2000);

    const float levelBefore = env.level();
    check(levelBefore > 0.0f, "envelope should have risen before the retrigger");

    env.noteOn(); // steal / retrigger
    const float levelAfter = env.next();
    check(levelAfter >= levelBefore,
          "retriggering should continue from the current level, not drop to zero and click");
}

// ---------------------------------------------------------------------------
// Phase 1: Biquad
// ---------------------------------------------------------------------------

void testLowPassPassesLowAndBlocksHigh() {
    const double passband = filteredRms(daw::Biquad::Type::LowPass, 1000.0f, 100.0f);
    const double stopband = filteredRms(daw::Biquad::Type::LowPass, 1000.0f, 12000.0f);

    // A unit sine has an RMS of about 0.707.
    check(passband > 0.65 && passband < 0.75, "low-pass should pass a well-below-cutoff sine at roughly unity gain");
    check(stopband < passband * 0.05, "low-pass should strongly attenuate a sine far above the cutoff");
}

void testHighPassPassesHighAndBlocksLow() {
    const double passband = filteredRms(daw::Biquad::Type::HighPass, 1000.0f, 12000.0f);
    const double stopband = filteredRms(daw::Biquad::Type::HighPass, 1000.0f, 100.0f);

    check(passband > 0.65 && passband < 0.75, "high-pass should pass a well-above-cutoff sine at roughly unity gain");
    check(stopband < passband * 0.05, "high-pass should strongly attenuate a sine far below the cutoff");
}

void testBandPassPeaksAtCentre() {
    const double centre = filteredRms(daw::Biquad::Type::BandPass, 1000.0f, 1000.0f);
    const double below = filteredRms(daw::Biquad::Type::BandPass, 1000.0f, 60.0f);
    const double above = filteredRms(daw::Biquad::Type::BandPass, 1000.0f, 16000.0f);

    check(centre > below * 5.0, "band-pass should pass its centre frequency far more than a frequency below the band");
    check(centre > above * 5.0, "band-pass should pass its centre frequency far more than a frequency above the band");
}

void testFilterStaysStableAtExtremes() {
    daw::Biquad filter;
    filter.prepare(48000.0);
    filter.setType(daw::Biquad::Type::LowPass);

    // Deliberately absurd requests: the clamps inside updateCoefficients are
    // what keep the bilinear transform from producing a divergent filter.
    filter.setCutoff(1'000'000.0f);
    filter.setResonance(500.0f);

    bool finite = true;
    for (int i = 0; i < 48000; ++i) {
        const float input = (i % 2 == 0) ? 1.0f : -1.0f; // Nyquist-rate square
        const float output = filter.process(input);
        if (!std::isfinite(output)) finite = false;
    }
    check(finite, "filter should stay finite when asked for an out-of-range cutoff and extreme resonance");

    filter.setCutoff(0.0f);
    filter.setResonance(0.0f);
    finite = true;
    for (int i = 0; i < 48000; ++i) {
        if (!std::isfinite(filter.process(0.5f))) finite = false;
    }
    check(finite, "filter should stay finite at a zero cutoff and zero resonance request");
}

// ---------------------------------------------------------------------------
// Phase 1: Voice
// ---------------------------------------------------------------------------

void testIdleVoiceAddsNothing() {
    daw::Voice voice;
    voice.prepare(48000.0, 128);

    std::vector<float> left(128, 0.25f), right(128, 0.25f);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);

    voice.renderAdd(buffer, 1.0f);

    bool untouched = true;
    for (float sample : left) if (sample != 0.25f) untouched = false;
    check(untouched, "an idle voice should add nothing to the buffer, leaving other voices' output intact");
}

void testVoiceSoundsThenFreesItself() {
    constexpr double kSampleRate = 48000.0;
    daw::Voice voice;
    voice.prepare(kSampleRate, 128);
    voice.setEnvelopeParameters({1.0f, 10.0f, 0.8f, 20.0f});
    voice.noteOn(69, 440.0f, 1.0f, 1);

    check(voice.isActive(), "voice should be active immediately after note on");
    check(voice.midiNote() == 69, "voice should remember which note it is playing");

    std::vector<float> left(128, 0.0f), right(128, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);

    float peak = 0.0f;
    for (int block = 0; block < 20; ++block) {
        buffer.clear();
        voice.renderAdd(buffer, 1.0f);
        for (float sample : left) peak = std::max(peak, std::fabs(sample));
    }
    check(peak > 0.1f, "a sounding voice should produce a non-trivial signal");

    voice.noteOff();
    for (int block = 0; block < 40; ++block) {
        buffer.clear();
        voice.renderAdd(buffer, 1.0f);
    }
    check(!voice.isActive(), "voice should free itself once its release has finished");
}

void testVoiceOutputIsAdditive() {
    daw::Voice voiceA;
    daw::Voice voiceB;
    voiceA.prepare(48000.0, 128);
    voiceB.prepare(48000.0, 128);
    voiceA.setEnvelopeParameters({0.0f, 1000.0f, 1.0f, 100.0f});
    voiceB.setEnvelopeParameters({0.0f, 1000.0f, 1.0f, 100.0f});

    std::vector<float> left(128, 0.0f), right(128, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);

    voiceA.noteOn(69, 440.0f, 1.0f, 1);
    buffer.clear();
    voiceA.renderAdd(buffer, 1.0f);
    std::vector<float> singleVoice = left;

    voiceA.reset();
    voiceA.noteOn(69, 440.0f, 1.0f, 1);
    voiceB.noteOn(69, 440.0f, 1.0f, 2);
    buffer.clear();
    voiceA.renderAdd(buffer, 1.0f);
    voiceB.renderAdd(buffer, 1.0f);

    // Two identical voices should sum to twice one voice, sample for sample.
    bool doubled = true;
    for (std::size_t i = 0; i < 128; ++i) {
        if (std::fabs(left[i] - 2.0f * singleVoice[i]) > 1e-5f) doubled = false;
    }
    check(doubled, "voices should sum into the block rather than overwrite each other");
}

// ---------------------------------------------------------------------------
// Phase 1: polyphony and voice stealing
// ---------------------------------------------------------------------------

void testEightVoicePolyphony() {
    daw::Synth synth;
    synth.prepare(48000.0, 128);

    for (int note = 60; note < 68; ++note) {
        synth.noteOn(note, 1.0f);
    }
    check(synth.activeVoiceCount() == daw::Synth::kNumVoices,
          "eight simultaneous notes should occupy all eight voices");
}

void testNinthNoteStealsRatherThanOverflows() {
    daw::Synth synth;
    synth.prepare(48000.0, 128);

    for (int note = 60; note < 68; ++note) synth.noteOn(note, 1.0f);
    synth.noteOn(72, 1.0f);

    check(synth.activeVoiceCount() == daw::Synth::kNumVoices,
          "a ninth note should steal a voice, never grow the pool");

    bool foundNewNote = false;
    for (std::size_t i = 0; i < daw::Synth::kNumVoices; ++i) {
        if (synth.voice(i).midiNote() == 72) foundNewNote = true;
    }
    check(foundNewNote, "the stealing note should actually be sounding after the steal");
}

void testStealingPrefersAReleasingVoice() {
    daw::Synth synth;
    synth.prepare(48000.0, 128);
    synth.setEnvelopeParameters({1.0f, 50.0f, 0.8f, 500.0f});

    for (int note = 60; note < 68; ++note) synth.noteOn(note, 1.0f);

    // Note 60 was pressed first and is now fading out; note 61 is the oldest
    // still-held note. A naive "steal the oldest" policy would take 61.
    synth.noteOff(60);
    synth.noteOn(72, 1.0f);

    bool note60Gone = true;
    bool note61Survived = false;
    for (std::size_t i = 0; i < daw::Synth::kNumVoices; ++i) {
        const int note = synth.voice(i).midiNote();
        if (note == 60) note60Gone = false;
        if (note == 61) note61Survived = true;
    }
    check(note60Gone, "stealing should take the releasing voice");
    check(note61Survived, "stealing should not interrupt a held note while a releasing voice is available");
}

void testNoteOffReleasesOnlyTheMatchingNote() {
    daw::Synth synth;
    synth.prepare(48000.0, 128);
    synth.setEnvelopeParameters({1.0f, 50.0f, 0.8f, 500.0f});

    synth.noteOn(60, 1.0f);
    synth.noteOn(64, 1.0f);
    synth.noteOn(67, 1.0f);
    synth.noteOff(64);

    int releasingCount = 0;
    bool correctNoteReleasing = false;
    for (std::size_t i = 0; i < daw::Synth::kNumVoices; ++i) {
        const daw::Voice& voice = synth.voice(i);
        if (voice.isReleasing()) {
            ++releasingCount;
            if (voice.midiNote() == 64) correctNoteReleasing = true;
        }
    }
    check(releasingCount == 1, "note off should release exactly one voice when one voice holds that note");
    check(correctNoteReleasing, "note off should release the voice playing that specific note");
    check(synth.activeVoiceCount() == 3, "the other held notes should still be sounding");
}

void testAllNotesOffSilencesEverything() {
    daw::Synth synth;
    synth.prepare(48000.0, 128);
    for (int note = 60; note < 68; ++note) synth.noteOn(note, 1.0f);

    synth.allNotesOff();
    check(synth.activeVoiceCount() == 0, "all notes off should leave no voice active");

    std::vector<float> left(128, 0.0f), right(128, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);
    synth.process(buffer);

    bool silent = true;
    for (float sample : left) if (sample != 0.0f) silent = false;
    check(silent, "all notes off should produce actual silence, not just a zeroed voice count");
}

void testMidiNoteMapsToCorrectPitch() {
    constexpr double kSampleRate = 48000.0;
    constexpr std::size_t kNumFrames = 48000;

    daw::Synth synth;
    synth.prepare(kSampleRate, kNumFrames);
    synth.setWaveform(daw::Waveform::Sine);
    // Instant attack and full sustain keep the amplitude steady so that zero
    // crossings reflect pitch alone.
    synth.setEnvelopeParameters({0.0f, 1.0f, 1.0f, 10.0f});
    synth.setMasterGain(1.0f);
    synth.noteOn(69, 1.0f); // MIDI 69 is A440 by definition

    std::vector<float> mono(kNumFrames, 0.0f);
    float* channels[1] = {mono.data()};
    daw::AudioBuffer buffer(channels, 1, kNumFrames);
    synth.process(buffer);

    int zeroCrossings = 0;
    for (std::size_t i = 1; i < kNumFrames; ++i) {
        const bool crossedUp = mono[i - 1] < 0.0f && mono[i] >= 0.0f;
        const bool crossedDown = mono[i - 1] >= 0.0f && mono[i] < 0.0f;
        if (crossedUp || crossedDown) ++zeroCrossings;
    }
    const double measuredHz = zeroCrossings / 2.0;
    check(std::fabs(measuredHz - 440.0) < 2.0, "MIDI note 69 should sound at 440 Hz");
}

void testPolyphonicOutputStaysInRange() {
    daw::Engine engine;
    engine.prepare(48000.0, 128);
    (void)engine.addTrack();

    for (int note = 60; note < 68; ++note) {
        check(engine.pushCommand({daw::CommandType::NoteOn, 1.0f, note, 0, 0}),
              "note on command should fit in the queue");
    }

    std::vector<float> left(128), right(128);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);

    bool inRange = true;
    for (int block = 0; block < 50; ++block) {
        engine.render(buffer);
        for (float sample : left) if (sample > 1.0f || sample < -1.0f) inRange = false;
        for (float sample : right) if (sample > 1.0f || sample < -1.0f) inRange = false;
    }
    check(inRange, "eight voices at full velocity should still leave the output within [-1, 1]");
    check(engine.track(0).synth().activeVoiceCount() == 8, "note on commands should reach the synth through the queue");
}

void testNoteCommandsRoundTripThroughQueue() {
    daw::Engine engine;
    engine.prepare(48000.0, 128);
    (void)engine.addTrack();

    std::vector<float> left(128), right(128);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 128);

    check(engine.pushCommand({daw::CommandType::NoteOn, 0.9f, 60, 0, 0}), "note on should queue");
    engine.render(buffer);
    check(engine.track(0).synth().activeVoiceCount() == 1, "queued note on should start exactly one voice");

    check(engine.pushCommand({daw::CommandType::SetRelease, 5.0f, 0, 0, 0}), "release change should queue");
    check(engine.pushCommand({daw::CommandType::NoteOff, 0.0f, 60, 0, 0}), "note off should queue");
    engine.render(buffer);

    for (int block = 0; block < 20; ++block) engine.render(buffer);
    check(engine.track(0).synth().activeVoiceCount() == 0, "queued note off should let the voice finish and free itself");

    check(engine.pushCommand({daw::CommandType::AllNotesOff, 0.0f, 0, 0, 0}), "all notes off should queue");
    engine.render(buffer);
    check(true, "all notes off should drain without crashing");
}

void testOutOfRangeMidiNotesAreIgnored() {
    daw::Synth synth;
    synth.prepare(48000.0, 128);

    synth.noteOn(-1, 1.0f);
    synth.noteOn(128, 1.0f);
    synth.noteOn(9999, 1.0f);
    check(synth.activeVoiceCount() == 0,
          "out-of-range MIDI notes should be dropped rather than indexing off the frequency table");
}

// ---------------------------------------------------------------------------
// Phase 2: graph, mixer, scheduling
// ---------------------------------------------------------------------------

// Records the order in which the graph ran it.
struct OrderRecordingNode final : daw::Node {
    std::vector<int>* log = nullptr;
    int id = 0;
    void prepare(double, std::size_t) override {}
    void process(daw::AudioBuffer&) noexcept override { log->push_back(id); }
};

// A source: adds a constant to the buffer the graph cleared for it.
struct ConstantNode final : daw::Node {
    float value = 1.0f;
    void prepare(double, std::size_t) override {}
    void process(daw::AudioBuffer& buffer) noexcept override {
        for (std::size_t ch = 0; ch < buffer.numChannels(); ++ch) {
            float* data = buffer.channel(ch);
            for (std::size_t i = 0; i < buffer.numFrames(); ++i) data[i] += value;
        }
    }
};

// An effect that leaves its input mix exactly as it found it.
struct PassThroughNode final : daw::Node {
    void prepare(double, std::size_t) override {}
    void process(daw::AudioBuffer&) noexcept override {}
};

float firstSampleOf(daw::Graph& graph, std::size_t frames = 16) {
    std::vector<float> left(frames, 0.0f), right(frames, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, frames);
    graph.process(buffer);
    return left[0];
}

// Renders the engine in fixed blocks and returns the left channel, so tests
// can assert on where energy actually lands in time.
std::vector<float> renderEngine(daw::Engine& engine, std::size_t totalFrames, std::size_t blockSize) {
    std::vector<float> left(blockSize, 0.0f), right(blockSize, 0.0f);
    float* channels[2] = {left.data(), right.data()};

    std::vector<float> captured;
    captured.reserve(totalFrames);

    std::size_t done = 0;
    while (done < totalFrames) {
        const std::size_t frames = std::min(blockSize, totalFrames - done);
        daw::AudioBuffer buffer(channels, 2, frames);
        engine.render(buffer);
        for (std::size_t i = 0; i < frames; ++i) captured.push_back(left[i]);
        done += frames;
    }
    return captured;
}

float peakBetween(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    float peak = 0.0f;
    for (std::size_t i = from; i < std::min(to, samples.size()); ++i) {
        peak = std::max(peak, std::fabs(samples[i]));
    }
    return peak;
}

void testBufferSliceWritesToTheRightRegion() {
    std::vector<float> left(16, 0.0f), right(16, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer whole(channels, 2, 16);

    daw::AudioBuffer middle = whole.slice(4, 8);
    check(middle.numFrames() == 8, "a slice should report the requested length");

    for (std::size_t i = 0; i < 8; ++i) middle.channel(0)[i] = 1.0f;

    bool correct = true;
    for (std::size_t i = 0; i < 16; ++i) {
        const float expected = (i >= 4 && i < 12) ? 1.0f : 0.0f;
        if (left[i] != expected) correct = false;
    }
    check(correct, "writing through a slice should touch only the frames the slice covers");

    // Slicing a slice has to compose, since a block can be split more than
    // once when several events land in it.
    daw::AudioBuffer inner = middle.slice(2, 2);
    inner.channel(0)[0] = 5.0f;
    check(left[6] == 5.0f, "slicing a slice should offset from the outer slice, not the original buffer");
}

void testGraphOrdersDependenciesBeforeDependents() {
    std::vector<int> log;
    OrderRecordingNode a, b, c;
    a.log = &log; a.id = 0;
    b.log = &log; b.id = 1;
    c.log = &log; c.id = 2;

    daw::Graph graph;
    graph.prepare(48000.0, 128, 2);
    const auto idA = graph.addNode(&a);
    const auto idB = graph.addNode(&b);
    const auto idC = graph.addNode(&c);

    // c depends on b, b depends on a, but they were added in an order that
    // does not match, so only a real sort can get this right.
    check(graph.connect(idB, idC), "connect b to c");
    check(graph.connect(idA, idB), "connect a to b");
    graph.setOutputNode(idC);
    check(graph.rebuild(), "an acyclic graph should sort");

    std::vector<float> left(8, 0.0f), right(8, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 8);
    graph.process(buffer);

    check(log.size() == 3, "every node in the graph should run exactly once per block");
    check(log[0] == 0 && log[1] == 1 && log[2] == 2,
          "the graph should run each node only after everything feeding it");
}

void testGraphSumsFanInFromADiamond() {
    ConstantNode source;
    source.value = 1.0f;
    PassThroughNode leftBranch, rightBranch, sink;

    daw::Graph graph;
    graph.prepare(48000.0, 128, 2);
    const auto idSource = graph.addNode(&source);
    const auto idLeft = graph.addNode(&leftBranch);
    const auto idRight = graph.addNode(&rightBranch);
    const auto idSink = graph.addNode(&sink);

    check(graph.connect(idSource, idLeft), "source to left branch");
    check(graph.connect(idSource, idRight), "source to right branch");
    check(graph.connect(idLeft, idSink), "left branch to sink");
    check(graph.connect(idRight, idSink), "right branch to sink");
    graph.setOutputNode(idSink);
    check(graph.rebuild(), "a diamond is acyclic and should sort");

    check(std::fabs(firstSampleOf(graph) - 2.0f) < 1e-6f,
          "a node fed by two branches should receive their sum, not just one of them");
}

void testGraphRejectsCyclesAndKeepsRunningPlan() {
    ConstantNode source;
    source.value = 1.0f;
    PassThroughNode middle;

    daw::Graph graph;
    graph.prepare(48000.0, 128, 2);
    const auto idSource = graph.addNode(&source);
    const auto idMiddle = graph.addNode(&middle);
    check(graph.connect(idSource, idMiddle), "source to middle");
    graph.setOutputNode(idMiddle);
    check(graph.rebuild(), "the acyclic version should publish");
    check(std::fabs(firstSampleOf(graph) - 1.0f) < 1e-6f, "the acyclic graph should render");

    // Close the loop.
    check(graph.connect(idMiddle, idSource), "adding the edge itself is allowed");
    check(!graph.rebuild(), "a graph with a cycle must fail to sort rather than produce a partial order");

    // The point of refusing is that the audio thread keeps walking the last
    // good plan instead of falling silent or reading a half-written one.
    check(std::fabs(firstSampleOf(graph) - 1.0f) < 1e-6f,
          "a rejected rebuild should leave the previously published plan running");

    check(graph.disconnect(idMiddle, idSource), "removing the cycle edge should succeed");
    check(graph.rebuild(), "the graph should sort again once the cycle is gone");
}

void testGraphIgnoresNodesNotRoutedToOutput() {
    ConstantNode routed, orphan;
    routed.value = 1.0f;
    orphan.value = 100.0f;

    daw::Graph graph;
    graph.prepare(48000.0, 128, 2);
    const auto idRouted = graph.addNode(&routed);
    (void)graph.addNode(&orphan); // deliberately connected to nothing
    graph.setOutputNode(idRouted);
    check(graph.rebuild(), "an unrouted node should not prevent sorting");

    check(std::fabs(firstSampleOf(graph) - 1.0f) < 1e-6f,
          "a node with no path to the output should not contribute to the output");
}

void testGraphRebuildWhileRenderingStaysConsistent() {
    ConstantNode source;
    source.value = 0.25f;
    PassThroughNode sink;

    daw::Graph graph;
    graph.prepare(48000.0, 128, 2);
    const auto idSource = graph.addNode(&source);
    const auto idSink = graph.addNode(&sink);
    check(graph.connect(idSource, idSink), "source to sink");
    graph.setOutputNode(idSink);
    check(graph.rebuild(), "initial plan should publish");

    // A render thread hammering process() while the message thread keeps
    // republishing. Every observed sample must be one of the valid answers;
    // a torn plan would show up as something else entirely.
    std::atomic<bool> running{true};
    std::atomic<int> badSamples{0};
    std::atomic<long> blocks{0};

    std::thread audio([&] {
        std::vector<float> left(128, 0.0f), right(128, 0.0f);
        float* channels[2] = {left.data(), right.data()};
        while (running.load(std::memory_order_relaxed)) {
            daw::AudioBuffer buffer(channels, 2, 128);
            graph.process(buffer);
            for (std::size_t i = 0; i < 128; ++i) {
                const float sample = left[i];
                const bool valid = std::fabs(sample - 0.25f) < 1e-6f || std::fabs(sample - 0.5f) < 1e-6f;
                if (!valid) badSamples.fetch_add(1, std::memory_order_relaxed);
            }
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Wait for the render thread to actually be running, otherwise the edits
    // below can finish before it starts and the test proves nothing.
    while (blocks.load(std::memory_order_relaxed) < 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ConstantNode second;
    second.value = 0.25f;
    const auto idSecond = graph.addNode(&second);
    check(graph.connect(idSecond, idSink), "second source to sink");

    int republished = 0;
    for (int i = 0; i < 20; ++i) {
        if (graph.rebuild()) ++republished;
        // Let a few blocks land between swaps so the reader is genuinely
        // interleaved with the writer rather than racing it once.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const long blocksAtEnd = blocks.load(std::memory_order_relaxed);
    running.store(false, std::memory_order_relaxed);
    audio.join();

    check(blocksAtEnd > 10, "the render thread should have kept rendering across the plan swaps");

    check(republished > 0, "republishing a plan while the audio thread renders should succeed");
    check(badSamples.load() == 0,
          "every block rendered during a plan swap should reflect one complete plan, never a mix of two");
}

void testSoftClipIsBoundedContinuousAndMonotonic() {
    // Below the knee the curve must be exactly transparent, or quiet signals
    // would be coloured for no reason.
    check(daw::softClip(0.3f) == 0.3f, "soft clip should leave signals below the knee untouched");
    check(daw::softClip(-0.3f) == -0.3f, "soft clip should be transparent below the knee for negatives too");

    // Continuity of value and of slope at the knee.
    const float justBelow = daw::softClip(0.5999f);
    const float justAbove = daw::softClip(0.6001f);
    check(std::fabs(justAbove - justBelow) < 1e-3f, "soft clip should be continuous across the knee");

    const float slopeBelow = (daw::softClip(0.59f) - daw::softClip(0.58f)) / 0.01f;
    const float slopeAbove = (daw::softClip(0.62f) - daw::softClip(0.61f)) / 0.01f;
    check(std::fabs(slopeBelow - slopeAbove) < 0.05f,
          "soft clip should have no kink in its slope at the knee, which would itself be audible");

    // Bounded and order-preserving no matter how hard it is driven.
    bool bounded = true;
    bool monotonic = true;
    float previous = daw::softClip(-50.0f);
    for (float x = -50.0f; x <= 50.0f; x += 0.01f) {
        const float y = daw::softClip(x);
        if (!(y <= 1.0f && y >= -1.0f)) bounded = false;
        if (y < previous - 1e-6f) monotonic = false;
        previous = y;
    }
    check(bounded, "soft clip output should stay within [-1, 1] for any input");
    check(monotonic, "soft clip should be monotonic, so it never folds the waveform back on itself");
    check(std::fabs(daw::softClip(0.0f)) < 1e-9f, "soft clip should map silence to silence");
}

void testMasterBusSaturatesInsteadOfClipping() {
    daw::MasterBus bus;
    bus.prepare(48000.0, 128);
    bus.setGain(1.0f);
    bus.setSaturationEnabled(true);

    std::vector<float> left(512, 0.0f), right(512, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    daw::AudioBuffer buffer(channels, 2, 512);

    // Drive it well past full scale, the way eight voices at full velocity do.
    for (std::size_t i = 0; i < 512; ++i) {
        left[i] = 2.5f;
        right[i] = -2.5f;
    }
    bus.process(buffer);

    bool bounded = true;
    bool notHardClipped = false;
    for (std::size_t i = 256; i < 512; ++i) { // past the gain smoother's ramp
        if (std::fabs(left[i]) > 1.0f) bounded = false;
        if (std::fabs(left[i]) < 0.9999f) notHardClipped = true;
    }
    check(bounded, "the master bus should keep an overdriven mix inside the rails");
    check(notHardClipped,
          "saturation should round the peak off below full scale rather than sitting flat against it");
    check(bus.peakLevel() <= 1.0f, "the reported peak should reflect the post-saturation signal");
}

void testTrackPanIsConstantPower() {
    daw::Track track;
    track.prepare(48000.0, 128);
    track.setGain(1.0f);

    auto measure = [&](float pan) {
        track.setPan(pan);
        // Let the smoother settle so the measurement is the steady state.
        std::vector<float> left(4096, 0.0f), right(4096, 0.0f);
        float* channels[2] = {left.data(), right.data()};
        daw::AudioBuffer buffer(channels, 2, 4096);
        for (std::size_t i = 0; i < 4096; ++i) {
            left[i] = 1.0f;
            right[i] = 1.0f;
        }
        // Silence the synth so only the gain stage is under test.
        track.synth().allNotesOff();
        track.process(buffer);
        return std::make_pair(left[4095], right[4095]);
    };

    const auto centre = measure(0.0f);
    const float centrePower = centre.first * centre.first + centre.second * centre.second;

    const auto hardLeft = measure(-1.0f);
    const float leftPower = hardLeft.first * hardLeft.first + hardLeft.second * hardLeft.second;

    check(std::fabs(centrePower - leftPower) < 0.05f,
          "panning should hold total power steady, so the image does not dip in the middle");
    check(hardLeft.second < 0.01f, "hard left should silence the right channel");
    check(hardLeft.first > 0.9f, "hard left should put essentially all the signal in the left channel");
}

void testTracksSumIntoTheMasterBus() {
    daw::Engine engine;
    engine.prepare(48000.0, 128);

    const std::size_t first = engine.addTrack();
    const std::size_t second = engine.addTrack();
    check(first == 0 && second == 1, "tracks should be handed out in order");
    check(engine.numTracks() == 2, "the engine should report both tracks");

    // Master bus plus two tracks.
    check(engine.graph().numNodes() == 3, "each track should add exactly one node to the graph");
    check(engine.graph().planLength() == 3, "the published plan should cover every node");

    engine.track(0).setPan(0.0f);
    engine.track(1).setPan(0.0f);
    engine.track(0).synth().setEnvelopeParameters({0.0f, 1.0f, 1.0f, 5.0f});
    engine.track(1).synth().setEnvelopeParameters({0.0f, 1.0f, 1.0f, 5.0f});

    engine.track(0).synth().noteOn(60, 1.0f);
    const std::vector<float> oneTrack = renderEngine(engine, 2048, 128);
    const float onePeak = peakBetween(oneTrack, 1024, 2048);

    engine.track(1).synth().noteOn(60, 1.0f);
    const std::vector<float> twoTracks = renderEngine(engine, 2048, 128);
    const float twoPeak = peakBetween(twoTracks, 1024, 2048);

    check(onePeak > 0.01f, "a single track should be audible");
    check(twoPeak > onePeak * 1.2f, "adding a second track playing the same note should raise the level");
}

void testMuteAndSoloResolveAcrossTracks() {
    daw::Engine engine;
    engine.prepare(48000.0, 128);
    (void)engine.addTrack();
    (void)engine.addTrack();

    check(engine.track(0).isActive() && engine.track(1).isActive(), "tracks should start audible");

    check(engine.pushCommand({daw::CommandType::SetTrackMute, 0.0f, 1, 0, 0}), "queue mute");
    (void)renderEngine(engine, 128, 128);
    check(!engine.track(0).isActive(), "muting a track should take it out of the mix");
    check(engine.track(1).isActive(), "muting one track should not affect another");

    check(engine.pushCommand({daw::CommandType::SetTrackMute, 0.0f, 0, 0, 0}), "queue unmute");
    check(engine.pushCommand({daw::CommandType::SetTrackSolo, 0.0f, 1, 1, 0}), "queue solo on track 1");
    (void)renderEngine(engine, 128, 128);
    check(!engine.track(0).isActive(), "soloing one track should silence the others");
    check(engine.track(1).isActive(), "the soloed track should stay audible");

    check(engine.pushCommand({daw::CommandType::SetTrackSolo, 0.0f, 0, 1, 0}), "queue solo off");
    (void)renderEngine(engine, 128, 128);
    check(engine.track(0).isActive() && engine.track(1).isActive(),
          "clearing solo should restore every unmuted track");
}

void testScheduledNoteLandsOnTheExactSample() {
    constexpr std::size_t kBlockSize = 128;
    constexpr std::uint64_t kEventSample = 100; // deliberately mid-block

    daw::Engine engine;
    engine.prepare(48000.0, kBlockSize);
    (void)engine.addTrack();
    engine.track(0).synth().setEnvelopeParameters({0.0f, 1000.0f, 1.0f, 5.0f});
    engine.track(0).synth().setWaveform(daw::Waveform::Square);

    check(engine.pushCommand({daw::CommandType::TransportPlay, 0.0f, 0, 0, 0}), "queue play");
    check(engine.pushCommand({daw::CommandType::NoteOn, 1.0f, 69, 0, true, kEventSample}), "queue scheduled note");
    check(engine.scheduledEventCount() == 0, "nothing is scheduled until the queue is drained");

    const std::vector<float> rendered = renderEngine(engine, 512, kBlockSize);

    check(engine.scheduledEventCount() == 1, "the scheduled event should be held until its time arrives");

    // If scheduling were quantised to the block, the note would have started
    // at sample 0 or sample 128. Silence right up to 99 and energy just after
    // 100 is only possible if the block was split at the event.
    check(peakBetween(rendered, 0, kEventSample) == 0.0f,
          "nothing should sound before a scheduled event's exact sample");
    check(peakBetween(rendered, kEventSample, kEventSample + 40) > 0.01f,
          "a scheduled note should start within a few samples of its timestamp, not at the next block boundary");
}

void testScheduledEventsAreIgnoredWhileStopped() {
    daw::Engine engine;
    engine.prepare(48000.0, 128);
    (void)engine.addTrack();
    engine.track(0).synth().setWaveform(daw::Waveform::Square);

    // No TransportPlay: the playhead never reaches the event.
    check(engine.pushCommand({daw::CommandType::NoteOn, 1.0f, 69, 0, true, 100}), "queue scheduled note");
    const std::vector<float> rendered = renderEngine(engine, 1024, 128);

    check(peakBetween(rendered, 0, 1024) == 0.0f,
          "a scheduled event should not fire while the transport is stopped");
    check(engine.transport().position() == 0, "a stopped transport should not advance");
}

void testTransportLoopWrapsAndRearmsEvents() {
    constexpr std::uint64_t kLoopLength = 2000;

    daw::Engine engine;
    engine.prepare(48000.0, 128);
    (void)engine.addTrack();
    engine.track(0).synth().setWaveform(daw::Waveform::Square);
    engine.track(0).synth().setEnvelopeParameters({0.0f, 1000.0f, 1.0f, 1.0f});

    engine.transport().setLoop(0, kLoopLength);
    check(engine.pushCommand({daw::CommandType::TransportSetLoop, 0.0f, 1, 0, 0}), "queue loop on");
    check(engine.pushCommand({daw::CommandType::TransportPlay, 0.0f, 0, 0, 0}), "queue play");
    check(engine.pushCommand({daw::CommandType::NoteOn, 1.0f, 69, 0, true, 100}), "queue note on at 100");
    check(engine.pushCommand({daw::CommandType::NoteOff, 0.0f, 69, 0, true, 300}), "queue note off at 300");

    const std::vector<float> rendered = renderEngine(engine, kLoopLength * 3, 128);

    // The same event must sound once per pass, which only happens if looping
    // re-arms events inside the loop range.
    bool everyPassSounded = true;
    bool everyPassWentQuiet = true;
    for (std::uint64_t pass = 0; pass < 3; ++pass) {
        const std::size_t base = static_cast<std::size_t>(pass * kLoopLength);
        if (peakBetween(rendered, base + 120, base + 290) < 0.01f) everyPassSounded = false;
        if (peakBetween(rendered, base + 700, base + 1900) > 0.01f) everyPassWentQuiet = false;
    }
    check(everyPassSounded, "an event inside the loop range should fire again on every pass");
    check(everyPassWentQuiet, "the gap after the note should stay silent on every pass");
    check(engine.transport().position() < kLoopLength, "the playhead should wrap inside the loop range");
}

// Regression: an event on sample zero is an ordinary timeline position, not a
// live gesture. Overloading a zero timestamp to mean "immediate" made the
// first event of a loop fire once and never repeat, which only showed up as a
// missing note on passes two onward.
void testEventOnSampleZeroRepeatsEveryLoopPass() {
    constexpr std::uint64_t kLoopLength = 2000;

    daw::Engine engine;
    engine.prepare(48000.0, 128);
    (void)engine.addTrack();
    engine.track(0).synth().setWaveform(daw::Waveform::Square);
    engine.track(0).synth().setEnvelopeParameters({0.0f, 1000.0f, 1.0f, 1.0f});

    engine.transport().setLoop(0, kLoopLength);
    check(engine.pushCommand({daw::CommandType::TransportSetLoop, 0.0f, 1, 0, false, 0}), "queue loop on");
    check(engine.pushCommand({daw::CommandType::TransportPlay, 0.0f, 0, 0, false, 0}), "queue play");

    // The distinguishing case: timestamp zero, explicitly scheduled.
    check(engine.pushCommand({daw::CommandType::NoteOn, 1.0f, 69, 0, true, 0}), "queue note on at sample zero");
    check(engine.pushCommand({daw::CommandType::NoteOff, 0.0f, 69, 0, true, 400}), "queue note off");

    const std::vector<float> rendered = renderEngine(engine, kLoopLength * 3, 128);
    check(engine.scheduledEventCount() == 2, "an event at sample zero should live in the schedule, not fire immediately");

    bool everyPassSounded = true;
    for (std::uint64_t pass = 0; pass < 3; ++pass) {
        const std::size_t base = static_cast<std::size_t>(pass * kLoopLength);
        if (peakBetween(rendered, base + 20, base + 380) < 0.01f) everyPassSounded = false;
    }
    check(everyPassSounded, "a note scheduled on sample zero should sound on every loop pass, not just the first");
}

// ---------------------------------------------------------------------------
// Phase 1: the real-time guarantee itself
// ---------------------------------------------------------------------------

void testAudioThreadNeverAllocates() {
#if DAW_TRACK_ALLOCATIONS
    daw::Engine engine;
    engine.prepare(48000.0, 512); // prepare() is allowed to allocate
    (void)engine.addTrack();      // so is building the graph plan
    (void)engine.addTrack();

    // Everything the measured section touches is allocated up front, so any
    // counted allocation can only have come from the engine.
    std::vector<float> left(512, 0.0f);
    std::vector<float> right(512, 0.0f);
    float* channels[2] = {left.data(), right.data()};
    const std::size_t blockSizes[] = {128, 64, 512, 7, 256};

    // One untimed pass first: this is where any lazy one-time setup inside
    // the engine would show up, and it would be unfair to blame the steady
    // state for it.
    daw::AudioBuffer warmup(channels, 2, 128);
    engine.render(warmup);

    // Guard against the test passing for the wrong reason. If the override
    // above were not actually linked in, the counter would sit at zero and
    // the comparison below would succeed no matter what the engine did.
    check(gAllocationCount.load(std::memory_order_relaxed) > 0,
          "allocation counter should have already seen the harness's own allocations, proving it is live");

    const long before = gAllocationCount.load(std::memory_order_relaxed);

    for (int block = 0; block < 400; ++block) {
        // Exercise the paths most likely to allocate: note starts, note ends,
        // pool exhaustion and stealing, and parameter changes through the
        // queue, all while the block size keeps changing underneath.
        const int note = 48 + (block % 40);
        (void)engine.pushCommand({daw::CommandType::NoteOn, 0.8f, note, static_cast<std::uint8_t>(block % 2), 0});
        if (block % 3 == 0) {
            (void)engine.pushCommand({daw::CommandType::NoteOff, 0.0f, 48 + ((block - 3) % 40), 0, 0});
        }
        if (block % 7 == 0) {
            (void)engine.pushCommand({daw::CommandType::SetFilterCutoff, 200.0f + static_cast<float>(block), 0, 0, 0});
            (void)engine.pushCommand({daw::CommandType::SetAttack, 1.0f + static_cast<float>(block % 50), 0, 0, 0});
            (void)engine.pushCommand({daw::CommandType::SetWaveform, 0.0f, block % 3, 0, 0});
            (void)engine.pushCommand({daw::CommandType::SetTrackPan, (block % 2 == 0) ? -0.5f : 0.5f, 0, 1, 0});
        }
        if (block % 97 == 0) {
            (void)engine.pushCommand({daw::CommandType::AllNotesOff, 0.0f, 0, 0, 0});
        }

        daw::AudioBuffer buffer(channels, 2, blockSizes[block % 5]);
        engine.render(buffer);
    }

    const long after = gAllocationCount.load(std::memory_order_relaxed);

    check(after == before, "rendering must not allocate: the audio thread cannot afford a malloc");
    if (after != before) {
        std::fprintf(stderr, "  (%ld allocation(s) during render)\n", after - before);
    }
#else
    check(true, "allocation tracking is disabled under sanitizer builds, which supply their own operator new");
#endif
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

    testEnvelopeIdleIsSilent();
    testEnvelopeAttackReachesPeak();
    testEnvelopeDecaysToSustainAndHolds();
    testEnvelopeReleaseTerminatesExactly();
    testEnvelopeReleaseTimeIndependentOfLevel();
    testEnvelopeZeroSustainFreesVoice();
    testEnvelopeRetriggerDoesNotSnapToZero();

    testLowPassPassesLowAndBlocksHigh();
    testHighPassPassesHighAndBlocksLow();
    testBandPassPeaksAtCentre();
    testFilterStaysStableAtExtremes();

    testIdleVoiceAddsNothing();
    testVoiceSoundsThenFreesItself();
    testVoiceOutputIsAdditive();

    testEightVoicePolyphony();
    testNinthNoteStealsRatherThanOverflows();
    testStealingPrefersAReleasingVoice();
    testNoteOffReleasesOnlyTheMatchingNote();
    testAllNotesOffSilencesEverything();
    testMidiNoteMapsToCorrectPitch();
    testPolyphonicOutputStaysInRange();
    testNoteCommandsRoundTripThroughQueue();
    testOutOfRangeMidiNotesAreIgnored();

    testBufferSliceWritesToTheRightRegion();
    testGraphOrdersDependenciesBeforeDependents();
    testGraphSumsFanInFromADiamond();
    testGraphRejectsCyclesAndKeepsRunningPlan();
    testGraphIgnoresNodesNotRoutedToOutput();
    testGraphRebuildWhileRenderingStaysConsistent();
    testSoftClipIsBoundedContinuousAndMonotonic();
    testMasterBusSaturatesInsteadOfClipping();
    testTrackPanIsConstantPower();
    testTracksSumIntoTheMasterBus();
    testMuteAndSoloResolveAcrossTracks();
    testScheduledNoteLandsOnTheExactSample();
    testScheduledEventsAreIgnoredWhileStopped();
    testTransportLoopWrapsAndRearmsEvents();
    testEventOnSampleZeroRepeatsEveryLoopPass();

    testAudioThreadNeverAllocates();

    std::printf("%d checks run, %d failed\n", checksRun, checksFailed);
    return checksFailed == 0 ? 0 : 1;
}
