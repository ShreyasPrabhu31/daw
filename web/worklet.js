// Runs on the audio thread. Has no fetch and no DOM, so the wasm bytes
// arrive via postMessage from the main thread instead of being fetched here.
//
// This handler and process() are interleaved on the same thread rather than
// concurrent, so calling into the engine from here is safe. It is still the
// audio thread though: nothing in here may block, and graph topology is fixed
// at init for exactly that reason.
const NUM_TRACKS = 4;

class DawProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false;
    this.exports = null;
    this.leftView = null;
    this.rightView = null;
    this.maxBlockFrames = 1024;
    this.blocksSinceReport = 0;

    // Message name to wasm export, for the calls that take (track, value).
    this.trackSetters = {
      setWaveform: 'daw_set_waveform',
      setAttack: 'daw_set_attack',
      setDecay: 'daw_set_decay',
      setSustain: 'daw_set_sustain',
      setRelease: 'daw_set_release',
      setFilterType: 'daw_set_filter_type',
      setFilterCutoff: 'daw_set_filter_cutoff',
      setFilterResonance: 'daw_set_filter_resonance',
      setTrackGain: 'daw_set_track_gain',
      setTrackPan: 'daw_set_track_pan',
      setTrackMute: 'daw_set_track_mute',
      setTrackSolo: 'daw_set_track_solo',
    };

    // Calls that take a single value and ignore the track.
    this.globalSetters = {
      setMasterGain: 'daw_set_master_gain',
      setMasterSaturation: 'daw_set_master_saturation',
      transportSetLoop: 'daw_transport_set_loop',
      transportSetPosition: 'daw_transport_set_position',
    };

    this.port.onmessage = (event) => this._handle(event.data);
  }

  _handle(data) {
    const { type } = data;

    if (type === 'init') {
      this._init(data.wasmBytes);
      return;
    }
    if (!this.exports) return;

    switch (type) {
      case 'noteOn':
        this.exports.daw_note_on(data.track, data.note, data.velocity);
        return;
      case 'noteOff':
        this.exports.daw_note_off(data.track, data.note);
        return;
      case 'allNotesOff':
        this.exports.daw_all_notes_off();
        return;
      case 'setTimeline':
        this._setTimeline(data.notes);
        return;
      case 'setTempo':
        this.exports.daw_set_tempo(data.value);
        return;
      case 'setLoopTicks':
        this.exports.daw_set_loop_ticks(data.start, data.end);
        return;
      case 'transportPlay':
        this.exports.daw_transport_play();
        return;
      case 'transportStop':
        this.exports.daw_transport_stop();
        return;
      default:
        break;
    }

    if (this.trackSetters[type]) {
      this.exports[this.trackSetters[type]](data.track, data.value);
    } else if (this.globalSetters[type]) {
      this.exports[this.globalSetters[type]](data.value);
    }
  }

  // Rewrites the whole arrangement. Cheaper and far simpler than diffing, and
  // the engine sorts it into a schedule once at the end rather than per note.
  _setTimeline(notes) {
    this.exports.daw_timeline_clear();
    for (const note of notes) {
      this.exports.daw_timeline_add_note(note.track, note.pitch, note.startTick, note.lengthTicks, note.velocity);
    }

    // A refusal means the audio thread still holds the slot being rewritten,
    // so the caller retries rather than the engine blocking here.
    const ok = this.exports.daw_timeline_compile() === 1;
    this.port.postMessage({
      type: 'compiled',
      ok,
      notes: this.exports.daw_timeline_note_count(),
      events: this.exports.daw_timeline_event_count(),
    });
  }

  async _init(wasmBytes) {
    const { instance } = await WebAssembly.instantiate(wasmBytes, {});
    this.exports = instance.exports;

    // Standalone wasm: static constructors run via the explicit _initialize
    // export, which must happen before any other call touches global state.
    if (typeof this.exports._initialize === 'function') {
      this.exports._initialize();
    }

    const tracks = this.exports.daw_init(sampleRate, NUM_TRACKS);

    // ALLOW_MEMORY_GROWTH=0 means this buffer never detaches, so these views
    // are built once here and reused for the processor's lifetime.
    const leftPtr = this.exports.daw_get_channel_ptr(0);
    const rightPtr = this.exports.daw_get_channel_ptr(1);
    const memory = this.exports.memory.buffer;
    this.leftView = new Float32Array(memory, leftPtr, this.maxBlockFrames);
    this.rightView = new Float32Array(memory, rightPtr, this.maxBlockFrames);

    this.ready = true;
    this.port.postMessage({ type: 'ready', tracks });
  }

  process(_inputs, outputs) {
    const output = outputs[0];
    const numFrames = output[0].length;

    // Hosts do not promise a constant block size; fall back to silence
    // rather than assume 128 or overrun the fixed-size wasm buffers.
    if (!this.ready || numFrames > this.maxBlockFrames) {
      for (const channel of output) channel.fill(0);
      return true;
    }

    this.exports.daw_render(numFrames);

    output[0].set(this.leftView.subarray(0, numFrames));
    if (output.length > 1) {
      output[1].set(this.rightView.subarray(0, numFrames));
    }

    // Meter roughly 20 times a second. Posting every block would flood the
    // main thread with messages it cannot paint anywhere near that fast.
    if (++this.blocksSinceReport >= 16) {
      this.blocksSinceReport = 0;

      const voices = [];
      const peaks = [];
      for (let i = 0; i < NUM_TRACKS; ++i) {
        voices.push(this.exports.daw_active_voice_count(i));
        peaks.push(this.exports.daw_track_peak(i));
      }

      this.port.postMessage({
        type: 'meter',
        voices,
        peaks,
        peak: this.exports.daw_peak_level(),
        position: this.exports.daw_transport_position(),
        playing: this.exports.daw_transport_is_playing() === 1,
      });
    }

    return true;
  }
}

registerProcessor('daw-processor', DawProcessor);
