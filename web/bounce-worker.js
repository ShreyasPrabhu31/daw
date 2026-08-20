// Renders the arrangement offline, in a Worker, using a second instance of the
// same engine the worklet is playing.
//
// This is the third host for the same C++ core, and the point of it is that it
// is not bound to the clock: it renders as fast as the CPU allows rather than
// in 2.67 ms slices. Doing it in a Worker keeps the work off both the audio
// thread (where it would blow the deadline) and the main thread (where it
// would freeze the page while dragging a clip).
let exports = null;

const BLOCK = 128;
const CHANNELS = 2;

async function instantiate(wasmBytes) {
  // The module is standalone and imports nothing, so an empty import object
  // is the whole host interface.
  const { instance } = await WebAssembly.instantiate(wasmBytes, {});
  exports = instance.exports;
  if (typeof exports._initialize === 'function') exports._initialize();
}

function applyTrack(track, settings) {
  exports.daw_set_waveform(track, settings.waveform);
  exports.daw_set_attack(track, settings.attack);
  exports.daw_set_decay(track, settings.decay);
  exports.daw_set_sustain(track, settings.sustain);
  exports.daw_set_release(track, settings.release);
  exports.daw_set_filter_type(track, settings.filterType);
  exports.daw_set_filter_cutoff(track, settings.cutoff);
  exports.daw_set_filter_resonance(track, settings.resonance);
  exports.daw_set_track_gain(track, settings.gain);
  exports.daw_set_track_pan(track, settings.pan);
  exports.daw_set_track_mute(track, settings.muted ? 1 : 0);
  exports.daw_set_track_solo(track, settings.soloed ? 1 : 0);
}

async function bounce(request) {
  const { wasmBytes, sampleRate, numTracks, bpm, notes, tracks, master, lengthSamples } = request;

  if (!exports) await instantiate(wasmBytes);

  exports.daw_init(sampleRate, numTracks);
  exports.daw_set_tempo(bpm);
  for (let t = 0; t < numTracks; ++t) applyTrack(t, tracks[t]);
  exports.daw_set_master_gain(master.gain);
  exports.daw_set_master_saturation(master.saturation ? 1 : 0);

  exports.daw_timeline_clear();
  for (const note of notes) {
    exports.daw_timeline_add_note(note.track, note.pitch, note.startTick, note.lengthTicks, note.velocity);
  }
  if (exports.daw_timeline_compile() !== 1) {
    self.postMessage({ type: 'error', message: 'could not compile the arrangement' });
    return;
  }

  // Looping stays off: a bounce should render the arrangement once, not
  // forever.
  exports.daw_transport_set_loop(0);
  exports.daw_transport_set_position(0);
  exports.daw_transport_play();

  const left = new Float32Array(lengthSamples);
  const right = new Float32Array(lengthSamples);

  const leftPtr = exports.daw_get_channel_ptr(0);
  const rightPtr = exports.daw_get_channel_ptr(1);
  const memory = exports.memory.buffer;
  const leftView = new Float32Array(memory, leftPtr, BLOCK);
  const rightView = new Float32Array(memory, rightPtr, BLOCK);

  const started = performance.now();

  let done = 0;
  while (done < lengthSamples) {
    const frames = Math.min(BLOCK, lengthSamples - done);
    exports.daw_render(frames);
    left.set(leftView.subarray(0, frames), done);
    right.set(rightView.subarray(0, frames), done);
    done += frames;
  }

  const elapsedMs = performance.now() - started;
  const audioSeconds = lengthSamples / sampleRate;

  self.postMessage(
    {
      type: 'bounced',
      left,
      right,
      sampleRate,
      elapsedMs,
      audioSeconds,
      // The headline number for an offline renderer: how much faster than the
      // clock it managed.
      realtimeFactor: audioSeconds / (elapsedMs / 1000),
    },
    [left.buffer, right.buffer],
  );
}

self.onmessage = (event) => {
  if (event.data.type === 'bounce') {
    bounce(event.data).catch((error) => {
      self.postMessage({ type: 'error', message: String(error && error.message ? error.message : error) });
    });
  }
};
