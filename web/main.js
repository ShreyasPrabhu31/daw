// Main thread: the only place that can fetch(). Wasm bytes are fetched here
// and handed to the worklet as a transferable ArrayBuffer.
let audioContext = null;
let workletNode = null;

const LOWEST_NOTE = 48; // C3
const COMPUTER_KEY_BASE = LOWEST_NOTE + 12; // the computer keys play the middle octave

// Must reach the top note the computer keyboard can produce, or those keys
// sound with no corresponding key lighting up.
const HIGHEST_KEY_OFFSET = 16;
const NUM_KEYS = 12 + HIGHEST_KEY_OFFSET + 1;

// Keyboard row mapped to a piano octave, the layout every tracker and DAW uses.
const KEY_TO_OFFSET = {
  a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6,
  g: 7, y: 8, h: 9, u: 10, j: 11, k: 12,
  o: 13, l: 14, p: 15, ';': 16,
};

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
  audioContext = new AudioContext({ sampleRate: 48000 });
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
    if (event.data.type === 'ready') {
      setStatus('running');
      pushAllControls();
    } else if (event.data.type === 'meter') {
      updateMeter(event.data.voices, event.data.peak);
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
  updateMeter(0, 0);
  setStatus('stopped');
}

function updateMeter(voices, peak) {
  document.getElementById('voices').textContent = `${voices} / 8`;
  const clipping = peak >= 0.999;
  const meter = document.getElementById('peak-fill');
  meter.style.width = `${Math.min(peak, 1) * 100}%`;
  meter.classList.toggle('clipping', clipping);
}

function noteOn(note, velocity = 0.85) {
  if (heldNotes.has(note)) return; // ignore auto-repeat
  heldNotes.add(note);
  send({ type: 'noteOn', note, velocity });
  document.querySelector(`[data-note="${note}"]`)?.classList.add('held');
}

function noteOff(note) {
  if (!heldNotes.delete(note)) return;
  send({ type: 'noteOff', note });
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
      startAudio().then(() => noteOn(note));
    });
    key.addEventListener('pointerup', () => noteOff(note));
    key.addEventListener('pointercancel', () => noteOff(note));

    keyboard.appendChild(key);
  }
}

// Sends every control's current value, used once the worklet reports ready so
// the engine starts out matching what the UI is showing.
function pushAllControls() {
  document.querySelectorAll('[data-param]').forEach((input) => {
    sendControl(input);
  });
}

function sendControl(input) {
  const value = input.type === 'range' ? parseFloat(input.value) : parseInt(input.value, 10);
  send({ type: input.dataset.param, value });

  const readout = document.getElementById(`${input.id}-value`);
  if (readout) readout.textContent = input.type === 'range' ? input.value : '';
}

function setupControls() {
  document.querySelectorAll('[data-param]').forEach((input) => {
    const handler = () => sendControl(input);
    input.addEventListener('input', handler);
    input.addEventListener('change', handler);
  });
}

function setupComputerKeyboard() {
  window.addEventListener('keydown', (event) => {
    if (event.repeat || event.metaKey || event.ctrlKey) return;
    const offset = KEY_TO_OFFSET[event.key.toLowerCase()];
    if (offset === undefined) return;
    event.preventDefault();
    startAudio().then(() => noteOn(COMPUTER_KEY_BASE + offset));
  });

  window.addEventListener('keyup', (event) => {
    const offset = KEY_TO_OFFSET[event.key.toLowerCase()];
    if (offset === undefined) return;
    noteOff(COMPUTER_KEY_BASE + offset);
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
    startAudio().then(() => noteOn(data1, data2 / 127));
  } else if (command === 0x80 || (command === 0x90 && data2 === 0)) {
    // A note-on with zero velocity is a note-off; many controllers send that
    // instead of a real 0x80 message.
    noteOff(data1);
  } else if (command === 0xb0 && (data1 === 120 || data1 === 123)) {
    heldNotes.clear();
    send({ type: 'allNotesOff' });
    document.querySelectorAll('.key.held').forEach((el) => el.classList.remove('held'));
  }
}

document.addEventListener('DOMContentLoaded', () => {
  buildKeyboard();
  setupControls();
  setupComputerKeyboard();
  setupWebMidi();

  document.getElementById('start').addEventListener('click', startAudio);
  document.getElementById('stop').addEventListener('click', stopAudio);
  document.getElementById('panic').addEventListener('click', () => {
    heldNotes.clear();
    send({ type: 'allNotesOff' });
    document.querySelectorAll('.key.held').forEach((el) => el.classList.remove('held'));
  });
});
