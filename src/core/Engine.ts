import resolveSessionRecorderReactNative from '../NativeSessionRecorderReactNative';
import type { ResolvedSessionRecorderConfig } from '../models/config';
import { SessionLogger } from '../utils/Logger';
import {
  ContextImpl,
  NoOpContext,
  type SessionRecorderContext,
} from './Context';
import {
  ControllerImpl,
  NoOpController,
  type SessionRecorderController,
} from './Controller';

export interface SessionRecorderEngineInternal {
  start(): void;
  dispose(): void | Promise<void>;

  readonly isEnabled: boolean;
  readonly config: ResolvedSessionRecorderConfig;
  readonly context: SessionRecorderContext;
  readonly controller: SessionRecorderController;
}

export class NoOpSessionRecorderEngine implements SessionRecorderEngineInternal {
  private static readonly _instance = new NoOpSessionRecorderEngine();
  static instance() {
    return this._instance;
  }
  private constructor() {}

  start(): void {}
  dispose(): void {}

  get isEnabled() {
    return false;
  }

  config = { endpoint: '' } as ResolvedSessionRecorderConfig;
  context = NoOpContext.instance();
  controller = NoOpController.instance();
}

export class SessionRecorderEngine implements SessionRecorderEngineInternal {
  private _isEnabled = false;
  private _isDisposed = false;
  private _disposeCompletion: Promise<void> | null = null;

  readonly config: ResolvedSessionRecorderConfig;
  readonly context: SessionRecorderContext;
  readonly controller: SessionRecorderController;

  constructor(config: ResolvedSessionRecorderConfig) {
    this.config = config;
    this.context = new ContextImpl(this);
    this.controller = new ControllerImpl(this);
  }

  get isEnabled() {
    return this._isEnabled;
  }

  start(): void {
    if (this._isEnabled || this._isDisposed) return;

    const SessionRecorderReactNative = resolveSessionRecorderReactNative();
    if (SessionRecorderReactNative === null) {
      throw new Error(
        'Native module SessionRecorderReactNative is not available'
      );
    }

    if (!SessionRecorderReactNative.initJSI()) {
      throw new Error('Cannot initialize Session Recorder JSI');
    }

    this.context.start();
    this.controller.startReporting();

    SessionLogger.info('SESSION RECORDER INITIALIZED');
    this._isEnabled = true;
  }

  dispose(): void | Promise<void> {
    if (this._isDisposed) return this._disposeCompletion ?? undefined;

    this._isDisposed = true;
    this._isEnabled = false;

    try {
      this.controller.dispose();
    } catch (error) {
      try {
        SessionLogger.error(
          'Error cleaning up Session Recorder controller',
          error
        );
      } catch {}
    }

    // eslint-disable-next-line no-undef-init
    let pending: void | Promise<void> = undefined;
    try {
      pending = this.context.dispose();
    } catch (error) {
      try {
        SessionLogger.error(
          'Error cleaning up Session Recorder context',
          error
        );
      } catch {}
    }

    const finish = (): void => {
      try {
        (globalThis as any).__onSessionRecorderTreeUpdate = undefined;
        (globalThis as any).__onSessionRecorderCaptureResult = undefined;
        const scanner = (globalThis as any).__viewTreeScanner;
        if (typeof scanner?.stopObserver === 'function') {
          scanner.stopObserver();
        } else {
          scanner?.setPaused?.(true);
        }
      } catch (error) {
        try {
          SessionLogger.error('Error cleaning up Session Recorder JSI', error);
        } catch {}
      }
    };
    if (!pending) {
      finish();
      return;
    }
    this._disposeCompletion = Promise.resolve(pending)
      .then(finish, (error) => {
        SessionLogger.error('Error awaiting Context disposal', error);
        finish();
      })
      .catch((error) =>
        SessionLogger.error('Error completing engine disposal', error)
      );
    return this._disposeCompletion;
  }
}
