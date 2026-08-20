// Note editor for the selected clip: pitch up the side, time across the top.
// Positions are relative to the clip, so moving the clip moves its notes with
// it and the editor never has to care where on the timeline it sits.
const ROLL_ROWS = 24; // two octaves
const ROLL_ROW_HEIGHT = 12;

class PianoRoll {
  constructor(canvas, onChange) {
    this.canvas = canvas;
    this.onChange = onChange;
    this.clip = null;
    this.lowestPitch = 48;

    this.canvas.addEventListener('pointerdown', (event) => this._toggleAt(event));
    this.resize();
  }

  setClip(clip) {
    this.clip = clip;
    if (clip) this.lowestPitch = TRACK_ROOT[clip.track] - 12;
    this.resize();
  }

  resize() {
    const ratio = window.devicePixelRatio || 1;
    const width = this.canvas.clientWidth;
    const height = ROLL_ROWS * ROLL_ROW_HEIGHT;
    this.canvas.style.height = `${height}px`;
    this.canvas.width = Math.round(width * ratio);
    this.canvas.height = Math.round(height * ratio);
    this.ratio = ratio;
    this.draw();
  }

  get columns() {
    return this.clip ? Math.max(1, Math.round(this.clip.lengthTicks / SNAP_TICKS)) : 16;
  }

  _toggleAt(event) {
    if (!this.clip) return;
    const rect = this.canvas.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const y = event.clientY - rect.top;

    const column = Math.floor((x / this.canvas.clientWidth) * this.columns);
    const row = Math.floor(y / ROLL_ROW_HEIGHT);
    if (column < 0 || column >= this.columns || row < 0 || row >= ROLL_ROWS) return;

    // Row 0 is the top of the display, which is the highest pitch.
    const pitch = this.lowestPitch + (ROLL_ROWS - 1 - row);
    const offsetTick = column * SNAP_TICKS;

    const existing = this.clip.notes.findIndex(
      (note) => note.pitch === pitch && note.offsetTick === offsetTick,
    );
    if (existing >= 0) {
      this.clip.notes.splice(existing, 1);
    } else {
      this.clip.notes.push({ pitch, offsetTick, lengthTicks: SNAP_TICKS });
    }

    this.draw();
    if (this.onChange) this.onChange();
  }

  draw() {
    const context = this.canvas.getContext('2d');
    const width = this.canvas.clientWidth;
    const height = ROLL_ROWS * ROLL_ROW_HEIGHT;

    context.setTransform(this.ratio, 0, 0, this.ratio, 0, 0);
    context.clearRect(0, 0, width, height);
    context.fillStyle = '#1a1d23';
    context.fillRect(0, 0, width, height);

    if (!this.clip) {
      context.fillStyle = '#6b7280';
      context.font = '12px system-ui, sans-serif';
      context.fillText('select a clip to edit its notes', 10, height / 2);
      return;
    }

    const columnWidth = width / this.columns;

    for (let row = 0; row < ROLL_ROWS; ++row) {
      const pitch = this.lowestPitch + (ROLL_ROWS - 1 - row);
      // Shade the black keys so the octave is readable at a glance.
      const isBlack = [1, 3, 6, 8, 10].includes(((pitch % 12) + 12) % 12);
      context.fillStyle = isBlack ? '#16181d' : '#1e2128';
      context.fillRect(0, row * ROLL_ROW_HEIGHT, width, ROLL_ROW_HEIGHT);
    }

    context.strokeStyle = '#282c34';
    for (let column = 0; column <= this.columns; ++column) {
      const x = column * columnWidth;
      context.strokeStyle = column % 4 === 0 ? '#3d4350' : '#282c34';
      context.beginPath();
      context.moveTo(x, 0);
      context.lineTo(x, height);
      context.stroke();
    }

    context.fillStyle = TRACK_COLORS[this.clip.track];
    for (const note of this.clip.notes) {
      const row = ROLL_ROWS - 1 - (note.pitch - this.lowestPitch);
      if (row < 0 || row >= ROLL_ROWS) continue;
      const x = (note.offsetTick / SNAP_TICKS) * columnWidth;
      const w = Math.max(3, (note.lengthTicks / SNAP_TICKS) * columnWidth - 2);
      context.fillRect(x + 1, row * ROLL_ROW_HEIGHT + 1, w, ROLL_ROW_HEIGHT - 2);
    }
  }
}
