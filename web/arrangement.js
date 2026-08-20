// The arrangement: clips on a timeline, drawn to a canvas and edited with the
// pointer. The engine owns the authoritative note list; this is the editable
// view that gets compiled into it.
const TICKS_PER_QUARTER = 960;
const BEATS_PER_BAR = 4;
const TICKS_PER_BAR = TICKS_PER_QUARTER * BEATS_PER_BAR;
const NUM_BARS = 4;
const TOTAL_TICKS = TICKS_PER_BAR * NUM_BARS;
const SNAP_TICKS = TICKS_PER_QUARTER / 4; // sixteenths

const TRACK_NAMES = ['Bass', 'Tenor', 'Alto', 'Lead'];
const TRACK_ROOT = [36, 48, 55, 67];
const TRACK_COLORS = ['#6ea8fe', '#5ac8a8', '#e0a458', '#c98bdb'];
const NUM_TRACKS = 4;

const LANE_HEIGHT = 56;
const RESIZE_GRIP_PX = 8;

class Arrangement {
  constructor(canvas, onChange, onSelect) {
    this.canvas = canvas;
    this.onChange = onChange;
    // Selection is separate from mutation: picking a clip should refresh the
    // note editor without recompiling an arrangement that has not changed.
    this.onSelect = onSelect;
    this.clips = [];
    this.nextId = 1;
    this.selectedId = null;
    this.playheadTick = 0;
    this.playing = false;

    this.drag = null;
    this._installPointerHandlers();
    this.resize();
  }

  resize() {
    // Backing store in device pixels so the lines stay crisp on a retina
    // display, with the CSS box left in layout pixels.
    const ratio = window.devicePixelRatio || 1;
    const width = this.canvas.clientWidth;
    const height = NUM_TRACKS * LANE_HEIGHT;
    this.canvas.style.height = `${height}px`;
    this.canvas.width = Math.round(width * ratio);
    this.canvas.height = Math.round(height * ratio);
    this.ratio = ratio;
    this.draw();
  }

  get pixelsPerTick() {
    return this.canvas.clientWidth / TOTAL_TICKS;
  }

  tickToX(tick) {
    return tick * this.pixelsPerTick;
  }

  xToTick(x) {
    return Math.max(0, Math.min(TOTAL_TICKS, x / this.pixelsPerTick));
  }

  snap(tick) {
    return Math.round(tick / SNAP_TICKS) * SNAP_TICKS;
  }

  // --- model ---

  addClip(track, startTick, lengthTicks = TICKS_PER_BAR) {
    const clip = {
      id: this.nextId++,
      track,
      startTick: Math.max(0, Math.min(TOTAL_TICKS - lengthTicks, this.snap(startTick))),
      lengthTicks,
      notes: [],
    };
    // A brand new empty clip would be silent and look broken, so it starts
    // with a plain root-note pulse the user can then edit.
    const root = TRACK_ROOT[track];
    for (let beat = 0; beat < lengthTicks / TICKS_PER_QUARTER; ++beat) {
      clip.notes.push({ pitch: root, offsetTick: beat * TICKS_PER_QUARTER, lengthTicks: TICKS_PER_QUARTER / 2 });
    }
    this.clips.push(clip);
    this.select(clip.id);
    this.commit();
    return clip;
  }

  removeSelected() {
    if (this.selectedId === null) return;
    this.clips = this.clips.filter((clip) => clip.id !== this.selectedId);
    this.select(null);
    this.commit();
  }

  select(id) {
    if (this.selectedId === id) return;
    this.selectedId = id;
    if (this.onSelect) this.onSelect(this.selectedClip());
  }

  selectedClip() {
    return this.clips.find((clip) => clip.id === this.selectedId) || null;
  }

  clipAt(x, y) {
    const track = Math.floor(y / LANE_HEIGHT);
    const tick = this.xToTick(x);
    // Reverse order so the topmost drawn clip wins the hit test.
    for (let i = this.clips.length - 1; i >= 0; --i) {
      const clip = this.clips[i];
      if (clip.track !== track) continue;
      if (tick >= clip.startTick && tick <= clip.startTick + clip.lengthTicks) return clip;
    }
    return null;
  }

  // Flattens clips into absolute-time notes for the engine.
  toNotes() {
    const notes = [];
    for (const clip of this.clips) {
      for (const note of clip.notes) {
        notes.push({
          track: clip.track,
          pitch: note.pitch,
          startTick: clip.startTick + note.offsetTick,
          lengthTicks: note.lengthTicks,
          velocity: 0.85,
        });
      }
    }
    return notes;
  }

  commit() {
    this.draw();
    if (this.onChange) this.onChange();
  }

  // --- interaction ---

  _installPointerHandlers() {
    const localPoint = (event) => {
      const rect = this.canvas.getBoundingClientRect();
      return { x: event.clientX - rect.left, y: event.clientY - rect.top };
    };

    this.canvas.addEventListener('pointerdown', (event) => {
      const { x, y } = localPoint(event);
      const clip = this.clipAt(x, y);

      if (!clip) {
        const track = Math.max(0, Math.min(NUM_TRACKS - 1, Math.floor(y / LANE_HEIGHT)));
        this.addClip(track, this.xToTick(x));
        return;
      }

      this.select(clip.id);
      this.canvas.setPointerCapture(event.pointerId);

      const rightEdge = this.tickToX(clip.startTick + clip.lengthTicks);
      const mode = Math.abs(x - rightEdge) <= RESIZE_GRIP_PX ? 'resize' : 'move';
      this.drag = { clip, mode, grabTick: this.xToTick(x) - clip.startTick };
      this.draw();
    });

    this.canvas.addEventListener('pointermove', (event) => {
      const { x, y } = localPoint(event);

      if (!this.drag) {
        // Only the cursor changes on hover, so this stays cheap.
        const clip = this.clipAt(x, y);
        const overEdge =
          clip && Math.abs(x - this.tickToX(clip.startTick + clip.lengthTicks)) <= RESIZE_GRIP_PX;
        this.canvas.style.cursor = overEdge ? 'ew-resize' : clip ? 'grab' : 'crosshair';
        return;
      }

      const { clip, mode } = this.drag;
      const tick = this.xToTick(x);

      if (mode === 'resize') {
        const raw = this.snap(tick - clip.startTick);
        clip.lengthTicks = Math.max(SNAP_TICKS, Math.min(TOTAL_TICKS - clip.startTick, raw));
        // Notes that fall outside the shortened clip would never sound, so
        // they are dropped rather than left as invisible state.
        clip.notes = clip.notes.filter((note) => note.offsetTick < clip.lengthTicks);
      } else {
        const start = this.snap(tick - this.drag.grabTick);
        clip.startTick = Math.max(0, Math.min(TOTAL_TICKS - clip.lengthTicks, start));
        clip.track = Math.max(0, Math.min(NUM_TRACKS - 1, Math.floor(y / LANE_HEIGHT)));
      }
      this.draw();
    });

    const finish = (event) => {
      if (!this.drag) return;
      this.drag = null;
      if (this.canvas.hasPointerCapture(event.pointerId)) {
        this.canvas.releasePointerCapture(event.pointerId);
      }
      this.commit();
    };
    this.canvas.addEventListener('pointerup', finish);
    this.canvas.addEventListener('pointercancel', finish);
  }

  // --- drawing ---

  setPlayhead(tick, playing) {
    this.playheadTick = tick;
    this.playing = playing;
    this.draw();
  }

  draw() {
    const context = this.canvas.getContext('2d');
    const width = this.canvas.clientWidth;
    const height = NUM_TRACKS * LANE_HEIGHT;

    context.setTransform(this.ratio, 0, 0, this.ratio, 0, 0);
    context.clearRect(0, 0, width, height);

    for (let track = 0; track < NUM_TRACKS; ++track) {
      const top = track * LANE_HEIGHT;
      context.fillStyle = track % 2 === 0 ? '#1e2128' : '#1a1d23';
      context.fillRect(0, top, width, LANE_HEIGHT);

      context.fillStyle = '#6b7280';
      context.font = '11px system-ui, sans-serif';
      context.fillText(TRACK_NAMES[track], 6, top + 14);
    }

    // Beat and bar lines. Bars get the brighter line so the grid reads.
    for (let tick = 0; tick <= TOTAL_TICKS; tick += TICKS_PER_QUARTER) {
      const x = this.tickToX(tick);
      context.strokeStyle = tick % TICKS_PER_BAR === 0 ? '#3d4350' : '#282c34';
      context.beginPath();
      context.moveTo(x, 0);
      context.lineTo(x, height);
      context.stroke();
    }

    for (const clip of this.clips) {
      this._drawClip(context, clip);
    }

    if (this.playing) {
      const x = this.tickToX(this.playheadTick);
      context.strokeStyle = '#e6e8ec';
      context.lineWidth = 2;
      context.beginPath();
      context.moveTo(x, 0);
      context.lineTo(x, height);
      context.stroke();
      context.lineWidth = 1;
    }
  }

  _drawClip(context, clip) {
    const x = this.tickToX(clip.startTick);
    const w = Math.max(4, this.tickToX(clip.lengthTicks));
    const y = clip.track * LANE_HEIGHT + 4;
    const h = LANE_HEIGHT - 8;
    const selected = clip.id === this.selectedId;
    const color = TRACK_COLORS[clip.track];

    context.fillStyle = selected ? `${color}55` : `${color}33`;
    context.strokeStyle = selected ? '#e6e8ec' : color;
    context.beginPath();
    context.roundRect(x, y, w, h, 4);
    context.fill();
    context.stroke();

    // Notes inside the clip, positioned by pitch within a two-octave window.
    const lowest = TRACK_ROOT[clip.track] - 12;
    const span = 24;
    context.fillStyle = color;
    for (const note of clip.notes) {
      const nx = this.tickToX(clip.startTick + note.offsetTick);
      const nw = Math.max(2, this.tickToX(note.lengthTicks) - 1);
      const rel = Math.max(0, Math.min(span - 1, note.pitch - lowest));
      const ny = y + h - 4 - ((rel + 1) / span) * (h - 8);
      context.fillRect(nx, ny, nw, 3);
    }
  }
}
