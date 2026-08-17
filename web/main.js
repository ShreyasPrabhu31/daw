// Main thread: the only place that can fetch(). Wasm bytes are fetched here
// and handed to the worklet as a transferable ArrayBuffer.
const SAMPLE_RATE = 48000;
const NUM_TRACKS = 4;
const STEPS = 16;
const LOWEST_NOTE = 48; // C3
const COMPUTER_KEY_BASE = LOWEST_NOTE + 12;
const HIGHEST_KEY_OFFSET = 16;
const NUM_KEYS = 12 + HIGHEST_KEY_OFFSET + 1;

// Keyboard row mapped to a piano octave, the layout every tracker and DAW uses.
const KEY_TO_OFFSET = {
  a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6,
  g: 7, y: 8, h: 9, u: 10, j: 11, k: 12,
  o: 13, l: 14, p: 15, ';': 16,
};

// One note per track, so a lit step is unambiguous about what it plays.
const TRACK_NOTES = [36, 43, 55, 67];
const TRACK_NAMES = ['Bass', 'Tenor', 'Alto', 'Lead'];

let audioContext = null;
let workletNode = null;
let armedTrack = 0;
let bpm = 110;

const heldNotes = new Set();
const pattern = Array.from({ length: NUM_TRACKS }, () => new Array(STEPS).fill(false));

function isBlackKey(note) {
  return [1, 3, 6, 8, 10].includes(note % 12);
}

function framesPerStep() {
  return Math.round((60 / bpm / 4) * SAMPLE_RATE); // sixteenth notes
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
  const wasmBytes = await response.arrayBuffer();

  workletNode = new AudioWorkletNode(audioContext, 'daw-processor', {
    numberOfOutputs: 1,
    outputChannelCount: [2],
  });
  workletNode.connect(audioContext.destination);

  workletNode.port.onmessage = (event) => {
    const data = event.data;
    if (data.type === 'ready') {
      setStatus(`running (${data.tracks} tracks)`);
      pushAllControls();
      pushPattern();
    } else if (data.type === 'meter') {
      updateMeter(data);
    }
  };

  workletNode.port.postMessage({ type: 'init', wasmBytes }, [wasmBytes]);
}

function stopAudio() {
  if (!audioContext) return;
  audioContext.close();
  audioContext = null;
  workletNode = null;
  heldNotes.clear();
  document.querySelectorAll('.key.held').forEach((el) => el.classList.remove('held'));
  document.querySelectorAll('.step.playing').forEach((el) => el.classList.remove('playing'));
  setStatus('stopped');
}

function updateMeter(data) {
  for (let t = 0; t < NUM_TRACKS; ++t) {
    const el = document.getElementById(`voices-${t}`);
    if (el) el.textContent = String(data.voices[t] ?? 0);
  }

  const fill = document.getElementById('peak-fill');
  fill.style.width = `${Math.min(data.peak, 1) * 100}%`;
  fill.classList.toggle('clipping', data.peak >= 0.999);

  // Highlight the step the playhead is inside, which is the visible proof
  // that the engine's transport and the pattern agree.
  const stepIndex = data.playing
    ? Math.floor(data.position / framesPerStep()) % STEPS
    : -1;
  document.querySelectorAll('.step').forEach((el) => {
    el.classList.toggle('playing', Number(el.dataset.step) === stepIndex);
  });
}

function noteOn(track, note, velocity = 0.85) {
  const key = `${track}:${note}`;
  if (heldNotes.has(key)) return; // ignore auto-repeat
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

function buildSequencer() {
  const grid = document.getElementById('sequencer');

  for (let track = 0; track < NUM_TRACKS; ++track) {
    const label = document.createElement('div');
    label.className = 'seq-label';
    label.textContent = TRACK_NAMES[track];
    grid.appendChild(label);

    for (let step = 0; step < STEPS; ++step) {
      const cell = document.createElement('button');
      cell.className = 'step';
      cell.dataset.step = String(step);
      cell.dataset.track = String(track);
      if (step % 4 === 0) cell.classList.add('downbeat');

      cell.addEventListener('click', () => {
        pattern[track][step] = !pattern[track][step];
        cell.classList.toggle('on', pattern[track][step]);
        pushPattern();
      });

      grid.appendChild(cell);
    }
  }
}

// Rewrites the whole pattern as scheduled events. Each note is placed at an
// exact sample, and the loop range is the pattern length, so the engine
// re-arms the events every pass instead of the UI re-sending them.
function pushPattern() {
  if (!workletNode) return;

  const step = framesPerStep();
  const length = step * STEPS;

  send({ type: 'clearScheduled' });
  send({ type: 'setLoopRange', start: 0, end: length });

  for (let track = 0; track < NUM_TRACKS; ++track) {
    for (let i = 0; i < STEPS; ++i) {
      if (!pattern[track][i]) continue;
      const note = TRACK_NOTES[track];
      send({ type: 'scheduleNoteOn', track, note, velocity: 0.85, time: i * step });
      send({ type: 'scheduleNoteOff', track, note, time: i * step + Math.floor(step * 0.8) });
    }
  }
}

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
      send({
        type: button.dataset.toggle,
        track: parseInt(button.dataset.track, 10),
        value: on ? 1 : 0,
      });
    });
  });

  const tempo = document.getElementById('bpm');
  tempo.addEventListener('input', () => {
    bpm = parseFloat(tempo.value);
    document.getElementById('bpm-value').textContent = tempo.value;
    pushPattern(); // event times are absolute, so a tempo change rewrites them
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

function setupComputerKeyboard() {
  window.addEventListener('keydown', (event) => {
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

// Web MIDI is optional: it needs a secure context and user permission, and
// plenty of browsers do not implement it at all. Nothing above depends on it.
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
    access.onstatechange = attach; // devices can be plugged in after load
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
    // A note-on with zero velocity is a note-off; many controllers send that
    // instead of a real 0x80 message.
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
  buildKeyboard();
  buildSequencer();
  setupControls();
  setupComputerKeyboard();
  setupWebMidi();

  document.getElementById('start').addEventListener('click', startAudio);
  document.getElementById('stop').addEventListener('click', stopAudio);
  document.getElementById('panic').addEventListener('click', panic);

  document.getElementById('play').addEventListener('click', () => {
    startAudio().then(() => {
      send({ type: 'setLoopRange', start: 0, end: framesPerStep() * STEPS });
      send({ type: 'transportSetLoop', value: 1 });
      send({ type: 'transportSetPosition', value: 0 });
      send({ type: 'transportPlay' });
    });
  });

  document.getElementById('pause').addEventListener('click', () => {
    send({ type: 'transportStop' });
    document.querySelectorAll('.step.playing').forEach((el) => el.classList.remove('playing'));
  });
});
