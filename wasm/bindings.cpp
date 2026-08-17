#include "daw/Engine.hpp"

// Standalone WASM export surface. No JS glue: the worklet calls these by
// name after running the module's own static constructors via _initialize.
// Buffers are static, not heap-allocated, so daw_render can run on the audio
// thread without a single malloc.
//
// Every setter goes through the engine's command queue rather than touching
// the synth directly, because these are called from the worklet's message
// handler, which is not the same context as the render callback.
extern "C" {

constexpr std::size_t kMaxBlockFrames = 1024;
constexpr std::size_t kNumChannels = 2;

static daw::Engine gEngine;
static float gLeft[kMaxBlockFrames];
static float gRight[kMaxBlockFrames];
static float* gChannels[kNumChannels] = {gLeft, gRight};

static void push(daw::CommandType type, float floatValue, std::int32_t intValue) {
    // A dropped command means the queue is full, which at UI rates means
    // something upstream is spamming; losing one parameter update is the
    // right failure mode versus blocking the caller.
    (void)gEngine.pushCommand({type, floatValue, intValue});
}

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

__attribute__((export_name("daw_note_on")))
void daw_note_on(int midiNote, float velocity) {
    push(daw::CommandType::NoteOn, velocity, midiNote);
}

__attribute__((export_name("daw_note_off")))
void daw_note_off(int midiNote) {
    push(daw::CommandType::NoteOff, 0.0f, midiNote);
}

__attribute__((export_name("daw_all_notes_off")))
void daw_all_notes_off() {
    push(daw::CommandType::AllNotesOff, 0.0f, 0);
}

__attribute__((export_name("daw_set_waveform")))
void daw_set_waveform(int waveform) {
    push(daw::CommandType::SetWaveform, 0.0f, waveform);
}

__attribute__((export_name("daw_set_master_gain")))
void daw_set_master_gain(float gain) {
    push(daw::CommandType::SetMasterGain, gain, 0);
}

__attribute__((export_name("daw_set_attack")))
void daw_set_attack(float milliseconds) {
    push(daw::CommandType::SetAttack, milliseconds, 0);
}

__attribute__((export_name("daw_set_decay")))
void daw_set_decay(float milliseconds) {
    push(daw::CommandType::SetDecay, milliseconds, 0);
}

__attribute__((export_name("daw_set_sustain")))
void daw_set_sustain(float level) {
    push(daw::CommandType::SetSustain, level, 0);
}

__attribute__((export_name("daw_set_release")))
void daw_set_release(float milliseconds) {
    push(daw::CommandType::SetRelease, milliseconds, 0);
}

__attribute__((export_name("daw_set_filter_type")))
void daw_set_filter_type(int type) {
    push(daw::CommandType::SetFilterType, 0.0f, type);
}

__attribute__((export_name("daw_set_filter_cutoff")))
void daw_set_filter_cutoff(float hz) {
    push(daw::CommandType::SetFilterCutoff, hz, 0);
}

__attribute__((export_name("daw_set_filter_resonance")))
void daw_set_filter_resonance(float q) {
    push(daw::CommandType::SetFilterResonance, q, 0);
}

// Lets the UI show how many voices the pool is actually using, which is the
// only way to see voice stealing happen from outside the engine.
__attribute__((export_name("daw_active_voice_count")))
int daw_active_voice_count() {
    return static_cast<int>(gEngine.synth().activeVoiceCount());
}

__attribute__((export_name("daw_peak_level")))
float daw_peak_level() {
    return gEngine.peakLevel();
}

__attribute__((export_name("daw_render")))
void daw_render(int numFrames) {
    if (numFrames <= 0 || static_cast<std::size_t>(numFrames) > kMaxBlockFrames) return;
    daw::AudioBuffer buffer(gChannels, kNumChannels, static_cast<std::size_t>(numFrames));
    gEngine.render(buffer);
}

} // extern "C"
