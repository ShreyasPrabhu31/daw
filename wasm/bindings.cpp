#include "daw/Engine.hpp"

// Standalone WASM export surface. No JS glue: the worklet calls these by
// name after running the module's own static constructors via _initialize.
// Buffers are static, not heap-allocated, so daw_render can run on the audio
// thread without a single malloc.
extern "C" {

constexpr std::size_t kMaxBlockFrames = 1024;
constexpr std::size_t kNumChannels = 2;

static daw::Engine gEngine;
static float gLeft[kMaxBlockFrames];
static float gRight[kMaxBlockFrames];
static float* gChannels[kNumChannels] = {gLeft, gRight};

__attribute__((export_name("daw_init")))
void daw_init(double sampleRate) {
    gEngine.prepare(sampleRate, kMaxBlockFrames);
}

// Lets the worklet build Float32Array views directly over wasm linear
// memory once, instead of copying samples across the JS/WASM boundary
// every block.
__attribute__((export_name("daw_get_channel_ptr")))
float* daw_get_channel_ptr(int channel) {
    return gChannels[static_cast<std::size_t>(channel)];
}

__attribute__((export_name("daw_set_frequency")))
void daw_set_frequency(float hz) {
    (void)gEngine.pushCommand({daw::CommandType::SetFrequency, hz, 0});
}

__attribute__((export_name("daw_set_waveform")))
void daw_set_waveform(int waveform) {
    (void)gEngine.pushCommand({daw::CommandType::SetWaveform, 0.0f, waveform});
}

__attribute__((export_name("daw_set_gain")))
void daw_set_gain(float gain) {
    (void)gEngine.pushCommand({daw::CommandType::SetGain, gain, 0});
}

__attribute__((export_name("daw_render")))
void daw_render(int numFrames) {
    if (numFrames <= 0 || static_cast<std::size_t>(numFrames) > kMaxBlockFrames) return;
    daw::AudioBuffer buffer(gChannels, kNumChannels, static_cast<std::size_t>(numFrames));
    gEngine.render(buffer);
}

} // extern "C"
