export type InactivityState = 'active' | 'inactive';

export interface InactivityDetectorOptions {
  /** * Callback invoked when inactivity is detected (when the timer completes). */
  onInactive: () => void;

  /** * Callback invoked when activity is detected after the system was
   * previously considered inactive. */
  onActive: () => void;
}

export class InactivityDetector {
  private readonly onInactive: () => void;
  private readonly onActive: () => void;

  private _timer: ReturnType<typeof setTimeout> | null = null;
  private _generation = 0;

  private _state: InactivityState = 'active';
  get state(): InactivityState {
    return this._state;
  }

  constructor(options: InactivityDetectorOptions) {
    this.onInactive = options.onInactive;
    this.onActive = options.onActive;
  }

  ping(): void {
    if (this._state === 'inactive') {
      this._state = 'active';
      this.onActive();
    }

    this._reset();
  }

  start(): void {
    this._state = 'active';
    return this._reset();
  }
  private _reset(): void {
    if (this._timer !== null) clearTimeout(this._timer);
    const generation = ++this._generation;

    this._timer = setTimeout(() => {
      if (generation !== this._generation || this._state !== 'active') return;
      this._timer = null;
      this._state = 'inactive';

      this.onInactive();
    }, 30000);
  }

  stop(): void {
    ++this._generation;
    if (this._timer !== null) clearTimeout(this._timer);
    this._timer = null;
  }
}
