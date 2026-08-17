// Runs on the audio thread. Has no fetch and no DOM, so the wasm bytes
// arrive via postMessage from the main thread instead of being fetched here.
class DawProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false;
    this.exports = null;
    this.leftView = null;
    this.rightView = null;
    this.maxBlockFrames = 1024;

    this.port.onmessage = (event) => {
      const { type } = event.data;
      if (type === 'init') {
        this._init(event.data.wasmBytes);
      } else if (type === 'setFrequency') {
        if (this.exports) this.exports.daw_set_frequency(event.data.value);
      } else if (type === 'setWaveform') {
        if (this.exports) this.exports.daw_set_waveform(event.data.value);
      } else if (type === 'setGain') {
        if (this.exports) this.exports.daw_set_gain(event.data.value);
      }
    };
  }

  async _init(wasmBytes) {
    const { instance } = await WebAssembly.instantiate(wasmBytes, {});
    this.exports = instance.exports;

    // Standalone wasm: static constructors run via the explicit _initialize
    // export, which must happen before any other call touches global state.
    if (typeof this.exports._initialize === 'function') {
      this.exports._initialize();
    }

    this.exports.daw_init(sampleRate);

    // ALLOW_MEMORY_GROWTH=0 means this buffer never detaches, so these views
    // are built once here and reused for the processor's lifetime.
    const leftPtr = this.exports.daw_get_channel_ptr(0);
    const rightPtr = this.exports.daw_get_channel_ptr(1);
    const memory = this.exports.memory.buffer;
    this.leftView = new Float32Array(memory, leftPtr, this.maxBlockFrames);
    this.rightView = new Float32Array(memory, rightPtr, this.maxBlockFrames);

    this.ready = true;
    this.port.postMessage({ type: 'ready' });
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

    return true;
  }
}

registerProcessor('daw-processor', DawProcessor);
