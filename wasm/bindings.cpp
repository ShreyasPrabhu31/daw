#include "daw/Engine.hpp"
#include "daw/Timeline.hpp"

// Standalone WASM export surface. No JS glue: the worklet calls these by
// name after running the module's own static constructors via _initialize.
// Buffers are static, not heap-allocated, so daw_render can run on the audio
// thread without a single malloc.
//
// Threading note. In an AudioWorklet the port's message handler and process()
// both run in the audio rendering thread, interleaved rather than concurrent.
// That makes parameter setters safe to call directly, but it also means a
// graph edit here would run where blocking is forbidden, and Graph::rebuild
// may wait for the audio thread to release a plan slot. So every track is
// created inside daw_init, before any rendering starts, and the topology is
// never touched again from the browser.
//
// The arrangement is edited directly rather than through the command queue.
// Editing is message-thread work, and in a worklet the message handler and
// process() are interleaved on one thread rather than concurrent, so a direct
// call is safe here and avoids pushing a whole song through a 256-slot queue.
//
// Sample counts cross the boundary as double rather than i64 deliberately: a
// standalone module with no glue would otherwise force BigInt on every call,
// and a double holds an exact integer sample count for about 5700 years at
// 48 kHz.
extern "C" {

constexpr std::size_t kMaxBlockFrames = 1024;
constexpr std::size_t kNumChannels = 2;

static daw::Engine gEngine;
static float gLeft[kMaxBlockFrames];
static float gRight[kMaxBlockFrames];
static float* gChannels[kNumChannels] = {gLeft, gRight};

static void push(daw::CommandType type, float floatValue, std::int32_t intValue, int track) {
    daw::EngineCommand command{};
    command.type = type;
    command.floatValue = floatValue;
    command.intValue = intValue;
    command.track = static_cast<std::uint8_t>(track < 0 ? 0 : track);
    // A dropped command means the queue is full, which at UI rates means
    // something upstream is spamming; losing one update beats blocking.
    (void)gEngine.pushCommand(command);
}

__attribute__((export_name("daw_init")))
int daw_init(double sampleRate, int numTracks) {
    gEngine.prepare(sampleRate, kMaxBlockFrames, kNumChannels);

    int created = 0;
    for (int i = 0; i < numTracks; ++i) {
        if (gEngine.addTrack() == daw::Engine::kMaxTracks) break;
        ++created;
    }
    return created;
}

// Lets the worklet build Float32Array views directly over wasm linear
// memory once, instead of copying samples across the boundary every block.
__attribute__((export_name("daw_get_channel_ptr")))
float* daw_get_channel_ptr(int channel) {
    return gChannels[static_cast<std::size_t>(channel)];
}

__attribute__((export_name("daw_note_on")))
void daw_note_on(int track, int midiNote, float velocity) {
    push(daw::CommandType::NoteOn, velocity, midiNote, track);
}

__attribute__((export_name("daw_note_off")))
void daw_note_off(int track, int midiNote) {
    push(daw::CommandType::NoteOff, 0.0f, midiNote, track);
}

__attribute__((export_name("daw_all_notes_off")))
void daw_all_notes_off() {
    push(daw::CommandType::AllNotesOff, 0.0f, 0, 0);
}

// --- arrangement ---

__attribute__((export_name("daw_timeline_clear")))
void daw_timeline_clear() {
    gEngine.timeline().clear();
}

__attribute__((export_name("daw_timeline_clear_track")))
void daw_timeline_clear_track(int track) {
    gEngine.timeline().clearTrack(static_cast<std::uint8_t>(track < 0 ? 0 : track));
}

__attribute__((export_name("daw_timeline_add_note")))
int daw_timeline_add_note(int track, int pitch, double startTick, double lengthTicks, float velocity) {
    daw::Note note{};
    note.track = static_cast<std::uint8_t>(track < 0 ? 0 : track);
    note.pitch = static_cast<std::uint8_t>(pitch < 0 ? 0 : (pitch > 127 ? 127 : pitch));
    note.startTick = static_cast<std::uint32_t>(startTick < 0.0 ? 0.0 : startTick);
    note.lengthTicks = static_cast<std::uint32_t>(lengthTicks < 1.0 ? 1.0 : lengthTicks);
    note.velocity = velocity;
    return gEngine.timeline().addNote(note) ? 1 : 0;
}

// Returns 0 when the audio thread still holds the slot being rewritten. The
// caller should retry; the running arrangement keeps playing meanwhile.
__attribute__((export_name("daw_timeline_compile")))
int daw_timeline_compile() {
    return gEngine.compileTimeline() ? 1 : 0;
}

__attribute__((export_name("daw_timeline_note_count")))
int daw_timeline_note_count() {
    return static_cast<int>(gEngine.timeline().noteCount());
}

__attribute__((export_name("daw_timeline_event_count")))
int daw_timeline_event_count() {
    return static_cast<int>(gEngine.scheduledEventCount());
}

__attribute__((export_name("daw_set_tempo")))
void daw_set_tempo(double bpm) {
    gEngine.transport().setTempo(bpm);
}

__attribute__((export_name("daw_ticks_per_quarter")))
int daw_ticks_per_quarter() {
    return static_cast<int>(daw::Timeline::kTicksPerQuarter);
}

__attribute__((export_name("daw_set_waveform")))
void daw_set_waveform(int track, int waveform) {
    push(daw::CommandType::SetWaveform, 0.0f, waveform, track);
}

__attribute__((export_name("daw_set_attack")))
void daw_set_attack(int track, float milliseconds) {
    push(daw::CommandType::SetAttack, milliseconds, 0, track);
}

__attribute__((export_name("daw_set_decay")))
void daw_set_decay(int track, float milliseconds) {
    push(daw::CommandType::SetDecay, milliseconds, 0, track);
}

__attribute__((export_name("daw_set_sustain")))
void daw_set_sustain(int track, float level) {
    push(daw::CommandType::SetSustain, level, 0, track);
}

__attribute__((export_name("daw_set_release")))
void daw_set_release(int track, float milliseconds) {
    push(daw::CommandType::SetRelease, milliseconds, 0, track);
}

__attribute__((export_name("daw_set_filter_type")))
void daw_set_filter_type(int track, int type) {
    push(daw::CommandType::SetFilterType, 0.0f, type, track);
}

__attribute__((export_name("daw_set_filter_cutoff")))
void daw_set_filter_cutoff(int track, float hz) {
    push(daw::CommandType::SetFilterCutoff, hz, 0, track);
}

__attribute__((export_name("daw_set_filter_resonance")))
void daw_set_filter_resonance(int track, float q) {
    push(daw::CommandType::SetFilterResonance, q, 0, track);
}

__attribute__((export_name("daw_set_track_gain")))
void daw_set_track_gain(int track, float gain) {
    push(daw::CommandType::SetTrackGain, gain, 0, track);
}

__attribute__((export_name("daw_set_track_pan")))
void daw_set_track_pan(int track, float pan) {
    push(daw::CommandType::SetTrackPan, pan, 0, track);
}

__attribute__((export_name("daw_set_track_mute")))
void daw_set_track_mute(int track, int muted) {
    push(daw::CommandType::SetTrackMute, 0.0f, muted, track);
}

__attribute__((export_name("daw_set_track_solo")))
void daw_set_track_solo(int track, int soloed) {
    push(daw::CommandType::SetTrackSolo, 0.0f, soloed, track);
}

__attribute__((export_name("daw_set_master_gain")))
void daw_set_master_gain(float gain) {
    push(daw::CommandType::SetMasterGain, gain, 0, 0);
}

__attribute__((export_name("daw_set_master_saturation")))
void daw_set_master_saturation(int enabled) {
    push(daw::CommandType::SetMasterSaturation, 0.0f, enabled, 0);
}

__attribute__((export_name("daw_transport_play")))
void daw_transport_play() {
    push(daw::CommandType::TransportPlay, 0.0f, 0, 0);
}

__attribute__((export_name("daw_transport_stop")))
void daw_transport_stop() {
    push(daw::CommandType::TransportStop, 0.0f, 0, 0);
}

__attribute__((export_name("daw_transport_set_position")))
void daw_transport_set_position(double samples) {
    push(daw::CommandType::TransportSetPosition, 0.0f, static_cast<std::int32_t>(samples), 0);
}

__attribute__((export_name("daw_transport_set_loop")))
void daw_transport_set_loop(int enabled) {
    push(daw::CommandType::TransportSetLoop, 0.0f, enabled, 0);
}

// Loop bounds are musical, so they survive a tempo change without the host
// recomputing them. They go straight to the transport rather than through the
// queue because they must be in place before the enable command takes effect.
__attribute__((export_name("daw_set_loop_ticks")))
void daw_set_loop_ticks(double startTick, double endTick) {
    gEngine.setLoopTicks(static_cast<std::uint32_t>(startTick), static_cast<std::uint32_t>(endTick));
}

__attribute__((export_name("daw_transport_position")))
double daw_transport_position() {
    return static_cast<double>(gEngine.transport().position());
}

__attribute__((export_name("daw_transport_is_playing")))
int daw_transport_is_playing() {
    return gEngine.transport().isPlaying() ? 1 : 0;
}

__attribute__((export_name("daw_active_voice_count")))
int daw_active_voice_count(int track) {
    if (track < 0 || static_cast<std::size_t>(track) >= gEngine.numTracks()) return 0;
    return static_cast<int>(gEngine.track(static_cast<std::size_t>(track)).synth().activeVoiceCount());
}

__attribute__((export_name("daw_peak_level")))
float daw_peak_level() {
    return gEngine.peakLevel();
}

__attribute__((export_name("daw_track_peak")))
float daw_track_peak(int track) {
    if (track < 0 || static_cast<std::size_t>(track) >= gEngine.numTracks()) return 0.0f;
    return gEngine.track(static_cast<std::size_t>(track)).peakLevel();
}

__attribute__((export_name("daw_num_tracks")))
int daw_num_tracks() {
    return static_cast<int>(gEngine.numTracks());
}

__attribute__((export_name("daw_render")))
void daw_render(int numFrames) {
    if (numFrames <= 0 || static_cast<std::size_t>(numFrames) > kMaxBlockFrames) return;
    daw::AudioBuffer buffer(gChannels, kNumChannels, static_cast<std::size_t>(numFrames));
    gEngine.render(buffer);
}

} // extern "C"
