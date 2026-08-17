// Main thread: the only place that can fetch(). Wasm bytes are fetched here
// and handed to the worklet as a transferable ArrayBuffer.
let audioContext = null;
let workletNode = null;

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
      applyControlsToNode();
    }
  };

  workletNode.port.postMessage({ type: 'init', wasmBytes }, [wasmBytes]);
}

function stopAudio() {
  if (audioContext) {
    audioContext.close();
    audioContext = null;
    workletNode = null;
    setStatus('stopped');
  }
}

function setStatus(text) {
  const el = document.getElementById('status');
  if (el) el.textContent = text;
}

function applyControlsToNode() {
  sendFrequency(parseFloat(document.getElementById('freq').value));
  sendWaveform(document.getElementById('wave').value);
  sendGain(parseFloat(document.getElementById('gain').value));
}

function sendFrequency(hz) {
  if (workletNode) workletNode.port.postMessage({ type: 'setFrequency', value: hz });
}

function sendWaveform(name) {
  const map = { sine: 0, saw: 1, square: 2 };
  if (workletNode) workletNode.port.postMessage({ type: 'setWaveform', value: map[name] ?? 0 });
}

function sendGain(g) {
  if (workletNode) workletNode.port.postMessage({ type: 'setGain', value: g });
}

document.addEventListener('DOMContentLoaded', () => {
  document.getElementById('play').addEventListener('click', startAudio);
  document.getElementById('stop').addEventListener('click', stopAudio);
  document.getElementById('freq').addEventListener('input', (e) => sendFrequency(parseFloat(e.target.value)));
  document.getElementById('wave').addEventListener('change', (e) => sendWaveform(e.target.value));
  document.getElementById('gain').addEventListener('input', (e) => sendGain(parseFloat(e.target.value)));
});
