import { createChunk, isChunkEmpty, type Chunk } from '../models/chunk';
import {
  resolveSessionEnvironment,
  type SessionEnvironment,
} from '../models/environment';
import type { ActionEvent, ExplorationEvent } from '../models/gestures';
import type { Lom } from '../models/lom';
import type { Session } from '../types/session';
import { sameAnonymousRoute } from '../utils/AnonymousRoute';
import { SessionLogger } from '../utils/Logger';
import { createUuidV7 } from '../utils/Uuid';
import type { SessionRecorderEngine } from './Engine';
import { GesturesCollector } from './Gestures';
import { SessionRecorderState } from './State';
import {
  TreeDetector,
  type ActiveSurface,
  type InternalCaptureMetadata,
  type PointerDownLomResolution,
} from './TreeDetector';
export interface SessionRecorderContext {
  start(): void;
  currentAnonymousRoute(): string[] | undefined;
  setAnonymousRoute(ar: string[] | undefined): void;
  dispose(): void | Promise<void>;
  resume(): void;
  pause(): void | Promise<void>;
  suspendLomCapture(): void;
  resumeLomCapture(): void;
  extractChunk(): Chunk | null;
  recordAction(action: ActionEvent): void;
  recordExploration(exploration: ExplorationEvent): void;
  reconcileSurfaceInstances(surfaces: readonly ActiveSurface[]): boolean;
  recordLom(lom: Lom, surfaceInstance: string, publish?: boolean): void;
  notifyLomMaterialized(lom: Lom, metadata: InternalCaptureMetadata): void;
  invalidateCurrentLom(surfaceId: number, surfaceInstance: string): boolean;
  currentLom(active?: readonly ActiveSurface[]): Lom | null;
  currentLomRef(active?: readonly ActiveSurface[]): string | null;
  resolveDirtyLomForPointerDown(): PointerDownLomResolution;
  setScrollCaptureSuppressed(
    suppressed: boolean,
    restorePendingAutomaticCapture: boolean
  ): void;
  markPostScrollCapturePending(): void;
  hasPostScrollCapturePending(): boolean;
  currentPostScrollCaptureGeneration(): number;
  requestPostScrollCapture(): boolean;
  clearPostScrollCapturePending(): void;
}
export class NoOpContext implements SessionRecorderContext {
  private static readonly _instance = new NoOpContext();
  static instance(): NoOpContext {
    return this._instance;
  }
  private constructor() {}

  start(): void {}
  currentAnonymousRoute(): string[] | undefined {
    return undefined;
  }
  setAnonymousRoute(_: string[] | undefined): void {}
  dispose(): void {}
  resume(): void {}
  pause(): void {}
  suspendLomCapture(): void {}
  resumeLomCapture(): void {}
  extractChunk(): Chunk | null {
    return null;
  }

  recordAction(_: ActionEvent): void {}
  recordExploration(_: ExplorationEvent): void {}
  reconcileSurfaceInstances(_: readonly ActiveSurface[]): boolean {
    return false;
  }
  recordLom(_: Lom, __: string): void {}
  notifyLomMaterialized(_: Lom, __: InternalCaptureMetadata): void {}
  invalidateCurrentLom(_: number, __: string): boolean {
    return false;
  }

  currentLom(): Lom | null {
    return null;
  }

  currentLomRef(): string | null {
    return null;
  }
  resolveDirtyLomForPointerDown(): PointerDownLomResolution {
    return { lomRef: '', resolved: false, priorityOutcome: 'skipped' };
  }
  setScrollCaptureSuppressed(_: boolean, __: boolean): void {}
  markPostScrollCapturePending(): void {}
  hasPostScrollCapturePending(): boolean {
    return false;
  }
  currentPostScrollCaptureGeneration(): number {
    return 0;
  }
  requestPostScrollCapture(): boolean {
    return false;
  }
  clearPostScrollCapturePending(): void {}
}
export class ContextImpl implements SessionRecorderContext {
  private readonly _engine: SessionRecorderEngine;
  private _chunk: Chunk;
  // Desired context; only an accepted capture/native dedup materializes it.
  private _anonymousRoute: string[] | undefined;
  private readonly _lomsBySurface = new Map<number, Lom>();
  private readonly _activeSurfaceInstances = new Map<number, string>();
  private readonly _session: Session;
  private readonly _environment: SessionEnvironment | undefined;
  private _detector: TreeDetector | null = null;
  private _gestures: GesturesCollector | null = null;
  private _state: SessionRecorderState | null = null;
  private _pauseCompletion: Promise<void> | null = null;
  private _disposeCompletion: Promise<void> | null = null;
  private _isDisposed = false;

  constructor(engine: SessionRecorderEngine) {
    this._engine = engine;
    this._session = { id: createUuidV7() };
    this._environment = resolveSessionEnvironment();
    this._chunk = createChunk(this._session.id, this._environment);
  }

  public currentAnonymousRoute(): string[] | undefined {
    return this._anonymousRoute;
  }

  public setAnonymousRoute(ar: string[] | undefined): void {
    if (this._isDisposed || sameAnonymousRoute(this._anonymousRoute, ar))
      return;
    this._anonymousRoute = ar;
  }

  public start(): void {
    this._state = new SessionRecorderState(this._engine);
    this._state.listen();

    this._detector = new TreeDetector(this._engine);
    this._detector.detect();

    this._gestures = new GesturesCollector(this._engine);
    this._gestures.init();
  }

  public dispose(): void | Promise<void> {
    if (this._isDisposed) return this._disposeCompletion ?? undefined;
    this._isDisposed = true;
    this._anonymousRoute = undefined;
    const cleanup = (name: string, action: () => void): void => {
      try {
        action();
      } catch (error) {
        try {
          SessionLogger.error(`Error cleaning up ${name}`, error);
        } catch {}
      }
    };

    if (this._state) cleanup('lifecycle listener', () => this._state?.stop());
    cleanup('lifecycle activity', () =>
      this._engine.controller.stopReporting(true)
    );
    cleanup('capture suspension', () => this._detector?.pauseDetector());
    const pending = this._runLifecycleStep('dispose collectors', () =>
      this._gestures?.dispose()
    );
    const finish = (): void => {
      if (this._detector)
        cleanup('Fabric detector', () => this._detector?.dispose());
      this._state = null;
      this._detector = null;
      this._gestures = null;
      this._lomsBySurface.clear();
      this._activeSurfaceInstances.clear();
      this._chunk.loms.length = 0;
      this._chunk.actionEvents.length = 0;
      this._chunk.explorationEvents.length = 0;
    };
    if (!pending) {
      finish();
      return;
    }
    this._disposeCompletion = Promise.resolve(pending)
      .then(finish, (error) => {
        SessionLogger.error('Error awaiting collector disposal', error);
        finish();
      })
      .catch((error) =>
        SessionLogger.error('Error completing Context disposal', error)
      );
    return this._disposeCompletion;
  }

  public pause(): void | Promise<void> {
    if (this._isDisposed) return this._disposeCompletion ?? undefined;
    if (this._pauseCompletion) return this._pauseCompletion;
    this._runLifecycleStep('suspend activity', () =>
      this._engine.controller.stopReporting(true)
    );
    const pending = this._runLifecycleStep('pause collectors', () =>
      this._gestures?.pause()
    );
    this._runLifecycleStep('pause detector', () =>
      this._detector?.pauseDetector()
    );
    if (!pending) return;
    const completion = Promise.resolve(pending)
      .catch((error) =>
        SessionLogger.error('Error awaiting collector pause', error)
      )
      .then(() => {
        if (this._pauseCompletion === completion) this._pauseCompletion = null;
      });
    this._pauseCompletion = completion;
    return completion;
  }

  public suspendLomCapture(): void {
    this._detector?.pauseDetector();
  }

  public resumeLomCapture(): void {
    this._detector?.resumeDetector(true);
  }

  public resume(): void {
    if (this._isDisposed || this._pauseCompletion) return;
    this._runLifecycleStep('resume detector', () =>
      this._detector?.resumeDetector()
    );
    this._runLifecycleStep('resume collectors', () => this._gestures?.resume());
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

  public recordAction(action: ActionEvent): void {
    this._chunk.actionEvents.push(action);
    this._detector?.armInteractionConsequence();
    this._engine.controller.pingActivity();
  }
  public recordExploration(exploration: ExplorationEvent): void {
    this._chunk.explorationEvents.push(exploration);
    this._engine.controller.pingActivity();
  }

  public recordLom(lom: Lom, surfaceInstance: string, publish = true): void {
    if (
      !lom ||
      this._activeSurfaceInstances.get(lom.surfaceId) !== surfaceInstance
    )
      return;

    this._lomsBySurface.set(lom.surfaceId, lom);

    if (publish) this._chunk.loms.push(lom);
  }

  public notifyLomMaterialized(
    lom: Lom,
    metadata: InternalCaptureMetadata
  ): void {
    this._gestures?.adoptMaterializedLom(lom, metadata);
  }

  public invalidateCurrentLom(
    surfaceId: number,
    surfaceInstance: string
  ): boolean {
    if (this._activeSurfaceInstances.get(surfaceId) !== surfaceInstance)
      return false;
    // Only CURRENT is retired; chunk history and structural cache are untouched.
    return this._lomsBySurface.delete(surfaceId);
  }

  public reconcileSurfaceInstances(
    surfaces: readonly ActiveSurface[]
  ): boolean {
    const active = new Map(
      surfaces.map(
        ({ surfaceId, surfaceInstance }) =>
          [surfaceId, surfaceInstance] as const
      )
    );
    for (const [id, instance] of this._activeSurfaceInstances) {
      if (active.get(id) !== instance) this._lomsBySurface.delete(id);
    }
    this._activeSurfaceInstances.clear();
    for (const [id, instance] of active)
      this._activeSurfaceInstances.set(id, instance);
    // Chunk records are history; only the current association map is retired.
    return this._lomsBySurface.size > 0;
  }

  public extractChunk(): Chunk | null {
    if (isChunkEmpty(this._chunk)) return null;

    const chunk: Chunk = this._chunk;

    this._chunk = createChunk(this._session.id, this._environment);

    return chunk;
  }

  public currentLom(active?: readonly ActiveSurface[]): Lom | null {
    const surfaces = active ?? this._detector?.reconcileSurfaces();
    // Touch events currently carry no Fabric surface identity. A single active
    // surface is unambiguous; with several surfaces, fail open rather than
    // associate the interaction with an arbitrary LOM.
    if (!surfaces || surfaces.length !== 1) return null;
    const surface = surfaces[0];
    if (
      !surface ||
      this._activeSurfaceInstances.get(surface.surfaceId) !==
        surface.surfaceInstance
    )
      return null;
    const lom = this._lomsBySurface.get(surface.surfaceId);
    return lom?.ref.trim() ? lom : null;
  }

  public currentLomRef(active?: readonly ActiveSurface[]): string | null {
    return this.currentLom(active)?.ref.trim() ?? null;
  }

  public resolveDirtyLomForPointerDown(): PointerDownLomResolution {
    const detector = this._detector;
    const active = detector?.reconcileSurfaces();
    const unavailable: PointerDownLomResolution = {
      lomRef: '',
      resolved: false,
      priorityOutcome: 'skipped',
    };
    if (!detector || !active || active.length !== 1) return unavailable;
    const surface = active[0];
    if (!surface) return unavailable;
    const resolution = detector.resolveDirtyPointerDown(
      surface.surfaceId,
      surface.surfaceInstance,
      this.currentLomRef(active) ?? ''
    );
    // Reconcile the final CURRENT inside this same resolution, never at trace
    // creation. A replaced or ambiguous surface cannot confirm this Down.
    const finalActive = detector.reconcileSurfaces();
    const finalSurface = finalActive?.length === 1 ? finalActive[0] : undefined;
    if (
      !finalActive ||
      !finalSurface ||
      finalSurface.surfaceId !== surface.surfaceId ||
      finalSurface.surfaceInstance !== surface.surfaceInstance
    ) {
      return {
        ...resolution,
        lomRef: '',
        resolved: false,
        adoption: undefined,
      };
    }
    const lomRef = this.currentLomRef(finalActive) ?? '';
    return {
      ...resolution,
      lomRef,
      resolved:
        resolution.resolved &&
        lomRef.length > 0 &&
        lomRef === resolution.lomRef,
      priorityOutcome: resolution.priorityOutcome,
    };
  }

  public setScrollCaptureSuppressed(
    suppressed: boolean,
    restorePendingAutomaticCapture: boolean
  ): void {
    this._detector?.setScrollSuppressed(
      suppressed,
      restorePendingAutomaticCapture
    );
  }

  public markPostScrollCapturePending(): void {
    this._detector?.markPostScrollCapturePending();
  }

  public hasPostScrollCapturePending(): boolean {
    return this._detector?.hasPostScrollCapturePending() ?? false;
  }

  public currentPostScrollCaptureGeneration(): number {
    return this._detector?.currentPostScrollCaptureGeneration() ?? 0;
  }

  public requestPostScrollCapture(): boolean {
    return this._detector?.requestPostScrollCapture() ?? false;
  }

  public clearPostScrollCapturePending(): void {
    this._detector?.clearPostScrollCapturePending();
  }
}
