// Min/max peak cache for waveform drawing.
//
// Drawing a waveform by scanning raw samples is O(samples) per repaint, which
// at 48 kHz means hundreds of thousands of reads for a few seconds of audio,
// every frame, while the user is dragging. Instead the signal is reduced once
// into buckets holding the minimum and maximum of the samples they cover, and
// the reductions are stacked into a pyramid: each level is built from the one
// below by combining pairs, so building every level costs about 2n total.
//
// A repaint then picks the level whose bucket is closest to one pixel of the
// current zoom and reads roughly one bucket per pixel. Keeping both the min
// and the max is what preserves the shape: averaging magnitudes would flatten
// a loud transient into the same grey band as steady tone.
class PeakCache {
  constructor(samples, baseBucket = 128) {
    this.length = samples.length;
    this.baseBucket = baseBucket;
    this.levels = [];

    if (this.length === 0) return;

    // Level 0 straight from the samples.
    const count = Math.ceil(this.length / baseBucket);
    const mins = new Float32Array(count);
    const maxs = new Float32Array(count);

    for (let bucket = 0; bucket < count; ++bucket) {
      const start = bucket * baseBucket;
      const end = Math.min(start + baseBucket, this.length);
      let min = samples[start];
      let max = samples[start];
      for (let i = start + 1; i < end; ++i) {
        const value = samples[i];
        if (value < min) min = value;
        if (value > max) max = value;
      }
      mins[bucket] = min;
      maxs[bucket] = max;
    }
    this.levels.push({ mins, maxs, bucket: baseBucket });

    // Each further level halves the resolution by combining adjacent pairs.
    while (this.levels[this.levels.length - 1].mins.length > 2) {
      const below = this.levels[this.levels.length - 1];
      const half = Math.ceil(below.mins.length / 2);
      const upMins = new Float32Array(half);
      const upMaxs = new Float32Array(half);

      for (let i = 0; i < half; ++i) {
        const a = i * 2;
        const b = Math.min(a + 1, below.mins.length - 1);
        upMins[i] = Math.min(below.mins[a], below.mins[b]);
        upMaxs[i] = Math.max(below.maxs[a], below.maxs[b]);
      }
      this.levels.push({ mins: upMins, maxs: upMaxs, bucket: below.bucket * 2 });
    }
  }

  // The coarsest level that still gives at least one bucket per pixel, so we
  // read as little as possible without dropping detail the screen could show.
  levelFor(samplesPerPixel) {
    let chosen = 0;
    for (let i = 0; i < this.levels.length; ++i) {
      if (this.levels[i].bucket <= samplesPerPixel) chosen = i;
    }
    return this.levels[chosen];
  }

  // Returns [min, max] over a sample range, read from the cache.
  range(level, fromSample, toSample) {
    const first = Math.max(0, Math.floor(fromSample / level.bucket));
    const last = Math.min(level.mins.length - 1, Math.ceil(toSample / level.bucket) - 1);
    if (last < first) return [0, 0];

    let min = level.mins[first];
    let max = level.maxs[first];
    for (let i = first + 1; i <= last; ++i) {
      if (level.mins[i] < min) min = level.mins[i];
      if (level.maxs[i] > max) max = level.maxs[i];
    }
    return [min, max];
  }
}

function drawWaveform(canvas, cache, colors) {
  const context = canvas.getContext('2d');
  const width = canvas.width;
  const height = canvas.height;

  context.clearRect(0, 0, width, height);
  context.fillStyle = colors.background;
  context.fillRect(0, 0, width, height);

  const middle = height / 2;
  context.strokeStyle = colors.axis;
  context.beginPath();
  context.moveTo(0, middle);
  context.lineTo(width, middle);
  context.stroke();

  if (!cache || cache.length === 0) {
    context.fillStyle = colors.axis;
    context.font = '12px system-ui, sans-serif';
    context.fillText('no bounce yet', 10, middle - 8);
    return;
  }

  const samplesPerPixel = cache.length / width;
  const level = cache.levelFor(samplesPerPixel);

  context.fillStyle = colors.wave;
  for (let x = 0; x < width; ++x) {
    const from = x * samplesPerPixel;
    const to = from + samplesPerPixel;
    const [min, max] = cache.range(level, from, to);

    const top = middle - max * middle;
    const bottom = middle - min * middle;
    // Always at least a hair tall, so silence still reads as a line rather
    // than a gap in the drawing.
    context.fillRect(x, top, 1, Math.max(1, bottom - top));
  }
}
