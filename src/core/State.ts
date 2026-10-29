import {
  AppState,
  type AppStateStatus,
  type EventSubscription,
} from 'react-native';
import { SessionLogger } from '../utils/Logger';
import type { SessionRecorderEngine } from './Engine';

export class SessionRecorderState {
  private readonly _engine: SessionRecorderEngine;

  private _appStateSubscription: EventSubscription | null = null;
  private _subscriptionGeneration = 0;
  private _pauseCompletion: Promise<void> | null = null;
  private _resumeRequested = false;

  constructor(engine: SessionRecorderEngine) {
    this._engine = engine;
  }

  public listen(): void {
    this.stop();
    const generation = this._subscriptionGeneration;
    this._appStateSubscription = AppState.addEventListener(
      'change',
      (appState: AppStateStatus) => {
        if (generation !== this._subscriptionGeneration) return;
        switch (appState) {
          case 'background':
          case 'inactive': {
            SessionLogger.verbose('App in background. Paused...');

            this._resumeRequested = false;
            this._runLifecycleStep('stop reporting', () =>
              this._engine.controller.stopReporting(true)
            );
            if (this._pauseCompletion) break;
            const pending = this._runLifecycleStep('pause context', () =>
              this._engine.context.pause()
            );
            if (pending) {
              const completion = Promise.resolve(pending)
                .catch((error) =>
                  SessionLogger.error('Error awaiting lifecycle pause', error)
                )
                .then(() => {
                  if (this._pauseCompletion !== completion) return;
                  this._pauseCompletion = null;
                  if (
                    generation === this._subscriptionGeneration &&
                    this._resumeRequested
                  ) {
                    this._resumeRuntime();
                  }
                });
              this._pauseCompletion = completion;
            }
            break;
          }

          case 'active':
            SessionLogger.verbose('App in foreground. Resuming...');

            this._resumeRequested = true;
            if (!this._pauseCompletion) this._resumeRuntime();
            break;
          default:
            break;
        }
      }
    );
  }

  public stop(): void {
    this._resumeRequested = false;
    ++this._subscriptionGeneration;
    const subscription = this._appStateSubscription;
    this._appStateSubscription = null;
    this._runLifecycleStep('remove lifecycle listener', () =>
      subscription?.remove()
    );
  }

  private _resumeRuntime(): void {
    this._resumeRequested = false;
    this._runLifecycleStep('resume context', () =>
      this._engine.context.resume()
    );
    this._runLifecycleStep('start reporting', () =>
      this._engine.controller.startReporting()
    );
  }

  private _runLifecycleStep<T>(
    operation: string,
    action: () => T
  ): T | undefined {
    try {
      return action();
    } catch (error) {
      SessionLogger.error(`Error during lifecycle ${operation}`, error);
      return undefined;
    }
  }
}
