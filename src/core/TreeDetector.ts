import { Platform } from 'react-native';
import type { Lom } from '../models/lom';
import { sameAnonymousRoute } from '../utils/AnonymousRoute';
import { SessionLogger } from '../utils/Logger';
import type { SessionRecorderEngine } from './Engine';
import { LomTreeInspector } from './TreeInspector';

type CaptureStatus = 'captured' | 'deduplicated' | 'unavailable';

export interface InternalCaptureMetadata {
  /** Internal capture provenance, omitted from the public wire. */
  source?: string;
  surfaceInstance: string;
  requestId: number;
  debtGeneration: number;
  captureGeneration: number;
  viewportWidth: number;
  viewportHeight: number;
  revisionNumber: number;
  pagerVersion: number;
  /** Native visibility evidence for this attempt only; never serialized. */
  visibilityGeneration?: number;
  /** Private physical-clock provenance; never serialized into a LOM/event. */
  scanActivation?: string;
  scanStart?: number;
  scanEnd?: number;
}

interface DirtyCaptureResponse extends InternalCaptureMetadata {
  dirty: boolean;
  status: 'clean' | 'captured' | 'unavailable';
  priorityAttempted: boolean;
  surfaceId: number;
  latestRevision: number;
  materializedRevision: number;
  observedGeneration: number;
  signature?: string;
  roots?: unknown[];
}

interface ExplicitCaptureState {
  requestId: number;
  debtGeneration: number;
  resultStatus: CaptureStatus | null;
  matchingLomMaterialized: boolean;
  nonInformative: boolean;
}

export interface ActiveSurface {
  surfaceId: number;
  surfaceInstance: string;
}

export type PointerDownCaptureResult = 'attempted' | 'rejected' | 'skipped';

export interface PointerDownLomResolution {
  readonly lomRef: string;
  readonly resolved: boolean;
  readonly priorityOutcome: PointerDownCaptureResult;
  readonly adoption?: {
    readonly surfaceId: number;
    readonly surfaceInstance: string;
    readonly captureGeneration: number;
    readonly revisionNumber: number;
  };
}

export class TreeDetector {
  private _engine: SessionRecorderEngine;
  private _inspector: LomTreeInspector;

  private _isRunning: boolean = false;
  private _activationCompleted = false;

  private _retryCount = 0;
  private readonly MAX_RETRIES = 3;
  private _retryTimer: ReturnType<typeof setTimeout> | null = null;
  private _hasLom = false;
  private _postScrollCapturePending = false;
  private _scrollSuppressed = false;
  private _captureRequestCounter = 0;
  private _captureGeneration = 0;
  private _debtGenerationCounter = 0;
  private _currentDebtGeneration = 0;
  private _pointerDownAttemptedDebtGeneration: number | null = null;
  private _explicitCaptureInFlight: ExplicitCaptureState | null = null;
  private _lastAcceptedMaterialization: {
    surfaceId: number;
    surfaceInstance: string;
    captureGeneration: number;
    ref: string;
  } | null = null;
  private readonly _examinedRevisionsBySurface = new Map<
    number,
    {
      surfaceInstance: string;
      revisionNumber: number;
      pagerVersion: number;
      visibilityGeneration?: number;
      nonInformative: boolean;
    }
  >();

  constructor(engine: SessionRecorderEngine) {
    this._engine = engine;
    this._inspector = new LomTreeInspector(engine.config);
  }

  public detect(): void {
    if (this._isRunning) return;

    this._isRunning = true;
    if (!this._startNativeObservation(true, true)) {
      throw new Error('Cannot start the Fabric tree observer');
    }
  }

  public armInteractionConsequence(): void {
    try {
      if (!this._isRunning) return;
      const surfaces = this.reconcileSurfaces();
      if (!surfaces || surfaces.length !== 1) return;
      const surface = surfaces[0]!;
      (globalThis as any).__viewTreeScanner?.armInteractionConsequence?.(
        surface.surfaceId,
        surface.surfaceInstance,
        this._captureGeneration
      );
    } catch {
      // The Action is already recorded; consequence capture is best-effort.
    }
  }

  private _startNativeObservation(
    requestInitialCapture: boolean = true,
    failOnStartError: boolean = false
  ): boolean {
    if (!this._isRunning) return false;
    let didStartSuccessfully = false;
    try {
      const scanner = (globalThis as any).__viewTreeScanner;

      if (!scanner) {
        throw new Error('C++ JSI not found');
      }

      const captureGeneration = scanner.setPaused?.(false);
      if (typeof captureGeneration === 'number') {
        this._captureGeneration = captureGeneration;
      }

      // * LISTEN TO TREE UPDATE
      (globalThis as any).__onSessionRecorderTreeUpdate = (
        surfaceId: number,
        payload: any,
        metadata?: InternalCaptureMetadata
      ) => {
        this._handleTree(surfaceId, payload, metadata);
      };
      (globalThis as any).__onSessionRecorderCaptureResult = (
        requestId: number,
        debtGeneration: number,
        status: CaptureStatus,
        resultCaptureGeneration: number
      ) => {
        this._handleCaptureResult(
          requestId,
          debtGeneration,
          status,
          resultCaptureGeneration
        );
      };

      // Both native entrypoints deliver asynchronously through the callback above.
      const didStart =
        this._retryCount >= this.MAX_RETRIES
          ? scanner.scan(requestInitialCapture)
          : scanner.startObserver(requestInitialCapture);

      if (didStart === true || (typeof didStart === 'string' && didStart)) {
        didStartSuccessfully = true;
        SessionLogger.verbose(
          `Native Observer has started correctly: ${didStart}`
        );
        this._retryCount = 0;
      } else {
        SessionLogger.warning(`Native Observer has failed: ${didStart}`);

        this._retryCount++;
        if (failOnStartError) {
          throw new Error('Native Fabric observer did not start');
        }
      }
    } catch (error) {
      if (failOnStartError) throw error;
      SessionLogger.error('Error capturing the tree', error);
    } finally {
      this._activationCompleted = didStartSuccessfully;
      if (!didStartSuccessfully) {
        (globalThis as any).__onSessionRecorderTreeUpdate = undefined;
        (globalThis as any).__onSessionRecorderCaptureResult = undefined;
      }
      this._scheduleInitialRetry();
    }
    return didStartSuccessfully;
  }

  private _handleTree(
    surfaceId: number,
    payload: any,
    metadata?: InternalCaptureMetadata
  ): void {
    if (!this._isRunning) return;
    if (metadata && metadata.captureGeneration !== this._captureGeneration)
      return;
    if (!payload || !payload.roots) return;
    const scanner = (globalThis as any).__viewTreeScanner;
    if (
      !metadata ||
      scanner?.isSurfaceInstanceCurrent?.(
        surfaceId,
        metadata.surfaceInstance
      ) !== true
    )
      return;

    const lom = this._inspector.createLom(
      surfaceId,
      payload,
      metadata,
      this._engine.context.currentAnonymousRoute()
    );

    if (!lom) return;
    if ('nonInformative' in lom) {
      this._handleNonInformativeCapture(surfaceId, metadata);
      return;
    }
    if (this._isBlockedByNonInformativeRevision(surfaceId, metadata)) {
      this._inspector.discardUnrecordedLom(lom);
      this._markNonInformativeRequest(metadata);
      return;
    }

    if (metadata?.revisionNumber !== undefined) {
      const accepted = scanner.acceptMaterializedRevision?.(
        surfaceId,
        metadata.revisionNumber,
        metadata.captureGeneration,
        metadata.surfaceInstance,
        metadata.pagerVersion,
        metadata.visibilityGeneration
      );
      if (accepted !== true) {
        this._inspector.discardUnrecordedLom(lom);
        return;
      }
    }

    if (!this._recordAcceptedLom(lom, metadata)) {
      this._inspector.discardUnrecordedLom(lom);
      return;
    }
    this._clearRetry();
    this._handleMaterializedCapture(metadata);
  }

  private _isBlockedByNonInformativeRevision(
    surfaceId: number,
    metadata: InternalCaptureMetadata
  ): boolean {
    const examined = this._examinedRevisionsBySurface.get(surfaceId);
    return (
      examined?.surfaceInstance === metadata.surfaceInstance &&
      examined.pagerVersion === metadata.pagerVersion &&
      ((Platform.OS !== 'android' && Platform.OS !== 'ios') ||
        examined.visibilityGeneration === metadata.visibilityGeneration) &&
      examined.nonInformative &&
      metadata.revisionNumber <= examined.revisionNumber
    );
  }

  private _handleNonInformativeCapture(
    surfaceId: number,
    metadata: InternalCaptureMetadata
  ): boolean {
    const scanner = (globalThis as any).__viewTreeScanner;
    if (
      !this._isRunning ||
      metadata.captureGeneration !== this._captureGeneration ||
      !Number.isSafeInteger(metadata.revisionNumber) ||
      metadata.revisionNumber < 0 ||
      scanner?.isSurfaceInstanceCurrent?.(
        surfaceId,
        metadata.surfaceInstance
      ) !== true
    )
      return false;
    const active = this.reconcileSurfaces();
    if (
      !active?.some(
        (surface) =>
          surface.surfaceId === surfaceId &&
          surface.surfaceInstance === metadata.surfaceInstance
      )
    )
      return false;

    this._markNonInformativeRequest(metadata);
    const examined = this._examinedRevisionsBySurface.get(surfaceId);
    if (
      examined?.surfaceInstance === metadata.surfaceInstance &&
      examined.pagerVersion === metadata.pagerVersion &&
      ((Platform.OS !== 'android' && Platform.OS !== 'ios') ||
        examined.visibilityGeneration === metadata.visibilityGeneration) &&
      (metadata.revisionNumber < examined.revisionNumber ||
        (!examined.nonInformative &&
          metadata.revisionNumber === examined.revisionNumber))
    )
      return false;

    // This is examination, not an ACK or an accepted native dedup baseline.
    this._examinedRevisionsBySurface.set(surfaceId, {
      surfaceInstance: metadata.surfaceInstance,
      revisionNumber: metadata.revisionNumber,
      pagerVersion: metadata.pagerVersion,
      visibilityGeneration: metadata.visibilityGeneration,
      nonInformative: true,
    });
    this._engine.context.invalidateCurrentLom(
      surfaceId,
      metadata.surfaceInstance
    );
    this._hasLom = this._engine.context.reconcileSurfaceInstances(active);
    return true;
  }

  private _markNonInformativeRequest(metadata: InternalCaptureMetadata): void {
    const request = this._explicitCaptureInFlight;
    if (
      metadata.requestId !== 0 &&
      request !== null &&
      request.requestId === metadata.requestId &&
      request.debtGeneration === metadata.debtGeneration
    ) {
      request.nonInformative = true;
      this._tryCompleteExplicitCapture(request);
    }
  }

  private _hasNonInformativeSurface(): boolean {
    for (const examined of this._examinedRevisionsBySurface.values()) {
      if (examined.nonInformative) return true;
    }
    return false;
  }

  private _recordAcceptedLom(
    lom: Lom,
    metadata?: InternalCaptureMetadata
  ): boolean {
    if (!metadata) return false;
    if (this._isBlockedByNonInformativeRevision(lom.surfaceId, metadata))
      return false;
    const active = this.reconcileSurfaces();
    if (
      !active?.some(
        (surface) =>
          surface.surfaceId === lom.surfaceId &&
          surface.surfaceInstance === metadata.surfaceInstance
      )
    )
      return false;
    const current = this._engine.context.currentLom(active);
    // Reconciliation scopes continuity to this active instance and occurrence context.
    const sameConsecutiveLomState =
      lom.fromCache &&
      current != null &&
      lom.ref === current.ref &&
      sameAnonymousRoute(lom.ar, current.ar);

    this._engine.context.recordLom(
      lom,
      metadata.surfaceInstance,
      !sameConsecutiveLomState
    );

    // Called only after acceptance. Keep tracking even when a consecutive
    // equivalent state reuses the current ref without a wire record.
    this._lastAcceptedMaterialization =
      Number.isSafeInteger(metadata.captureGeneration) &&
      metadata.captureGeneration >= 0 &&
      Number.isSafeInteger(metadata.revisionNumber) &&
      metadata.revisionNumber >= 0
        ? {
            surfaceId: lom.surfaceId,
            surfaceInstance: metadata.surfaceInstance,
            captureGeneration: metadata.captureGeneration,
            ref: lom.ref,
          }
        : null;
    this._hasLom = true;
    if (
      Number.isSafeInteger(metadata.revisionNumber) &&
      metadata.revisionNumber >= 0
    ) {
      this._examinedRevisionsBySurface.set(lom.surfaceId, {
        surfaceInstance: metadata.surfaceInstance,
        revisionNumber: metadata.revisionNumber,
        pagerVersion: metadata.pagerVersion,
        visibilityGeneration: metadata.visibilityGeneration,
        nonInformative: false,
      });
    }
    this._engine.context.notifyLomMaterialized(lom, metadata);
    return true;
  }

  private _refreshAnonymousRoute(
    surfaceId: number,
    surfaceInstance: string,
    ref: string
  ): void {
    if (!this._isRunning) return;
    const active = this.reconcileSurfaces();
    if (
      active?.length !== 1 ||
      active[0]?.surfaceId !== surfaceId ||
      active[0]?.surfaceInstance !== surfaceInstance
    )
      return;
    const current = this._engine.context.currentLom(active);
    if (!current || current.ref !== ref) return;
    const ar = this._engine.context.currentAnonymousRoute();
    if (sameAnonymousRoute(ar, current.ar)) return;

    this._engine.context.recordLom(
      {
        surfaceId,
        id: current.id,
        ref,
        timestamp: Date.now(),
        root: current.root,
        fromCache: true,
        ...(ar !== undefined ? { ar } : {}),
      },
      surfaceInstance
    );
  }

  private _refreshDeduplicatedContext(): void {
    const accepted = this._lastAcceptedMaterialization;
    if (!accepted || accepted.captureGeneration !== this._captureGeneration)
      return;
    try {
      this._refreshAnonymousRoute(
        accepted.surfaceId,
        accepted.surfaceInstance,
        accepted.ref
      );
    } catch {
      // Context-only metadata must not interfere with native freshness/debt settlement.
    }
  }

  public reconcileSurfaces(): ActiveSurface[] | null {
    try {
      const active: unknown = (
        globalThis as any
      ).__viewTreeScanner?.getActiveSurfaces?.();
      if (!Array.isArray(active)) return null;
      const ids = new Set<number>();
      for (const surface of active) {
        if (
          !surface ||
          !Number.isSafeInteger(surface.surfaceId) ||
          surface.surfaceId < 0 ||
          typeof surface.surfaceInstance !== 'string' ||
          surface.surfaceInstance.length === 0 ||
          ids.has(surface.surfaceId)
        )
          return null;
        ids.add(surface.surfaceId);
      }
      this._hasLom = this._engine.context.reconcileSurfaceInstances(active);
      for (const [surfaceId, examined] of this._examinedRevisionsBySurface) {
        if (
          !active.some(
            (surface) =>
              surface.surfaceId === surfaceId &&
              surface.surfaceInstance === examined.surfaceInstance
          )
        )
          this._examinedRevisionsBySurface.delete(surfaceId);
      }
      const previous = this._lastAcceptedMaterialization;
      if (
        previous &&
        !active.some(
          (surface) =>
            surface.surfaceId === previous.surfaceId &&
            surface.surfaceInstance === previous.surfaceInstance
        )
      )
        this._lastAcceptedMaterialization = null;
      return active;
    } catch (error) {
      SessionLogger.error('Error reconciling Fabric surfaces', error);
      return null;
    }
  }

  public resolveDirtyPointerDown(
    surfaceId: number,
    surfaceInstance: string,
    currentRef: string
  ): PointerDownLomResolution {
    let result: PointerDownLomResolution = {
      lomRef: currentRef,
      resolved: false,
      priorityOutcome: 'skipped',
    };
    if (!this._isRunning) return result;
    const priorityDebtGeneration =
      this._postScrollCapturePending &&
      this._currentDebtGeneration !== 0 &&
      !this._explicitCaptureInFlight &&
      this._pointerDownAttemptedDebtGeneration !== this._currentDebtGeneration
        ? this._currentDebtGeneration
        : 0;
    if (priorityDebtGeneration === 0 && this._scrollSuppressed) return result;

    try {
      const scanner = (globalThis as any).__viewTreeScanner;
      if (typeof scanner?.captureIfDirty !== 'function') {
        return result;
      }
      if (priorityDebtGeneration !== 0)
        result = { ...result, priorityOutcome: 'rejected' };
      const capture = scanner.captureIfDirty(
        surfaceId,
        surfaceInstance,
        priorityDebtGeneration
      ) as DirtyCaptureResponse;
      if (
        priorityDebtGeneration !== 0 &&
        capture.priorityAttempted === true &&
        capture.debtGeneration === priorityDebtGeneration
      ) {
        if (this._currentDebtGeneration === priorityDebtGeneration) {
          this._pointerDownAttemptedDebtGeneration = priorityDebtGeneration;
          result = { ...result, priorityOutcome: 'attempted' };
        }
      }
      if (capture.status === 'clean') {
        const resolved =
          currentRef.length > 0 &&
          !this._postScrollCapturePending &&
          priorityDebtGeneration === 0 &&
          capture.captureGeneration === this._captureGeneration &&
          capture.surfaceId === surfaceId &&
          capture.surfaceInstance === surfaceInstance &&
          Number.isSafeInteger(capture.latestRevision) &&
          capture.latestRevision >= 0 &&
          Number.isSafeInteger(capture.materializedRevision) &&
          capture.materializedRevision >= capture.latestRevision &&
          scanner.isSurfaceInstanceCurrent?.(surfaceId, surfaceInstance) ===
            true;
        return { ...result, resolved };
      }
      if (
        Platform.OS === 'ios' &&
        capture.status === 'unavailable' &&
        capture.dirty === true &&
        capture.captureGeneration === this._captureGeneration &&
        capture.surfaceId === surfaceId &&
        capture.surfaceInstance === surfaceInstance &&
        Number.isSafeInteger(capture.latestRevision) &&
        capture.latestRevision >= 0 &&
        scanner.isSurfaceInstanceCurrent?.(surfaceId, surfaceInstance) === true
      ) {
        result = {
          ...result,
          adoption: {
            surfaceId,
            surfaceInstance,
            captureGeneration: capture.captureGeneration,
            revisionNumber: capture.latestRevision,
          },
        };
      }

      if (
        capture.status !== 'captured' ||
        capture.captureGeneration !== this._captureGeneration ||
        !Number.isSafeInteger(capture.revisionNumber) ||
        (priorityDebtGeneration !== 0 &&
          (capture.priorityAttempted !== true ||
            capture.debtGeneration !== priorityDebtGeneration ||
            this._currentDebtGeneration !== priorityDebtGeneration))
      ) {
        return result;
      }

      if (
        capture.surfaceId !== surfaceId ||
        capture.surfaceInstance !== surfaceInstance ||
        scanner.isSurfaceInstanceCurrent?.(surfaceId, surfaceInstance) !== true
      )
        return result;
      const lom = this._inspector.createLom(
        capture.surfaceId,
        capture,
        capture,
        this._engine.context.currentAnonymousRoute()
      );
      if (!lom) {
        return result;
      }
      if ('nonInformative' in lom) {
        if (this._handleNonInformativeCapture(surfaceId, capture))
          return { ...result, lomRef: '', resolved: false };
        return result;
      }
      if (this._isBlockedByNonInformativeRevision(surfaceId, capture)) {
        this._inspector.discardUnrecordedLom(lom);
        return { ...result, lomRef: '', resolved: false };
      }

      const accepted = scanner.acceptMaterializedRevision?.(
        capture.surfaceId,
        capture.revisionNumber,
        capture.captureGeneration,
        capture.surfaceInstance,
        capture.pagerVersion,
        capture.visibilityGeneration
      );
      if (accepted !== true) {
        this._inspector.discardUnrecordedLom(lom);
        return result;
      }

      if (!this._recordAcceptedLom(lom, capture)) {
        this._inspector.discardUnrecordedLom(lom);
        return result;
      }
      this._clearRetry();
      this._handleMaterializedCapture({
        requestId: 0,
        debtGeneration: capture.debtGeneration,
        captureGeneration: capture.captureGeneration,
        surfaceInstance: capture.surfaceInstance,
        viewportWidth: capture.viewportWidth,
        viewportHeight: capture.viewportHeight,
        revisionNumber: capture.revisionNumber,
        pagerVersion: capture.pagerVersion,
      });
      result = {
        ...result,
        lomRef: lom.ref,
        resolved: lom.ref.trim().length > 0 && !this._postScrollCapturePending,
      };
    } catch (error) {
      SessionLogger.error('Error resolving dirty PointerDown LOM', error);
    }
    return result;
  }

  private _scheduleInitialRetry(): void {
    if (
      !this._isRunning ||
      this._hasLom ||
      this._postScrollCapturePending ||
      this._scrollSuppressed ||
      this._retryTimer !== null
    )
      return;
    this._retryTimer = setTimeout(() => {
      this._retryTimer = null;
      this._startNativeObservation();
    }, 500);
  }

  private _clearRetry(): void {
    if (this._retryTimer === null) return;
    clearTimeout(this._retryTimer);
    this._retryTimer = null;
  }

  public pauseDetector(): void {
    this._isRunning = false;
    this._activationCompleted = false;
    this._clearRetry();
    this._explicitCaptureInFlight = null;

    try {
      const captureGeneration = (
        globalThis as any
      ).__viewTreeScanner?.setPaused?.(true);
      if (typeof captureGeneration === 'number') {
        this._captureGeneration = captureGeneration;
      }
    } catch (error) {
      SessionLogger.error('Error pausing native tree capture', error);
    }

    (globalThis as any).__onSessionRecorderTreeUpdate = undefined;
    (globalThis as any).__onSessionRecorderCaptureResult = undefined;
  }

  public dispose(): void {
    this._isRunning = false;
    this._activationCompleted = false;
    this._clearRetry();
    this._explicitCaptureInFlight = null;
    this._postScrollCapturePending = false;
    this._currentDebtGeneration = 0;
    this._pointerDownAttemptedDebtGeneration = null;
    this._examinedRevisionsBySurface.clear();

    (globalThis as any).__onSessionRecorderTreeUpdate = undefined;
    (globalThis as any).__onSessionRecorderCaptureResult = undefined;

    try {
      const scanner = (globalThis as any).__viewTreeScanner;
      if (typeof scanner?.stopObserver === 'function') {
        scanner.stopObserver();
      } else {
        scanner?.setPaused?.(true);
      }
    } catch (error) {
      SessionLogger.error('Error unregistering native tree observer', error);
    }
  }

  public resumeDetector(requestInitialCapture?: boolean): void {
    this.reconcileSurfaces();
    if (this._isRunning && this._activationCompleted) return;
    this._isRunning = true;
    this._startNativeObservation(
      requestInitialCapture ?? !this._postScrollCapturePending
    );
  }

  public setScrollSuppressed(
    suppressed: boolean,
    restorePendingAutomaticCapture: boolean
  ): void {
    this._scrollSuppressed = suppressed;
    if (suppressed) {
      this._clearRetry();
      this._explicitCaptureInFlight = null;
    }

    try {
      (globalThis as any).__viewTreeScanner?.setScrollSuppressed?.(
        suppressed,
        restorePendingAutomaticCapture
      );
    } catch (error) {
      SessionLogger.error('Error updating scroll capture suppression', error);
    }
    if (!suppressed) this._scheduleInitialRetry();
  }

  public markPostScrollCapturePending(): void {
    this._postScrollCapturePending = true;
    this._currentDebtGeneration = ++this._debtGenerationCounter;
    this._pointerDownAttemptedDebtGeneration = null;
    this._explicitCaptureInFlight = null;
    this._setNativeDebtGeneration(this._currentDebtGeneration);
    this._clearRetry();
  }

  public hasPostScrollCapturePending(): boolean {
    return this._postScrollCapturePending;
  }

  public currentPostScrollCaptureGeneration(): number {
    return this._postScrollCapturePending ? this._currentDebtGeneration : 0;
  }

  public requestPostScrollCapture(): boolean {
    if (
      !this._isRunning ||
      this._scrollSuppressed ||
      !this._postScrollCapturePending ||
      this._explicitCaptureInFlight
    )
      return false;

    const scanner = (globalThis as any).__viewTreeScanner;
    if (!scanner?.scanPostScroll) return false;

    const requestId = ++this._captureRequestCounter;
    const debtGeneration = this._currentDebtGeneration;
    try {
      const accepted = scanner.scanPostScroll(requestId, debtGeneration);
      if (accepted === true) {
        this._explicitCaptureInFlight = {
          requestId,
          debtGeneration,
          resultStatus: null,
          matchingLomMaterialized: false,
          nonInformative: false,
        };
        return true;
      }
    } catch (error) {
      SessionLogger.error('Error requesting post-scroll LOM capture', error);
    }
    return false;
  }

  public clearPostScrollCapturePending(): void {
    this._postScrollCapturePending = false;
    this._currentDebtGeneration = 0;
    this._pointerDownAttemptedDebtGeneration = null;
    this._explicitCaptureInFlight = null;
    this._setNativeDebtGeneration(0);
  }

  private _handleMaterializedCapture(metadata?: InternalCaptureMetadata): void {
    if (
      !metadata ||
      metadata.debtGeneration === 0 ||
      metadata.debtGeneration !== this._currentDebtGeneration
    )
      return;

    if (metadata.requestId === 0) {
      this._satisfyPostScrollCapture(metadata.debtGeneration);
      return;
    }

    const request = this._explicitCaptureInFlight;
    if (
      !request ||
      request.requestId !== metadata.requestId ||
      request.debtGeneration !== metadata.debtGeneration
    )
      return;

    request.matchingLomMaterialized = true;
    this._tryCompleteExplicitCapture(request);
  }

  private _handleCaptureResult(
    requestId: number,
    debtGeneration: number,
    status: CaptureStatus,
    captureGeneration: number
  ): void {
    if (captureGeneration !== this._captureGeneration) return;
    if (requestId === 0) {
      if (status === 'deduplicated' && !this._hasNonInformativeSurface()) {
        this._refreshDeduplicatedContext();
        this._satisfyPostScrollCapture(debtGeneration);
      }
      return;
    }

    const request = this._explicitCaptureInFlight;
    if (
      !request ||
      request.requestId !== requestId ||
      request.debtGeneration !== debtGeneration
    )
      return;

    if (status === 'deduplicated' && !this._hasNonInformativeSurface())
      this._refreshDeduplicatedContext();
    request.resultStatus = status;
    if (status === 'unavailable') {
      this._explicitCaptureInFlight = null;
      return;
    }
    this._tryCompleteExplicitCapture(request);
  }

  private _tryCompleteExplicitCapture(request: ExplicitCaptureState): void {
    if (
      request !== this._explicitCaptureInFlight ||
      request.debtGeneration !== this._currentDebtGeneration
    )
      return;

    if (
      request.resultStatus !== null &&
      (request.nonInformative ||
        (request.resultStatus === 'deduplicated' &&
          this._hasNonInformativeSurface()))
    ) {
      // The request completed without a usable LOM. Release it, not its debt.
      this._explicitCaptureInFlight = null;
      return;
    }

    if (
      request.resultStatus === 'deduplicated' ||
      (request.resultStatus === 'captured' && request.matchingLomMaterialized)
    ) {
      this._satisfyPostScrollCapture(request.debtGeneration);
    }
  }

  private _satisfyPostScrollCapture(debtGeneration: number): void {
    if (
      !this._postScrollCapturePending ||
      debtGeneration === 0 ||
      debtGeneration !== this._currentDebtGeneration
    )
      return;

    this._postScrollCapturePending = false;
    this._currentDebtGeneration = 0;
    this._pointerDownAttemptedDebtGeneration = null;
    this._explicitCaptureInFlight = null;
    this._setNativeDebtGeneration(0);
  }

  private _setNativeDebtGeneration(generation: number): void {
    try {
      (globalThis as any).__viewTreeScanner?.setPostScrollDebtGeneration?.(
        generation
      );
    } catch (error) {
      SessionLogger.error('Error updating post-scroll debt generation', error);
    }
  }
}
