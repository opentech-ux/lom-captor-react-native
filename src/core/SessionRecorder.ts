import {
  ResolvedSessionRecorderConfig,
  type SessionRecorderConfig,
} from '../models/config';
import { anonymousRoute, sameAnonymousRoute } from '../utils/AnonymousRoute';
import { SessionLogger } from '../utils/Logger';
import {
  NoOpSessionRecorderEngine,
  SessionRecorderEngine,
  type SessionRecorderEngineInternal,
} from './Engine';

let engine: SessionRecorderEngineInternal =
  NoOpSessionRecorderEngine.instance();
let isInitializing = false;
// Only anonymized context is retained before activation.
let pendingAnonymousRoute: string[] | undefined;

/**
 * This getter is solely for internal communication within the SDK
 *
 * Accessing or manipulating this engine directly from your application code
 * bypasses all safety checks. Doing so will corrupt the Session Recorder
 * state, cause unexpected memory leaks, and potentially crash the host
 * application.
 * * @internal
 */
export const getEngine = (): SessionRecorderEngineInternal => {
  return engine;
};

export class SessionRecorder {
  private constructor() {}

  /** Optional logical screen context; no navigation integration is required. */
  static setScreenName(name: string): void {
    if (typeof name !== 'string' || name.length === 0) {
      return;
    }
    const ar = anonymousRoute(name);
    if (engine.isEnabled) {
      engine.context.setAnonymousRoute(ar);
    } else if (!sameAnonymousRoute(pendingAnonymousRoute, ar)) {
      pendingAnonymousRoute = ar;
    }
  }

  static init(config: SessionRecorderConfig): void {
    if (!config) {
      throw new Error(
        'FATAL ERROR: [SessionRecorderConfig] params must be provided in init({}) function'
      );
    }

    if (engine.isEnabled || isInitializing) {
      SessionLogger.warning('SessionRecorder already initialized');
      return;
    }

    isInitializing = true;
    let candidate: SessionRecorderEngine | null = null;
    let rollbackPending = false;
    try {
      const resolvedConfig = new ResolvedSessionRecorderConfig(config);

      SessionLogger.configure({
        debugLog: resolvedConfig.debugLog,
        logger: resolvedConfig.logger,
      });

      resolvedConfig.validate();

      candidate = new SessionRecorderEngine(resolvedConfig);
      candidate.context.setAnonymousRoute(pendingAnonymousRoute);
      candidate.start();
      // A call during startup may have updated the pre-init context.
      candidate.context.setAnonymousRoute(pendingAnonymousRoute);
      engine = candidate;
      pendingAnonymousRoute = undefined;
    } catch (error) {
      engine = NoOpSessionRecorderEngine.instance();
      try {
        const pending = candidate?.dispose();
        if (pending) {
          rollbackPending = true;
          Promise.resolve(pending).then(
            () => {
              isInitializing = false;
            },
            (cleanupError) => {
              try {
                SessionLogger.error(
                  'Error awaiting Session Recorder rollback',
                  cleanupError
                );
              } catch {
              } finally {
                isInitializing = false;
              }
            }
          );
        }
      } catch (cleanupError) {
        try {
          SessionLogger.error(
            'Error rolling back Session Recorder startup',
            cleanupError
          );
        } catch {}
      }
      try {
        SessionLogger.error('Cannot initialized the SDK', error);
      } catch {}
    } finally {
      // The old engine owns shared JSI globals until rollback has completed.
      if (!rollbackPending) isInitializing = false;
    }
  }
}
