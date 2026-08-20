// Main thread: the only place that can fetch(). Wasm bytes are fetched here
// and handed to both the worklet (live playback) and the bounce worker
// (offline render) as transferable ArrayBuffers.
const SAMPLE_RATE = 48000;
const LOWEST_NOTE = 48; // C3
const COMPUTER_KEY_BASE = LOWEST_NOTE + 12;
const NUM_KEYS = 29;

const KEY_TO_OFFSET = {
  a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6,
  g: 7, y: 8, h: 9, u: 10, j: 11, k: 12,
  o: 13, l: 14, p: 15, ';': 16,
};

let audioContext = null;
let workletNode = null;
let wasmBytesForWorker = null;
let bounceWorker = null;

let armedTrack = 0;
let bpm = 110;
let arrangement = null;
let pianoRoll = null;
let peakCache = null;

const heldNotes = new Set();

function isBlackKey(note) {
  return [1, 3, 6, 8, 10].includes(note % 12);
}

function send(message) {
  if (workletNode) workletNode.port.postMessage(message);
}

function setStatus(text) {
  document.getElementById('status').textContent = text;
}

async function startAudio() {
  if (audioContext) return;

  setStatus('loading');
  audioContext = new AudioContext({ sampleRate: SAMPLE_RATE });
  await audioContext.audioWorklet.addModule('worklet.js');

  const response = await fetch('engine.wasm');
  if (!response.ok) {
    setStatus('error: engine.wasm not found (run wasm/build.sh first)');
    return;
  }
  const bytes = await response.arrayBuffer();

  // The worker needs its own copy: the worklet's is transferred away.
  wasmBytesForWorker = bytes.slice(0);

  workletNode = new AudioWorkletNode(audioContext, 'daw-processor', {
    numberOfOutputs: 1,
    outputChannelCount: [2],
  });
  workletNode.connect(audioContext.destination);
  workletNode.port.onmessage = (event) => handleWorkletMessage(event.data);
  workletNode.port.postMessage({ type: 'init', wasmBytes: bytes }, [bytes]);
}

function handleWorkletMessage(data) {
  if (data.type === 'ready') {
    setStatus(`running (${data.tracks} tracks)`);
    send({ type: 'setTempo', value: bpm });
    pushAllControls();
    syncArrangement();
  } else if (data.type === 'compiled') {
    if (!data.ok) {
      // The audio thread still held the slot. Retrying on the next tick is
      // enough, since a block boundary passes in well under a millisecond.
      setTimeout(syncArrangement, 16);
      return;
    }
    document.getElementById('note-count').textContent = `${data.notes} notes / ${data.events} events`;
  } else if (data.type === 'meter') {
    updateMeters(data);
  }
}

function stopAudio() {
  if (!audioContext) return;
  audioContext.close();
  audioContext = null;
  workletNode = null;
  heldNotes.clear();
  document.querySelectorAll('.key.held').forEach((el) => el.classList.remove('held'));
  arrangement.setPlayhead(0, false);
  setStatus('stopped');
}

function updateMeters(data) {
  for (let t = 0; t < NUM_TRACKS; ++t) {
    const bar = document.getElementById(`meter-${t}`);
    if (bar) bar.style.width = `${Math.min(data.peaks[t] ?? 0, 1) * 100}%`;
    const voices = document.getElementById(`voices-${t}`);
    if (voices) voices.textContent = String(data.voices[t] ?? 0);
  }

  const fill = document.getElementById('peak-fill');
  fill.style.width = `${Math.min(data.peak, 1) * 100}%`;
  fill.classList.toggle('clipping', data.peak >= 0.999);

  // Samples to ticks, so the playhead lines up with the musical grid.
  const samplesPerTick = (60 / bpm / TICKS_PER_QUARTER) * SAMPLE_RATE;
  arrangement.setPlayhead(data.position / samplesPerTick, data.playing);
}

function syncArrangement() {
  if (!workletNode) return;
  send({ type: 'setTimeline', notes: arrangement.toNotes() });
}

// --- keyboard ---

function noteOn(track, note, velocity = 0.85) {
  const key = `${track}:${note}`;
  if (heldNotes.has(key)) return;
  heldNotes.add(key);
  send({ type: 'noteOn', track, note, velocity });
  document.querySelector(`[data-note="${note}"]`)?.classList.add('held');
}

function noteOff(track, note) {
  if (!heldNotes.delete(`${track}:${note}`)) return;
  send({ type: 'noteOff', track, note });
  document.querySelector(`[data-note="${note}"]`)?.classList.remove('held');
}

function buildKeyboard() {
  const keyboard = document.getElementById('keyboard');
  for (let i = 0; i < NUM_KEYS; ++i) {
    const note = LOWEST_NOTE + i;
    const key = document.createElement('div');
    key.className = `key ${isBlackKey(note) ? 'black' : 'white'}`;
    key.dataset.note = String(note);

    key.addEventListener('pointerdown', (event) => {
      event.preventDefault();
      key.setPointerCapture(event.pointerId);
      startAudio().then(() => noteOn(armedTrack, note));
    });
    key.addEventListener('pointerup', () => noteOff(armedTrack, note));
    key.addEventListener('pointercancel', () => noteOff(armedTrack, note));
    keyboard.appendChild(key);
  }
}

function setupComputerKeyboard() {
  window.addEventListener('keydown', (event) => {
    if (event.key === 'Backspace' || event.key === 'Delete') {
      arrangement.removeSelected();
      pianoRoll.setClip(null);
      return;
    }
    if (event.repeat || event.metaKey || event.ctrlKey) return;
    const offset = KEY_TO_OFFSET[event.key.toLowerCase()];
    if (offset === undefined) return;
    event.preventDefault();
    startAudio().then(() => noteOn(armedTrack, COMPUTER_KEY_BASE + offset));
  });

  window.addEventListener('keyup', (event) => {
    const offset = KEY_TO_OFFSET[event.key.toLowerCase()];
    if (offset === undefined) return;
    noteOff(armedTrack, COMPUTER_KEY_BASE + offset);
  });
}

// --- controls ---

function pushAllControls() {
  document.querySelectorAll('[data-param]').forEach((input) => sendControl(input));
}

function sendControl(input) {
  const value = input.type === 'range' ? parseFloat(input.value) : parseInt(input.value, 10);
  const track = input.dataset.track !== undefined ? parseInt(input.dataset.track, 10) : armedTrack;
  send({ type: input.dataset.param, track, value });

  const readout = document.getElementById(`${input.id}-value`);
  if (readout) readout.textContent = input.value;
}

function setupControls() {
  document.querySelectorAll('[data-param]').forEach((input) => {
    const handler = () => sendControl(input);
    input.addEventListener('input', handler);
    input.addEventListener('change', handler);
  });

  document.querySelectorAll('[data-toggle]').forEach((button) => {
    button.addEventListener('click', () => {
      const on = button.classList.toggle('on');
      send({ type: button.dataset.toggle, track: parseInt(button.dataset.track, 10), value: on ? 1 : 0 });
    });
  });

  const tempo = document.getElementById('bpm');
  tempo.addEventListener('input', () => {
    bpm = parseFloat(tempo.value);
    document.getElementById('bpm-value').textContent = tempo.value;
    // Notes are stored in musical time, so a tempo change is a recompile of
    // the same arrangement rather than the UI resending different positions.
    send({ type: 'setTempo', value: bpm });
    send({ type: 'setLoopTicks', start: 0, end: TOTAL_TICKS });
    syncArrangement();
  });

  document.querySelectorAll('.arm').forEach((button) => {
    button.addEventListener('click', () => {
      armedTrack = parseInt(button.dataset.track, 10);
      document.querySelectorAll('.arm').forEach((b) => b.classList.toggle('on', b === button));
      document.getElementById('armed-name').textContent = TRACK_NAMES[armedTrack];
      pushAllControls();
    });
  });
}

// --- offline bounce ---

function collectTrackSettings() {
  const read = (id, fallback) => {
    const el = document.getElementById(id);
    return el ? parseFloat(el.value) : fallback;
  };
  const settings = [];
  for (let t = 0; t < NUM_TRACKS; ++t) {
    settings.push({
      waveform: parseInt(document.getElementById('waveform').value, 10),
      attack: read('attack', 5),
      decay: read('decay', 120),
      sustain: read('sustain', 0.7),
      release: read('release', 250),
      filterType: parseInt(document.getElementById('filter-type').value, 10),
      cutoff: read('cutoff', 4000),
      resonance: read('resonance', 0.9),
      gain: read(`gain-${t}`, 0.8),
      pan: read(`pan-${t}`, 0),
      muted: document.querySelector(`[data-toggle="setTrackMute"][data-track="${t}"]`)?.classList.contains('on'),
      soloed: document.querySelector(`[data-toggle="setTrackSolo"][data-track="${t}"]`)?.classList.contains('on'),
    });
  }
  return settings;
}

function bounce() {
  if (!wasmBytesForWorker) {
    setStatus('start audio first so the engine is loaded');
    return;
  }

  if (!bounceWorker) {
    bounceWorker = new Worker('bounce-worker.js');
    bounceWorker.onmessage = (event) => {
      const data = event.data;
      if (data.type === 'error') {
        document.getElementById('bounce-info').textContent = `bounce failed: ${data.message}`;
        return;
      }

      // Build the peak pyramid once per bounce, then every repaint reads it
      // instead of rescanning hundreds of thousands of samples.
      const built = performance.now();
      peakCache = new PeakCache(data.left);
      const cacheMs = performance.now() - built;

      drawWaveformPanel();
      document.getElementById('bounce-info').textContent =
        `${data.audioSeconds.toFixed(1)}s rendered in ${data.elapsedMs.toFixed(0)}ms ` +
        `(${data.realtimeFactor.toFixed(0)}x real time), peak cache ${cacheMs.toFixed(0)}ms`;
    };
  }

  const secondsPerTick = 60 / bpm / TICKS_PER_QUARTER;
  const lengthSamples = Math.ceil(TOTAL_TICKS * secondsPerTick * SAMPLE_RATE) + SAMPLE_RATE; // + release tail

  document.getElementById('bounce-info').textContent = 'rendering…';
  bounceWorker.postMessage({
    type: 'bounce',
    wasmBytes: wasmBytesForWorker.slice(0),
    sampleRate: SAMPLE_RATE,
    numTracks: NUM_TRACKS,
    bpm,
    notes: arrangement.toNotes(),
    tracks: collectTrackSettings(),
    master: {
      gain: parseFloat(document.getElementById('master').value),
      saturation: document.getElementById('saturation').value === '1',
    },
    lengthSamples,
  });
}

function drawWaveformPanel() {
  const canvas = document.getElementById('waveform');
  const ratio = window.devicePixelRatio || 1;
  canvas.width = Math.round(canvas.clientWidth * ratio);
  canvas.height = Math.round(120 * ratio);
  canvas.style.height = '120px';

  const context = canvas.getContext('2d');
  context.setTransform(1, 0, 0, 1, 0, 0);
  context.scale(ratio, ratio);

  // drawWaveform works in CSS pixels; the scale above maps them to the
  // backing store.
  const logical = { width: canvas.clientWidth, height: 120, getContext: () => context };
  drawWaveform(logical, peakCache, { background: '#1a1d23', axis: '#3d4350', wave: '#6ea8fe' });
}

// --- Web MIDI ---

async function setupWebMidi() {
  const midiStatus = document.getElementById('midi-status');
  if (!navigator.requestMIDIAccess) {
    midiStatus.textContent = 'not supported in this browser';
    return;
  }
  try {
    const access = await navigator.requestMIDIAccess();
    const attach = () => {
      const names = [];
      for (const input of access.inputs.values()) {
        input.onmidimessage = handleMidiMessage;
        names.push(input.name);
      }
      midiStatus.textContent = names.length > 0 ? names.join(', ') : 'no devices connected';
    };
    attach();
    access.onstatechange = attach;
  } catch (error) {
    midiStatus.textContent = `unavailable (${error.name})`;
  }
}

function handleMidiMessage(event) {
  const [status, data1, data2] = event.data;
  const command = status & 0xf0;
  if (command === 0x90 && data2 > 0) {
    startAudio().then(() => noteOn(armedTrack, data1, data2 / 127));
  } else if (command === 0x80 || (command === 0x90 && data2 === 0)) {
    noteOff(armedTrack, data1);
  } else if (command === 0xb0 && (data1 === 120 || data1 === 123)) {
    panic();
  }
}

function panic() {
  heldNotes.clear();
  send({ type: 'allNotesOff' });
  document.querySelectorAll('.key.held').forEach((el) => el.classList.remove('held'));
}

document.addEventListener('DOMContentLoaded', () => {
  arrangement = new Arrangement(
    document.getElementById('timeline'),
    () => syncArrangement(),
    (clip) => pianoRoll.setClip(clip),
  );
  pianoRoll = new PianoRoll(document.getElementById('pianoroll'), () => syncArrangement());

  // Something to hear on first load rather than an empty grid.
  arrangement.addClip(0, 0);
  arrangement.addClip(3, TICKS_PER_BAR);

  buildKeyboard();
  setupControls();
  setupComputerKeyboard();
  setupWebMidi();
  drawWaveformPanel();

  window.addEventListener('resize', () => {
    arrangement.resize();
    pianoRoll.resize();
    drawWaveformPanel();
  });

  document.getElementById('start').addEventListener('click', startAudio);
  document.getElementById('stop').addEventListener('click', stopAudio);
  document.getElementById('panic').addEventListener('click', panic);
  document.getElementById('bounce').addEventListener('click', bounce);

  document.getElementById('play').addEventListener('click', () => {
    startAudio().then(() => {
      send({ type: 'setLoopTicks', start: 0, end: TOTAL_TICKS });
      send({ type: 'transportSetLoop', value: 1 });
      send({ type: 'transportSetPosition', value: 0 });
      send({ type: 'transportPlay' });
    });
  });

  document.getElementById('pause').addEventListener('click', () => {
    send({ type: 'transportStop' });
    arrangement.setPlayhead(0, false);
  });
});
