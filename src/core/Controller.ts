import { SessionLogger } from '../utils/Logger';
import { SessionRecorderEngine } from './Engine';
import { InactivityDetector } from './Inactivity';
import { SessionRecorderReporter } from './Reporter';

export interface SessionRecorderController {
  startReporting(): void;
  stopReporting(suspendActivity?: boolean): void;
  pingActivity(): void;
  dispose(): void;
}
export class NoOpController implements SessionRecorderController {
  private static readonly _instance = new NoOpController();
  static instance(): NoOpController {
    return this._instance;
  }
  private constructor() {}

  startReporting(): void {}
  stopReporting(): void {}
  pingActivity(): void {}
  dispose(): void {}
}
export class ControllerImpl implements SessionRecorderController {
  private readonly _engine: SessionRecorderEngine;
  private readonly _reporter: SessionRecorderReporter;
  private readonly _inactivity: InactivityDetector;
  private _activitySuspended = false;
  private _isDisposed = false;

  constructor(engine: SessionRecorderEngine) {
    this._engine = engine;
    this._reporter = new SessionRecorderReporter(this._engine);
    this._inactivity = new InactivityDetector({
      onActive: () => {
        this.startReporting();
        this._runLifecycleStep('resume LOM capture', () =>
          this._engine.context.resumeLomCapture()
        );
      },
      onInactive: () => {
        this.stopReporting();
        this._runLifecycleStep('suspend LOM capture', () =>
          this._engine.context.suspendLomCapture()
        );
      },
    });
  }

  startReporting(): void {
    if (this._isDisposed) return;
    this._activitySuspended = false;
    this._runLifecycleStep('start inactivity', () => this._inactivity.start());
    this._runLifecycleStep('start reporter', () => this._reporter.start());
  }
  stopReporting(suspendActivity: boolean = false): void {
    // Ordinary inactivity must still allow a later input to wake the recorder.
    if (suspendActivity) this._activitySuspended = true;
    this._runLifecycleStep('stop inactivity', () => this._inactivity.stop());
    this._runLifecycleStep('stop reporter', () => this._reporter.stop());
  }

  private _runLifecycleStep(operation: string, action: () => void): void {
    try {
      action();
    } catch (error) {
      SessionLogger.error(`Error during lifecycle ${operation}`, error);
    }
  }

  dispose(): void {
    if (this._isDisposed) return;
    this._isDisposed = true;
    this._activitySuspended = true;
    try {
      this._inactivity.stop();
    } finally {
      this._reporter.close();
    }
  }

  pingActivity(): void {
    if (this._activitySuspended || this._isDisposed) return;
    this._inactivity.ping();
  }
}
