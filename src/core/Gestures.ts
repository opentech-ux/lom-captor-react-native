import {
  NativeEventEmitter,
  NativeModules,
  Platform,
  type EventSubscription,
} from 'react-native';
import {
  doubleTapSlop,
  doubleTapTimeoutMs,
  longPressTimeoutMs,
  touchSlop,
} from '../const/gestures_constants';
import {
  createDoubleTapEvent,
  createPinchEvent,
  createScrollEvent,
  createTapEvent,
  splitForTransition,
  type ExplorationEvent,
  type GesturesType,
  type GestureTrace,
  type NativeGesturesEvent,
  type NativeScrollBounds,
  type NativeScrollRemovedSignal,
  type NativeScrollSignal,
  type NativeTouchOwnershipAbandonedEvent,
  type PinchMetricsBaseline,
  type PinchTrack,
  type TimedPosition,
} from '../models/gestures';
import type { Lom } from '../models/lom';
import type { Offset, Rect } from '../types/geometry';
import {
  distance,
  duration,
  getFirst,
  getLast,
  getLastPosition,
  getLastTimestamp,
  isEmpty,
} from '../utils/GesturesUtils';
import { SessionLogger } from '../utils/Logger';
import {
  evaluatePinchGesture,
  getAverageDistance,
  getCentroid,
} from '../utils/MathUtils';
import { createUuidV7 } from '../utils/Uuid';
import type { SessionRecorderEngine } from './Engine';
import { _appendTerminalPosition, _recordPosition } from './GestureTraceBuffer';
import {
  _createActionEvent,
  _createExplorationEvent,
} from './GestureTraceEvents';
import type {
  InternalCaptureMetadata,
  PointerDownLomResolution,
} from './TreeDetector';

interface PendingScrollSession {
  readonly activationToken?: string;
  readonly token: number;
  readonly viewId: number;
  readonly timestamp: number;
  readonly viewport: Rect;
  readonly offset: Offset;
  readonly lomRef: string;
  lastKnownViewport: Rect;
  lastKnownOffset: Offset;
  validated: boolean;
}

interface PendingTap {
  readonly firstPosition: TimedPosition;
  readonly terminalPosition: TimedPosition;
  readonly downOrder: number;
  readonly upOrder: number;
  readonly lomRef: string | null;
  readonly lomResolvedAtDown: boolean;
}

interface PinchTrackState {
  readonly pointerId: number;
  readonly entryTimestamp: number;
  readonly entryReportingTimestamp: number;
  exitTimestamp: number | null;
  exitReportingTimestamp: number | null;
  readonly positions: TimedPosition[];
  latestRaw: TimedPosition;
  previousRaw: TimedPosition | null;
  sampleIntervalMs: number;
}

const MAX_RETAINED_CLOSED_PINCH_TRACKS = 64;
const BASE_POSITION_SAMPLE_INTERVAL_MS = 50;
// Budget for one bridge round trip and one UI task, not scroll stabilization.
const SCROLL_SNAPSHOT_TIMEOUT_MS = 200;

interface PinchSessionState {
  readonly startTimestamp: number;
  readonly startReportingTimestamp: number;
  readonly viewport: Rect;
  readonly lomRef: string | null;
  /** Time-ordered retained history; closed intermediates are compacted by budget. */
  readonly tracks: PinchTrackState[];
  /** Participants present when the Pinch session opens survive compaction. */
  readonly initialTracks: ReadonlySet<PinchTrackState>;
  /** O(1) lookup for the currently active incarnation of each pointer ID. */
  readonly activeTracks: Map<number, PinchTrackState>;
}

// Android MotionEvent pointer IDs are limited to 0..31, bounding active traces
// and active tracks to 32 while the native MotionEvent source is in use.

export class GesturesCollector {
  private readonly _engine: SessionRecorderEngine;

  private _gestureSubscription: EventSubscription | null = null;
  private _scrollSubscriptions: EventSubscription[] = [];
  private _scrollRemoveSubscription: EventSubscription | null = null;

  private _gestures = new Map<number, GestureTrace>();
  private _pendingScrolls = new Map<number, PendingScrollSession>();
  private _pagerCaptureActive = false;
  private _pagerStateVersion = -1;
  private _pagerSubscription: EventSubscription | null = null;
  private _scrollTokenByView = new Map<number, number>();
  private _postScrollCaptureTimer: ReturnType<typeof setTimeout> | null = null;
  private _postScrollRetryUsedGeneration: number | null = null;
  private _scrollCaptureSuppressed = false;
  private _scrollSuppressionPropagationFailed = false;
  private readonly _postScrollStabilizationMs = 600;

  private _pendingTaps: PendingTap[] = [];
  private _tapExpiryTimer: ReturnType<typeof setTimeout> | null = null;
  private _scheduledTapExpiry: number | null = null;
  private _contactOrder = 0;
  private _lomAdoptionCandidate: {
    trace: GestureTrace;
    activation: string;
    capture: NonNullable<PointerDownLomResolution['adoption']>;
    pendingTap?: PendingTap;
    pinchOnly?: boolean;
    accepted?: { ref: string; scanEnd: number };
  } | null = null;
  private _lastReportingTimestamp = 0;
  private _pinchMetrics: PinchMetricsBaseline | null = null;
  private _pinchSession: PinchSessionState | null = null;
  private _pinchTransitionParticipants: readonly GestureTrace[] | null = null;
  private _isActive = false;
  private _touchActivationToken: string | null = null;
  private _lifecycleResumePending = false;
  private _lifecycleCleanupPending = false;
  private _lifecycleOperation: Promise<void> | null = null;
  private _isDisposed = false;

  private _sessionRecorderEmitter = new NativeEventEmitter(
    NativeModules.SessionRecorderLifecycle
  );

  constructor(engine: SessionRecorderEngine) {
    this._engine = engine;
  }

  private _discardLomAdoptionCandidate(): void {
    const candidate = this._lomAdoptionCandidate;
    this._lomAdoptionCandidate = null;
    if (!candidate) return;
    const { trace, activation, capture } = candidate;
    try {
      (globalThis as any).__viewTreeScanner?.withActiveTouch?.(
        activation,
        trace.pointerId,
        capture.surfaceId,
        capture.surfaceInstance,
        capture.captureGeneration,
        false,
        () => {}
      );
    } catch (error) {
      SessionLogger.error('Error retiring a touch LOM adoption', error);
    }
  }

  private _adoptPendingLom(): void {
    const candidate = this._lomAdoptionCandidate;
    const pending = candidate?.pendingTap;
    if (!candidate || !pending) return;
    const index = this._pendingTaps.indexOf(pending);
    if (
      index < 0 ||
      Date.now() > pending.terminalPosition.timestamp + doubleTapTimeoutMs
    ) {
      this._discardLomAdoptionCandidate();
      return;
    }
    const accepted = candidate.accepted;
    if (!accepted) return;
    // PendingTap is only a delivery window for scans completed before physical Up.
    if (accepted.scanEnd >= pending.terminalPosition.timestamp) {
      candidate.accepted = undefined;
      return;
    }
    this._pendingTaps[index] = { ...pending, lomRef: accepted.ref };
    this._discardLomAdoptionCandidate();
  }

  private _freezeLomForPublication(
    trace: GestureTrace,
    terminalTimestamp?: number
  ): void {
    const candidate = this._lomAdoptionCandidate;
    if (!candidate) return;
    if (candidate.pinchOnly) {
      this._discardLomAdoptionCandidate();
      return;
    }
    if (candidate.trace !== trace) return;
    const { accepted, activation, capture } = candidate;
    try {
      if (accepted) {
        if (terminalTimestamp !== undefined) {
          if (accepted.scanEnd < terminalTimestamp) {
            trace.lomRef = accepted.ref;
          }
        } else {
          // A publication during Move needs the original contact still active.
          // Do not infer native activity from the JS trace's presence.
          (globalThis as any).__viewTreeScanner?.withActiveTouch?.(
            activation,
            trace.pointerId,
            capture.surfaceId,
            capture.surfaceInstance,
            capture.captureGeneration,
            false,
            () => {
              trace.lomRef = accepted.ref;
            }
          );
        }
      }
    } catch (error) {
      SessionLogger.error('Error finalizing a touch LOM adoption', error);
    } finally {
      // Close before constructing/storing any record, including failed attempts.
      this._discardLomAdoptionCandidate();
    }
  }

  public adoptMaterializedLom(
    lom: Lom,
    metadata: InternalCaptureMetadata
  ): void {
    const candidate = this._lomAdoptionCandidate;
    if (!candidate || candidate.pinchOnly) return;
    const { trace, activation, capture, pendingTap } = candidate;
    if (
      !this._isActive ||
      activation !== this._touchActivationToken ||
      this._pinchSession ||
      (trace.type !== 'tap' &&
        trace.type !== 'drag' &&
        trace.type !== 'longPress') ||
      trace.isOrphanedPointer ||
      trace.lomResolvedAtDown ||
      (!pendingTap &&
        (this._gestures.size !== 1 ||
          this._gestures.get(trace.pointerId) !== trace)) ||
      (pendingTap &&
        (duration(trace) >= longPressTimeoutMs || distance(trace) >= touchSlop))
    ) {
      this._discardLomAdoptionCandidate();
      return;
    }
    const { scanStart, scanEnd } = metadata;
    if (
      lom.surfaceId !== capture.surfaceId ||
      metadata.surfaceInstance !== capture.surfaceInstance ||
      metadata.captureGeneration !== capture.captureGeneration ||
      !Number.isSafeInteger(metadata.revisionNumber) ||
      metadata.revisionNumber < capture.revisionNumber ||
      metadata.viewportWidth !== getFirst(trace).viewport.width ||
      metadata.viewportHeight !== getFirst(trace).viewport.height ||
      metadata.scanActivation !== activation ||
      typeof scanStart !== 'number' ||
      typeof scanEnd !== 'number' ||
      !Number.isFinite(scanStart) ||
      !Number.isFinite(scanEnd) ||
      scanStart <= getFirst(trace).timestamp ||
      scanEnd < scanStart ||
      !lom.ref
    )
      return;

    // Keep one accepted candidate. Delivery order cannot expand the physical window.
    if (!candidate.accepted || scanEnd < candidate.accepted.scanEnd) {
      candidate.accepted = { ref: lom.ref, scanEnd };
    }
    this._adoptPendingLom();
  }

  /**
   * Initialize the `scroll` and `gestures` listeners
   */
  public init(): void {
    this._isActive = true;
    this._listenPagerNotification();

    if (NativeModules.SessionRecorderLifecycle) {
      this._startTouchListening();
    } else {
      SessionLogger.warning('NativeModule SessionRecorderLifecycle not found');
    }

    // * SCROLL LISTENER
    this._listenScrollNotification();

    // * GESTURE LISTENER
    this._gestureSubscription = this._sessionRecorderEmitter.addListener(
      'SessionRecorderGesture',
      (gesture: any) => {
        if (!this._isActive) return;
        this._runCallback('touch dispatch', () => {
          if (
            this._touchActivationToken === null ||
            gesture?.activationToken !== this._touchActivationToken
          )
            return;
          const { type } = gesture as
            | NativeGesturesEvent
            | NativeTouchOwnershipAbandonedEvent;
          switch (type) {
            case 'ownershipAbandoned':
              this._abandonTouchOwnership(gesture.pointerIds);
              break;
            case 'pointerDown': {
              const pointerId = gesture.actionPointerId;
              this._runPointerCallback('pointer down', pointerId, () =>
                this._onPointerDown(gesture)
              );
              break;
            }
            case 'pointerMove':
              this._onPointerMove(gesture);
              break;
            case 'pointerUp': {
              const pointerId = gesture.actionPointerId;
              this._runPointerCallback('pointer up', pointerId, () =>
                this._onPointerUp(gesture)
              );
              break;
            }
            case 'pointerCancel': {
              const { cancelScope, actionPointerId } = gesture;
              if (cancelScope === 'pointer') {
                if (
                  !Number.isSafeInteger(actionPointerId) ||
                  actionPointerId < 0 ||
                  (!this._gestures.has(actionPointerId) &&
                    !this._pinchSession?.activeTracks.has(actionPointerId))
                )
                  break;
                this._runPointerCallback(
                  'scoped pointer cancel',
                  actionPointerId,
                  () => this._onPointerCancelScoped(gesture)
                );
                break;
              }
              if (cancelScope !== undefined && cancelScope !== 'stream') break;
              try {
                this._runPointerCallback('pointer cancel', undefined, () =>
                  this._onPointerCancel(gesture)
                );
              } finally {
                this._clearGestureState();
              }
              break;
            }
            default:
              break;
          }
        });
      }
    );
  }

  private _startTouchListening(): void {
    this._touchActivationToken = null;
    this._pagerCaptureActive = false;
    this._pagerStateVersion = -1;
    try {
      const token = createUuidV7();
      this._touchActivationToken = token;
      NativeModules.SessionRecorderLifecycle.startListening(token);
    } catch (error) {
      this._touchActivationToken = null;
      throw error;
    }
  }

  private _abandonTouchOwnership(pointerIds: unknown): void {
    if (!Array.isArray(pointerIds)) return;
    const affected = new Set<number>(
      pointerIds.filter(
        (id): id is number =>
          Number.isSafeInteger(id) &&
          id >= 0 &&
          (this._gestures.has(id) ||
            this._pinchSession?.activeTracks.has(id) === true ||
            this._pinchTransitionParticipants?.some(
              (trace) => trace.pointerId === id
            ) === true)
      )
    );
    if (affected.size === 0) return;

    let sharedAbandoned = false;
    const session = this._pinchSession;
    if (session?.tracks.some((track) => affected.has(track.pointerId))) {
      // Consume the shared interaction, without emission or survivor creation.
      this._pinchSession = null;
      sharedAbandoned = true;
      for (const track of session.tracks) affected.add(track.pointerId);
    }
    const participants = this._pinchTransitionParticipants;
    if (participants?.some((trace) => affected.has(trace.pointerId))) {
      this._pinchTransitionParticipants = null;
      sharedAbandoned = true;
      for (const trace of participants) affected.add(trace.pointerId);
    }
    for (const id of affected) this._gestures.delete(id);
    if (
      this._lomAdoptionCandidate &&
      affected.has(this._lomAdoptionCandidate.trace.pointerId)
    )
      this._discardLomAdoptionCandidate();
    if (
      sharedAbandoned ||
      Array.from(affected).some((id) =>
        this._pinchMetrics?.initialPositions?.has(id)
      )
    )
      this._pinchMetrics = null;
    // Completed pending taps and unrelated contacts retain their ownership.
    // No action, exploration, physical Cancel or compensating gesture is built.
  }

  private _runCallback(operation: string, callback: () => void): boolean {
    try {
      callback();
      return true;
    } catch (error) {
      SessionLogger.error(`Gesture collector: ${operation} failed`, error);
      return false;
    }
  }

  private _runPointerCallback(
    operation: string,
    pointerId: number | undefined,
    callback: () => void
  ): void {
    const previousPinch = this._pinchSession;
    if (this._runCallback(operation, callback)) return;

    const abandon = (id: number): void => {
      this._gestures.delete(id);
    };
    // A transition may have mutated traces before installing its session.
    for (const trace of this._pinchTransitionParticipants ?? []) {
      abandon(trace.pointerId);
    }
    for (const session of [previousPinch, this._pinchSession]) {
      if (!session) continue;
      for (const track of session.tracks) abandon(track.pointerId);
    }
    if (pointerId !== undefined) abandon(pointerId);
    this._pinchTransitionParticipants = null;
    this._pinchSession = null;
    this._pinchMetrics = null;
  }

  private _reportingTimestamp(): number {
    // Keep reporting order coherent even if the wall clock moves backwards.
    // Recognition, sampling and timeouts never read this clock.
    this._lastReportingTimestamp = Math.max(
      Date.now(),
      this._lastReportingTimestamp
    );
    return this._lastReportingTimestamp;
  }

  /**
   * Add the first `[PointerTrace]` with their first `[TimedPosition]`.
   *
   * Set `[GesturesType.tap]` type by __default__.
   * @param pointer
   * @param position
   * @param timestamp
   * @param gestureType
   */
  private _addGesture(
    pointer: number,
    position: Offset,
    timestamp: number,
    viewport: Rect,
    gestureType: GesturesType,
    lomRef: string | null,
    lomResolvedAtDown: boolean,
    downOrder: number = -1
  ) {
    const firstPosition: TimedPosition = {
      timestamp,
      reportingTimestamp: this._reportingTimestamp(),
      position: { x: position.x, y: position.y },
      viewport,
    };
    this._gestures.set(pointer, {
      pointerId: pointer,
      downOrder,
      type: gestureType,
      lomRef,
      lomResolvedAtDown,
      positions: [firstPosition],
      latestRaw: firstPosition,
      previousRaw: null,
      sampleIntervalMs: BASE_POSITION_SAMPLE_INTERVAL_MS,
      isOrphanedPointer: false,
    });
  }

  /**
   * Called when a pointer first touches the screen.
   * @param gesture Called when a pointer first touches the screen.
   * @returns
   */
  private _onPointerDown(gesture: NativeGesturesEvent) {
    const { actionPointerId, timestamp, pointers, viewport } = gesture;
    const downOrder = ++this._contactOrder;

    if (!pointers || !Array.isArray(pointers)) {
      SessionLogger.warning(`No pointers in: ${gesture}`);
      return;
    }

    const activePointer = pointers.find((p) => p.pointerId === actionPointerId);
    if (!activePointer) return;

    this._engine.controller.pingActivity();

    const isFirstPointerDown =
      this._gestures.size === 0 && this._pinchSession === null;
    const candidate = this._lomAdoptionCandidate;
    if (
      Platform.OS === 'ios' &&
      !isFirstPointerDown &&
      !this._pinchSession &&
      candidate &&
      candidate.accepted &&
      !candidate.pendingTap &&
      this._gestures.get(candidate.trace.pointerId) === candidate.trace
    ) {
      // Only an already accepted result survives for a single Pinch promotion.
      candidate.pinchOnly = true;
    } else {
      this._discardLomAdoptionCandidate();
    }

    const activeTrace = this._gestures.values().next().value;
    const resolution: PointerDownLomResolution = isFirstPointerDown
      ? this._engine.context.resolveDirtyLomForPointerDown()
      : {
          lomRef: activeTrace
            ? (activeTrace.lomRef ?? '')
            : (this._pinchSession?.lomRef ?? ''),
          resolved: activeTrace?.lomResolvedAtDown ?? false,
          priorityOutcome: 'skipped',
        };

    this._addGesture(
      actionPointerId,
      {
        x: activePointer.x,
        y: activePointer.y,
      },
      timestamp,
      viewport,
      'tap',
      resolution.lomRef,
      resolution.resolved,
      downOrder
    );

    if (
      Platform.OS === 'ios' &&
      isFirstPointerDown &&
      resolution.adoption &&
      !resolution.resolved &&
      this._touchActivationToken
    ) {
      const trace = this._gestures.get(actionPointerId);
      const activation = this._touchActivationToken;
      const capture = resolution.adoption;
      if (trace) {
        try {
          (globalThis as any).__viewTreeScanner?.withActiveTouch?.(
            activation,
            actionPointerId,
            capture.surfaceId,
            capture.surfaceInstance,
            capture.captureGeneration,
            true,
            () => {
              this._lomAdoptionCandidate = { trace, activation, capture };
            }
          );
        } catch (error) {
          this._lomAdoptionCandidate = null;
          SessionLogger.error('Error registering a touch LOM adoption', error);
        }
      }
    }

    if (isFirstPointerDown) {
      if (
        resolution.priorityOutcome === 'attempted' ||
        !this._engine.context.hasPostScrollCapturePending()
      ) {
        this._cancelPostScrollCaptureTimer();
      } else if (resolution.priorityOutcome === 'rejected') {
        this._armPostScrollCaptureTimer(true);
      }
    }

    if (this._pinchSession) {
      const trace = this._gestures.get(actionPointerId)!;
      trace.lomRef = this._pinchSession.lomRef;
      const initialPosition = getLast(trace);
      const track: PinchTrackState = {
        pointerId: actionPointerId,
        entryTimestamp: timestamp,
        entryReportingTimestamp: initialPosition.reportingTimestamp,
        exitTimestamp: null,
        exitReportingTimestamp: null,
        positions: [initialPosition],
        latestRaw: initialPosition,
        previousRaw: null,
        sampleIntervalMs: BASE_POSITION_SAMPLE_INTERVAL_MS,
      };
      this._pinchSession.tracks.push(track);
      this._pinchSession.activeTracks.set(actionPointerId, track);
      trace.type = 'pinch';
      this._pinchMetrics = null;
      return;
    }

    this._updatePinchMetrics();
  }

  /**
   * Called whenever the pointer moves across the screen.
   *
   * Compares the current position and movement delta with the initial data
   * from [onPointerDown] to determine whether the gesture still qualifies
   * as a tap or if it should be treated for another gesture.
   *
   * This is where movement thresholds or gesture cancellation logic
   * (e.g. “no longer a tap”) are typically evaluated.
   *
   * Doing nothing if `_didScroll` is [true].
   * @param gesture
   * @returns
   */
  private _onPointerMove(gesture: NativeGesturesEvent) {
    const { timestamp, pointers, viewport } = gesture;

    if (!pointers || !Array.isArray(pointers)) {
      SessionLogger.warning(`No pointers in: ${gesture}`);
      return;
    }

    for (const pointer of pointers) {
      this._runCallback('pointer move dispatch', () => {
        const pointerId = pointer.pointerId;
        this._runPointerCallback('pointer move', pointerId, () => {
          const gestureTrace = this._gestures.get(pointer.pointerId);

          if (!gestureTrace) {
            return;
          }

          const point: TimedPosition = {
            timestamp,
            reportingTimestamp: this._reportingTimestamp(),
            position: { x: pointer.x, y: pointer.y },
            viewport,
          };

          if (this._pinchSession) {
            const track = this._pinchSession.activeTracks.get(
              pointer.pointerId
            );
            if (
              track &&
              this._pinchSession.activeTracks.has(pointer.pointerId)
            ) {
              _recordPosition(track, point);
            }
            return;
          }

          _recordPosition(gestureTrace, point);
          let currentTrace = gestureTrace;

          if (
            gestureTrace.type !== 'pinch' &&
            distance(gestureTrace) >= touchSlop
          ) {
            const isLongPress: boolean = gestureTrace.type === 'longPress';
            if (
              gestureTrace.type === 'tap' &&
              !gestureTrace.isOrphanedPointer
            ) {
              this._resolveRelatedPendingBeforeNonTap(gestureTrace);
            }
            if (isLongPress && !gestureTrace.isOrphanedPointer) {
              this._emitAction(gestureTrace);
              currentTrace = splitForTransition(gestureTrace, 'drag');
              this._gestures.set(gestureTrace.pointerId, currentTrace);
            } else {
              gestureTrace.type = 'drag';
            }
          } else if (
            gestureTrace.type === 'tap' &&
            !gestureTrace.isOrphanedPointer &&
            distance(gestureTrace) <= touchSlop &&
            duration(gestureTrace) >= longPressTimeoutMs
          ) {
            if (
              gestureTrace.type === 'tap' &&
              !gestureTrace.isOrphanedPointer
            ) {
              this._resolveRelatedPendingBeforeNonTap(gestureTrace);
            }
            gestureTrace.type = 'longPress';
          }

          // * PINCHING
          if (
            this._gestures.size >= 2 &&
            this._pinchMetrics &&
            evaluatePinchGesture(this._gestures, this._pinchMetrics)
          ) {
            this._startPinchSession(
              currentTrace,
              timestamp,
              viewport,
              point.reportingTimestamp
            );
          }
        });
      });
    }
  }

  /**
   * Called when Android cancels the complete active touch stream.
   * @param gesture
   */
  private _onPointerCancel(gesture: NativeGesturesEvent) {
    this._discardLomAdoptionCandidate();
    if (this._gestures.size === 0 && !this._pinchSession) return;

    if (this._pinchSession) {
      const session = this._pinchSession;
      const reportingTimestamp = this._reportingTimestamp();
      for (const track of session.activeTracks.values()) {
        track.exitTimestamp = gesture.timestamp;
        track.exitReportingTimestamp = reportingTimestamp;
        _appendTerminalPosition(track);
        this._compactClosedPinchTracks(session);
      }
      session.activeTracks.clear();
      this._compactClosedPinchTracks(session);
      this._gestures.clear();
      this._finishPinchIfNeeded(gesture.timestamp, false, reportingTimestamp);
      return;
    }

    for (const gestureTrace of this._gestures.values()) {
      this._runPointerCallback(
        'pointer cancel delivery',
        gestureTrace.pointerId,
        () => {
          if (gestureTrace.type === 'tap') {
            this._resolveRelatedPendingBeforeNonTap(gestureTrace);
          }
          this._evaluateGesture(gestureTrace);
        }
      );
    }

    this._clearGestureState();
  }

  private _onPointerCancelScoped(gesture: NativeGesturesEvent): void {
    this._discardLomAdoptionCandidate();
    const { actionPointerId, timestamp } = gesture;
    const trace = this._gestures.get(actionPointerId);
    const session = this._pinchSession;
    const track = session?.activeTracks.get(actionPointerId);
    if (!trace && !track) return;
    if (!Number.isFinite(timestamp)) {
      // The pointer boundary abandons the damaged contact/shared Pinch, not independent taps.
      throw new Error('Invalid physical timestamp for scoped pointer cancel');
    }

    // Consume before any delivery: Context may insert a record and then throw.
    this._gestures.delete(actionPointerId);
    if (session && track) {
      session.activeTracks.delete(actionPointerId);
      const reportingTimestamp = this._reportingTimestamp();
      track.exitTimestamp = timestamp;
      track.exitReportingTimestamp = reportingTimestamp;
      _appendTerminalPosition(track); // Retain the last real sample, not a synthetic Up.
      this._compactClosedPinchTracks(session);
      if (session.activeTracks.size < 2) {
        this._pinchSession = null;
        this._pinchMetrics = null;
        this._finishPinchIfNeeded(timestamp, true, reportingTimestamp, session);
      }
      return;
    }

    try {
      if (trace?.type === 'tap' && !trace.isOrphanedPointer) {
        this._resolveRelatedPendingBeforeNonTap(trace);
      }
      if (trace?.type === 'drag') this._emitExplorations(trace);
      // Incomplete Down/Tap and LongPress are discarded, never finalized as Up.
    } finally {
      if (!this._pinchSession) this._updatePinchMetrics();
    }
  }

  /**
   * Called when the pointer is lifted from the screen.
   *
   * Finalizes the gesture logic based on previous movement analysis.
   * @param gesture
   * @returns
   */
  private _onPointerUp(gesture: NativeGesturesEvent) {
    const { actionPointerId, timestamp, pointers, viewport } = gesture;
    const upOrder = ++this._contactOrder;
    if (this._lomAdoptionCandidate?.pinchOnly)
      this._discardLomAdoptionCandidate();

    const gestureTrace = this._gestures.get(actionPointerId);
    if (!gestureTrace) {
      this._discardLomAdoptionCandidate();
      return;
    }

    const activePointer = pointers?.find(
      (p) => p.pointerId === actionPointerId
    );
    if (this._pinchSession) {
      this._discardLomAdoptionCandidate();
      const pinchTrack = this._pinchSession.activeTracks.get(actionPointerId);
      if (!pinchTrack || !this._pinchSession.activeTracks.has(actionPointerId))
        return;
      const reportingTimestamp = this._reportingTimestamp();
      if (activePointer) {
        _recordPosition(pinchTrack, {
          timestamp,
          reportingTimestamp,
          position: { x: activePointer.x, y: activePointer.y },
          viewport,
        });
      }
      _appendTerminalPosition(pinchTrack);
      pinchTrack.exitTimestamp = timestamp;
      pinchTrack.exitReportingTimestamp = reportingTimestamp;
      this._pinchSession.activeTracks.delete(actionPointerId);
      this._gestures.delete(actionPointerId);
      this._compactClosedPinchTracks(this._pinchSession);
      this._finishPinchIfNeeded(timestamp, true, reportingTimestamp);
      return;
    }

    if (activePointer) {
      const point: TimedPosition = {
        timestamp: timestamp,
        reportingTimestamp: this._reportingTimestamp(),
        position: { x: activePointer.x, y: activePointer.y },
        viewport,
      };
      _recordPosition(gestureTrace, point);
    }

    _appendTerminalPosition(gestureTrace);

    this._gestures.delete(actionPointerId);

    this._updatePinchMetrics();

    if (
      gestureTrace.isOrphanedPointer ||
      !Number.isFinite(timestamp) ||
      !activePointer ||
      gestureTrace.latestRaw.timestamp !== timestamp
    ) {
      this._discardLomAdoptionCandidate();
    } else if (
      gestureTrace.type !== 'tap' ||
      duration(gestureTrace) >= longPressTimeoutMs ||
      distance(gestureTrace) >= touchSlop
    ) {
      // Apply only an already accepted scan ending before this physical Up.
      this._freezeLomForPublication(gestureTrace, timestamp);
    }

    // * ORPHANED POINTER
    if (gestureTrace.isOrphanedPointer) {
      if (gestureTrace.type === 'drag' || distance(gestureTrace) >= touchSlop) {
        this._evaluateDrag(gestureTrace);
      }
      return;
    }

    // Once a Move has recognized drag, terminal geometry cannot demote it.
    if (gestureTrace.type === 'drag') {
      this._evaluateDrag(gestureTrace);
      return;
    }

    // * LONG PRESS
    if (
      duration(gestureTrace) >= longPressTimeoutMs &&
      distance(gestureTrace) < touchSlop
    ) {
      if (gestureTrace.type === 'tap') {
        this._resolveRelatedPendingBeforeNonTap(gestureTrace);
      }
      this._evaluateLongPress(gestureTrace);
      return;
    }

    // * DRAG
    if (distance(gestureTrace) >= touchSlop) {
      if (gestureTrace.type === 'tap') {
        this._resolveRelatedPendingBeforeNonTap(gestureTrace);
      }
      this._evaluateDrag(gestureTrace);
      return;
    }

    // * DOUBLE TAP
    gestureTrace.type = 'tap';
    this._completeTap(gestureTrace, upOrder);
  }

  private _evaluateGesture(gestureTrace: GestureTrace) {
    if (gestureTrace.type === 'drag') {
      this._emitExplorations(gestureTrace);
    } else if (distance(gestureTrace) >= touchSlop) {
      gestureTrace.type = 'drag';
      this._emitExplorations(gestureTrace);
    }
  }

  private _completeTap(gestureTrace: GestureTrace, upOrder: number): void {
    const firstPosition = getFirst(gestureTrace);
    const terminalPosition = gestureTrace.latestRaw;
    const current: PendingTap = {
      firstPosition,
      terminalPosition,
      downOrder: gestureTrace.downOrder,
      upOrder,
      lomRef: gestureTrace.lomRef,
      lomResolvedAtDown: gestureTrace.lomResolvedAtDown,
    };

    const expired = this._takeExpiredPendingTaps(terminalPosition.timestamp);
    let first: PendingTap | null = null;
    try {
      const matchIndex = this._findTapMatch(current);
      first =
        matchIndex === null
          ? null
          : this._pendingTaps.splice(matchIndex, 1)[0]!;
      if (first === null) {
        this._pendingTaps.push(current);
        if (this._lomAdoptionCandidate?.trace === gestureTrace) {
          this._lomAdoptionCandidate.pendingTap = current;
          this._adoptPendingLom();
        }
      }
    } finally {
      try {
        this._rescheduleTapExpiry();
      } finally {
        this._emitPendingTaps(expired);
      }
    }
    if (first !== null) this._emitDoubleTap(first, current);
  }

  private _findTapMatch(second: PendingTap): number | null {
    let bestIndex: number | null = null;
    let bestDistance = Number.POSITIVE_INFINITY;
    let bestUpOrder = -1;
    const secondDownTimestamp = second.firstPosition.timestamp;

    for (let i = 0; i < this._pendingTaps.length; i++) {
      const pending = this._pendingTaps[i]!;
      const elapsed =
        second.terminalPosition.timestamp - pending.terminalPosition.timestamp;
      if (
        second.downOrder <= pending.upOrder ||
        secondDownTimestamp < pending.terminalPosition.timestamp ||
        elapsed < 0 ||
        elapsed > doubleTapTimeoutMs ||
        !pending.lomResolvedAtDown ||
        !second.lomResolvedAtDown ||
        !this._sameViewport(
          second.firstPosition.viewport,
          pending.firstPosition.viewport
        ) ||
        !second.lomRef ||
        !pending.lomRef ||
        second.lomRef !== pending.lomRef
      ) {
        continue;
      }

      const dis = Math.hypot(
        second.terminalPosition.position.x -
          pending.terminalPosition.position.x,
        second.terminalPosition.position.y - pending.terminalPosition.position.y
      );
      if (dis >= doubleTapSlop) continue;

      if (
        dis < bestDistance ||
        (dis === bestDistance && pending.upOrder > bestUpOrder)
      ) {
        bestIndex = i;
        bestDistance = dis;
        bestUpOrder = pending.upOrder;
      }
    }

    return bestIndex;
  }

  private _resolveRelatedPendingBeforeNonTap(trace: GestureTrace): void {
    if (this._pendingTaps.length === 0 || isEmpty(trace)) return;

    const now = getLastTimestamp(trace);
    const expired = this._takeExpiredPendingTaps(now);
    let related: PendingTap | null = null;
    try {
      const firstPosition = getFirst(trace);
      let relatedIndex: number | null = null;
      let bestDistance = Number.POSITIVE_INFINITY;
      let bestUpOrder = -1;

      for (let i = 0; i < this._pendingTaps.length; i++) {
        const pending = this._pendingTaps[i]!;
        const elapsed = now - pending.terminalPosition.timestamp;
        if (
          trace.downOrder <= pending.upOrder ||
          firstPosition.timestamp < pending.terminalPosition.timestamp ||
          elapsed < 0 ||
          elapsed > doubleTapTimeoutMs ||
          !this._sameViewport(
            firstPosition.viewport,
            pending.firstPosition.viewport
          ) ||
          !trace.lomRef ||
          !pending.lomRef ||
          trace.lomRef !== pending.lomRef
        ) {
          continue;
        }

        const dis = Math.hypot(
          firstPosition.position.x - pending.terminalPosition.position.x,
          firstPosition.position.y - pending.terminalPosition.position.y
        );
        if (dis >= doubleTapSlop) continue;

        if (
          dis < bestDistance ||
          (dis === bestDistance && pending.upOrder > bestUpOrder)
        ) {
          relatedIndex = i;
          bestDistance = dis;
          bestUpOrder = pending.upOrder;
        }
      }

      related =
        relatedIndex === null
          ? null
          : this._pendingTaps.splice(relatedIndex, 1)[0]!;
    } finally {
      try {
        if (expired.length > 0 || related !== null) this._rescheduleTapExpiry();
      } finally {
        this._emitPendingTaps(expired);
        if (related !== null) this._emitPendingTap(related);
      }
    }
  }

  private _sameViewport(a: Rect, b: Rect): boolean {
    return (
      a.left === b.left &&
      a.top === b.top &&
      a.width === b.width &&
      a.height === b.height
    );
  }

  private _takeExpiredPendingTaps(now: number): PendingTap[] {
    const expired: PendingTap[] = [];
    // Consume an unreadable pending item without losing independent taps.
    this._pendingTaps = this._pendingTaps.filter((pending) => {
      let keep = false;
      this._runCallback('pending tap expiry selection', () => {
        const isExpired =
          now > pending.terminalPosition.timestamp + doubleTapTimeoutMs;
        if (isExpired) expired.push(pending);
        keep = !isExpired;
      });
      return keep;
    });
    return expired;
  }

  private _rescheduleTapExpiry(): void {
    if (this._pendingTaps.length === 0) {
      this._cancelTapExpiryTimer();
      return;
    }

    let nextExpiry = Number.POSITIVE_INFINITY;
    this._pendingTaps = this._pendingTaps.filter((pending) =>
      this._runCallback('pending tap expiry scheduling', () => {
        nextExpiry = Math.min(
          nextExpiry,
          pending.terminalPosition.timestamp + doubleTapTimeoutMs + 1
        );
      })
    );
    if (this._pendingTaps.length === 0) {
      this._cancelTapExpiryTimer();
      return;
    }
    if (
      this._tapExpiryTimer !== null &&
      this._scheduledTapExpiry === nextExpiry
    ) {
      return;
    }

    this._cancelTapExpiryTimer();
    this._scheduledTapExpiry = nextExpiry;
    this._tapExpiryTimer = setTimeout(
      () => {
        this._runCallback('tap expiry', () => {
          try {
            this._tapExpiryTimer = null;
            this._scheduledTapExpiry = null;
            const expired = this._takeExpiredPendingTaps(Date.now());
            this._emitPendingTaps(expired);
          } finally {
            this._rescheduleTapExpiry();
          }
        });
      },
      Math.max(0, nextExpiry - Date.now())
    );
  }

  private _cancelTapExpiryTimer(): void {
    if (this._tapExpiryTimer !== null) clearTimeout(this._tapExpiryTimer);
    this._tapExpiryTimer = null;
    this._scheduledTapExpiry = null;
  }

  private _emitPendingTaps(taps: PendingTap[]): void {
    for (const tap of taps) this._emitPendingTap(tap);
  }

  private _emitPendingTap(tap: PendingTap): void {
    if (this._lomAdoptionCandidate?.pendingTap === tap)
      this._discardLomAdoptionCandidate();
    this._runCallback('pending tap delivery', () => {
      if (tap.lomRef === null) return;
      this._engine.context.recordAction(
        createTapEvent({
          timestamp: tap.firstPosition.reportingTimestamp,
          viewport: tap.firstPosition.viewport,
          position: tap.firstPosition.position,
          lomRef: tap.lomRef,
        })
      );
    });
  }

  private _emitDoubleTap(first: PendingTap, second: PendingTap): void {
    this._runCallback('double tap delivery', () => {
      if (!first.lomRef || first.lomRef !== second.lomRef) return;
      this._engine.context.recordAction(
        createDoubleTapEvent({
          timestamp: first.firstPosition.reportingTimestamp,
          secondTimestamp: second.firstPosition.reportingTimestamp,
          viewport: first.firstPosition.viewport,
          positions: [
            first.firstPosition.position,
            second.firstPosition.position,
          ],
          lomRef: first.lomRef,
        })
      );
    });
  }

  private _evaluateDrag(gestureTrace: GestureTrace) {
    this._freezeLomForPublication(gestureTrace);
    gestureTrace.type = 'drag';
    const explorations = _createExplorationEvent(gestureTrace);
    for (const exploration of explorations) {
      this._engine.context.recordExploration(exploration);
    }
  }

  private _evaluateLongPress(gestureTrace: GestureTrace) {
    gestureTrace.type = 'longPress';
    this._emitAction(gestureTrace);
  }

  private _promotePinchLom(
    participants: readonly GestureTrace[],
    viewport: Rect
  ): void {
    const candidate = this._lomAdoptionCandidate;
    try {
      if (
        Platform.OS !== 'ios' ||
        !this._isActive ||
        !candidate?.pinchOnly ||
        candidate.pendingTap ||
        candidate.activation !== this._touchActivationToken ||
        !participants.includes(candidate.trace)
      )
        return;
      const accepted = candidate.accepted;
      if (
        !accepted ||
        this._engine.context.currentLomRef() !== accepted.ref ||
        participants.some((trace) => {
          const initial = getFirst(trace).viewport;
          return (
            trace.isOrphanedPointer ||
            initial.left !== viewport.left ||
            initial.top !== viewport.top ||
            initial.width !== viewport.width ||
            initial.height !== viewport.height
          );
        })
      )
        return;
      // All local checks precede this synchronous update and any publication.
      for (const trace of participants) trace.lomRef = accepted.ref;
    } finally {
      this._discardLomAdoptionCandidate();
    }
  }

  private _startPinchSession(
    qualifyingTrace: GestureTrace,
    timestamp: number,
    viewport: Rect,
    reportingTimestamp: number
  ): void {
    if (this._pinchSession) return;

    const participants = Array.from(this._gestures.values()).sort(
      (a, b) =>
        getFirst(a).timestamp - getFirst(b).timestamp ||
        a.pointerId - b.pointerId
    );
    if (participants.length < 2) {
      this._discardLomAdoptionCandidate();
      return;
    }

    this._promotePinchLom(participants, viewport);

    const oldest = participants[0]!;
    const candidateLomRef = oldest.lomRef;
    const lomRef = participants.every(
      (trace) => trace.lomRef === candidateLomRef
    )
      ? candidateLomRef
      : null;

    this._pinchTransitionParticipants = participants;
    for (const trace of participants) {
      if (trace.type === 'tap' && !trace.isOrphanedPointer) {
        this._resolveRelatedPendingBeforeNonTap(trace);
      }
    }

    const tracks: PinchTrackState[] = [];
    const activeTracks = new Map<number, PinchTrackState>();
    for (const trace of participants) {
      if (trace.type === 'drag') {
        if (trace === qualifyingTrace) {
          const previousRaw = trace.previousRaw;
          const first = getFirst(trace).position;
          if (trace.positions[trace.positions.length - 1] === trace.latestRaw) {
            trace.positions.pop();
          }
          if (
            previousRaw &&
            Math.hypot(
              previousRaw.position.x - first.x,
              previousRaw.position.y - first.y
            ) >= touchSlop
          ) {
            this._emitExplorations(trace, previousRaw);
          }
        } else {
          this._emitExplorations(trace);
        }
      }

      const initialPosition = getLast(trace);
      trace.lomRef = lomRef;
      this._gestures.set(trace.pointerId, splitForTransition(trace, 'pinch'));
      const track: PinchTrackState = {
        pointerId: trace.pointerId,
        entryTimestamp: timestamp,
        entryReportingTimestamp: reportingTimestamp,
        exitTimestamp: null,
        exitReportingTimestamp: null,
        positions: [initialPosition],
        latestRaw: initialPosition,
        previousRaw: null,
        sampleIntervalMs: BASE_POSITION_SAMPLE_INTERVAL_MS,
      };
      tracks.push(track);
      activeTracks.set(trace.pointerId, track);
    }

    this._pinchSession = {
      startTimestamp: timestamp,
      startReportingTimestamp: reportingTimestamp,
      viewport,
      lomRef,
      tracks,
      initialTracks: new Set(tracks),
      activeTracks,
    };
    this._pinchTransitionParticipants = null;
    this._pinchMetrics = null;
  }

  private _finishPinchIfNeeded(
    endTimestamp: number,
    preserveSurvivor: boolean,
    endReportingTimestamp: number,
    session: PinchSessionState | null = this._pinchSession
  ): void {
    if (!session || session.activeTracks.size >= 2) return;

    const survivorId =
      preserveSurvivor && session.activeTracks.size === 1
        ? session.activeTracks.keys().next().value
        : undefined;
    const survivorTrack =
      survivorId === undefined
        ? undefined
        : session.activeTracks.get(survivorId);
    for (const track of session.activeTracks.values()) {
      if (track.exitTimestamp === null) {
        track.exitTimestamp = endTimestamp;
        track.exitReportingTimestamp = endReportingTimestamp;
      }
      _appendTerminalPosition(track);
      this._compactClosedPinchTracks(session);
    }
    session.activeTracks.clear();
    this._compactClosedPinchTracks(session);

    if (session.lomRef !== null) {
      const tracks: PinchTrack[] = session.tracks.map((track) => ({
        pointerId: track.pointerId,
        entryDelta:
          track.entryReportingTimestamp - session.startReportingTimestamp,
        exitDelta:
          (track.exitReportingTimestamp ?? endReportingTimestamp) -
          session.startReportingTimestamp,
        positions: track.positions.map((position) => position.position),
      }));
      this._engine.context.recordExploration(
        createPinchEvent({
          timestamp: session.startReportingTimestamp,
          viewport: session.viewport,
          endTimestamp: endReportingTimestamp,
          tracks,
          lomRef: session.lomRef,
        })
      );
    }

    this._pinchSession = null;
    if (survivorId !== undefined) {
      const survivor = this._gestures.get(survivorId);
      if (survivor && survivorTrack) {
        const anchor = {
          ...survivorTrack.latestRaw,
          reportingTimestamp: endReportingTimestamp,
        };
        this._gestures.set(survivorId, {
          ...survivor,
          type: 'tap',
          isOrphanedPointer: true,
          positions: [anchor],
          latestRaw: anchor,
          previousRaw: null,
          sampleIntervalMs: BASE_POSITION_SAMPLE_INTERVAL_MS,
        });
      }
    }
    this._updatePinchMetrics();
  }

  private _finishPinchForLifecycle(
    endTimestamp: number,
    reservedReportingTimestamp?: number
  ): void {
    const session = this._pinchSession;
    if (!session) return;
    // Consume ownership before finalization/delivery: Context may store the
    // event and then throw, so this session must never be replayed.
    this._pinchSession = null;
    this._pinchMetrics = null;
    for (const track of session.tracks) {
      if (this._gestures.get(track.pointerId)?.type === 'pinch') {
        this._gestures.delete(track.pointerId);
      }
    }
    const endReportingTimestamp =
      reservedReportingTimestamp ?? this._reportingTimestamp();
    for (const track of session.activeTracks.values()) {
      if (track.exitTimestamp === null) {
        track.exitTimestamp = endTimestamp;
        track.exitReportingTimestamp = endReportingTimestamp;
      }
      _appendTerminalPosition(track);
      this._compactClosedPinchTracks(session);
    }
    session.activeTracks.clear();
    this._compactClosedPinchTracks(session);
    this._finishPinchIfNeeded(
      endTimestamp,
      false,
      endReportingTimestamp,
      session
    );
  }

  private _compactClosedPinchTracks(session: PinchSessionState): void {
    const closedTracks = session.tracks.filter(
      (track) => track.exitTimestamp !== null
    );
    if (closedTracks.length <= MAX_RETAINED_CLOSED_PINCH_TRACKS) return;

    const initialTracks = closedTracks.filter((track) =>
      session.initialTracks.has(track)
    );
    const nonInitialTracks = closedTracks.filter(
      (track) => !session.initialTracks.has(track)
    );
    const recentCount = Math.min(
      32,
      MAX_RETAINED_CLOSED_PINCH_TRACKS - initialTracks.length
    );
    const trackOrder = new Map(
      session.tracks.map((track, index) => [track, index] as const)
    );
    const recentTracks = new Set(
      [...nonInitialTracks]
        .sort(
          (a, b) =>
            b.exitTimestamp! - a.exitTimestamp! ||
            trackOrder.get(b)! - trackOrder.get(a)!
        )
        .slice(0, recentCount)
    );
    const olderTracks = nonInitialTracks.filter(
      (track) => !recentTracks.has(track)
    );
    const olderBudget = Math.max(
      0,
      MAX_RETAINED_CLOSED_PINCH_TRACKS -
        initialTracks.length -
        recentTracks.size
    );
    const stride = Math.max(
      1,
      olderBudget === 0
        ? olderTracks.length + 1
        : Math.ceil(olderTracks.length / olderBudget)
    );
    const retainedTracks = new Set<PinchTrackState>([
      ...initialTracks,
      ...recentTracks,
      ...(olderBudget === 0
        ? []
        : olderTracks.filter((_, index) => index % stride === 0)),
      ...Array.from(session.activeTracks.values()).filter(
        (track) => track.exitTimestamp === null
      ),
    ]);

    const compacted = session.tracks.filter((track) =>
      retainedTracks.has(track)
    );
    session.tracks.splice(0, session.tracks.length, ...compacted);
  }

  private _updatePinchMetrics(): void {
    if (this._gestures.size >= 2) {
      const initialPositions = new Map<number, Offset>();
      for (const [pointerId, gestureTrace] of this._gestures.entries()) {
        initialPositions.set(pointerId, getLastPosition(gestureTrace));
      }

      this._pinchMetrics = {
        initialPositions: initialPositions,
        centroid: getCentroid(this._gestures),
        avgDistance: getAverageDistance(this._gestures),
      };
    } else {
      this._pinchMetrics = null;
    }
  }

  private _clearGestureState(): void {
    this._discardLomAdoptionCandidate();
    this._gestures.clear();
    this._pinchMetrics = null;
    this._pinchSession = null;
    this._pinchTransitionParticipants = null;
  }

  private _drainPendingTaps(): void {
    this._cancelTapExpiryTimer();
    const pending = this._pendingTaps.splice(0);
    this._emitPendingTaps(pending);
  }

  private _drainRecognizedGestures(): void {
    for (const gestureTrace of this._gestures.values()) {
      // Pinch owns its participant traces and is drained separately.
      if (gestureTrace.type !== 'drag' && gestureTrace.type !== 'longPress') {
        continue;
      }
      this._gestures.delete(gestureTrace.pointerId);
      this._runCallback('lifecycle gesture delivery', () => {
        if (isEmpty(gestureTrace)) return;
        if (gestureTrace.type === 'drag') {
          this._emitExplorations(gestureTrace);
        } else if (!gestureTrace.isOrphanedPointer) {
          this._emitAction(gestureTrace);
        }
      });
    }
  }

  private _emitAction(gestureTrace: GestureTrace): void {
    this._freezeLomForPublication(gestureTrace);
    const action = _createActionEvent(gestureTrace);
    if (!action) return;
    this._engine.context.recordAction(action);
  }

  private _emitExplorations(
    gestureTrace: GestureTrace,
    terminal: TimedPosition = gestureTrace.latestRaw
  ): void {
    this._freezeLomForPublication(gestureTrace);
    if (gestureTrace.type === 'drag') {
      _appendTerminalPosition(gestureTrace, terminal);
    }
    const explorations: ExplorationEvent[] =
      _createExplorationEvent(gestureTrace);
    if (explorations.length !== 0) {
      for (const exploration of explorations) {
        this._engine.context.recordExploration(exploration);
      }
    }
  }

  private _nativeEventListener<T extends object>(
    handler: (event: T) => void,
    operation?: string
  ): (...args: readonly Object[]) => void {
    return (...args: readonly Object[]): void => {
      const event = args[0] as unknown as T | undefined;
      if (!event) return;

      if (operation) {
        this._runCallback(operation, () => handler(event));
      } else {
        handler(event);
      }
    };
  }

  /**
   * Calling both listeners from Native Emitter
   */
  private _scrollEventListener<T extends { activationToken?: string }>(
    handler: (event: T) => void,
    operation?: string
  ): (...args: readonly Object[]) => void {
    return this._nativeEventListener<T>((event) => {
      // Only the explicitly scoped bridge is gated; Android legacy is unchanged.
      if (
        event.activationToken !== undefined &&
        (this._touchActivationToken === null ||
          event.activationToken !== this._touchActivationToken)
      )
        return;
      handler(event);
    }, operation);
  }

  private _listenPagerNotification(): void {
    this._pagerSubscription = this._sessionRecorderEmitter.addListener(
      'SessionRecorderPagerState',
      this._scrollEventListener<{
        activationToken: string;
        version: number;
        active: boolean;
        capture: boolean;
      }>((event) => {
        if (
          !this._isActive ||
          !Number.isSafeInteger(event.version) ||
          event.version <= this._pagerStateVersion ||
          typeof event.active !== 'boolean'
        )
          return;
        this._pagerStateVersion = event.version;
        const wasActive = this._pagerCaptureActive;
        this._pagerCaptureActive = event.active;
        // One debt for an activity interval; a new idle selection still renews it.
        if (!wasActive && (event.active || event.capture)) {
          this._cancelPostScrollCaptureTimer();
          this._engine.context.markPostScrollCapturePending();
        }
        if (event.active) this._setScrollCaptureSuppressed(true, false);
        else if (wasActive || event.capture) this._settleScrollCaptureIfIdle();
      }, 'pager capture state')
    );
  }

  private _listenScrollNotification() {
    this._scrollSubscriptions = [
      this._sessionRecorderEmitter.addListener(
        'SessionRecorderScrollStart',
        this._scrollEventListener<NativeScrollSignal>(
          (event) => this._onScrollStart(event),
          'scroll start'
        )
      ),
      this._sessionRecorderEmitter.addListener(
        'SessionRecorderScrollValidated',
        this._scrollEventListener<NativeScrollSignal>((event) =>
          this._onScrollValidated(event)
        )
      ),
      this._sessionRecorderEmitter.addListener(
        'SessionRecorderScrollEnd',
        this._scrollEventListener<NativeScrollSignal>(
          (event) => this._onScrollEnd(event),
          'scroll end'
        )
      ),
      this._sessionRecorderEmitter.addListener(
        'SessionRecorderScrollCancel',
        this._scrollEventListener<NativeScrollSignal>(
          (event) => this._onScrollCancel(event),
          'scroll cancel'
        )
      ),
    ];

    this._scrollRemoveSubscription = this._sessionRecorderEmitter.addListener(
      'SessionRecorderScrollRemoved',
      this._scrollEventListener<NativeScrollRemovedSignal>((event) => {
        const viewId = event.viewId;
        const token = this._scrollTokenByView.get(viewId);
        if (token !== undefined) {
          this._removePendingScroll(token, viewId);
        }
      }, 'scroll removed')
    );
  }

  private _onScrollStart(event: NativeScrollSignal): void {
    if (!this._isActive) return;

    this._cancelPostScrollCaptureTimer();

    const viewId = event.viewId;
    const token = event.token;
    const previousToken = this._scrollTokenByView.get(viewId);
    const replacing = previousToken !== undefined && previousToken !== token;
    if (previousToken !== undefined && previousToken !== token) {
      this._removePendingScroll(previousToken, viewId, false);
    }

    try {
      const viewport = this._scrollRect(event);
      this._pendingScrolls.set(token, {
        activationToken: event.activationToken,
        token,
        viewId,
        timestamp: event.timestamp,
        viewport,
        offset: this._scrollOffset(event),
        lomRef: this._engine.context.currentLomRef() ?? '',
        lastKnownViewport: viewport,
        lastKnownOffset: this._scrollOffset(event),
        validated: false,
      });
      this._scrollTokenByView.set(viewId, token);
      this._setScrollCaptureSuppressed(true, false);
    } finally {
      if (replacing && this._pendingScrolls.size === 0) {
        this._settleScrollCaptureIfIdle();
      }
    }
  }

  private _onScrollValidated(event: NativeScrollSignal): void {
    if (!this._isActive) return;

    const pending = this._matchingPendingScroll(event);
    if (!pending || pending.validated) return;

    pending.validated = true;
    pending.lastKnownViewport = this._scrollRect(event);
    pending.lastKnownOffset = this._scrollOffset(event);
    this._engine.context.markPostScrollCapturePending();
    this._engine.controller.pingActivity();
    this._engine.context.recordExploration(
      createScrollEvent({
        timestamp: pending.timestamp,
        viewport: pending.viewport,
        offset: pending.offset,
        lomRef: pending.lomRef,
        phase: 'start',
      })
    );
  }

  private _onScrollEnd(event: NativeScrollSignal): void {
    const pending = this._matchingPendingScroll(event);
    if (!pending) return;
    const { token, viewId } = pending;

    try {
      if (pending.validated) {
        this._engine.context.recordExploration(
          createScrollEvent({
            timestamp: event.timestamp,
            viewport: this._scrollRect(event),
            offset: this._scrollOffset(event),
            lomRef: pending.lomRef,
            phase: 'end',
          })
        );
      }
    } finally {
      this._removePendingScroll(token, viewId);
    }
  }

  private _onScrollCancel(event: NativeScrollSignal): void {
    const pending = this._matchingPendingScroll(event);
    if (!pending) return;
    this._removePendingScroll(pending.token, pending.viewId);
  }

  private _matchingPendingScroll(
    event: NativeScrollSignal
  ): PendingScrollSession | null {
    const pending = this._pendingScrolls.get(event.token);
    if (!pending || pending.viewId !== event.viewId) return null;
    if (this._scrollTokenByView.get(event.viewId) !== event.token) return null;
    return pending;
  }

  private _removePendingScroll(
    token: number,
    viewId: number,
    settleCapture: boolean = true
  ): void {
    this._pendingScrolls.delete(token);
    if (this._scrollTokenByView.get(viewId) === token) {
      this._scrollTokenByView.delete(viewId);
    }
    if (settleCapture) {
      this._runCallback('scroll settlement', () =>
        this._settleScrollCaptureIfIdle()
      );
    }
  }

  private _clearScrollState(): void {
    this._pendingScrolls.clear();
    this._scrollTokenByView.clear();
    this._pagerCaptureActive = false;
    this._pagerStateVersion = -1;
  }

  private _setScrollCaptureSuppressed(
    suppressed: boolean,
    restorePendingAutomaticCapture: boolean
  ): void {
    suppressed = suppressed || this._pagerCaptureActive;
    if (
      this._scrollCaptureSuppressed === suppressed &&
      !this._scrollSuppressionPropagationFailed
    ) {
      return;
    }
    const applied = this._runCallback('scroll suppression', () =>
      this._engine.context.setScrollCaptureSuppressed(
        suppressed,
        restorePendingAutomaticCapture
      )
    );
    this._scrollSuppressionPropagationFailed = !applied;
    if (applied) this._scrollCaptureSuppressed = suppressed;
  }

  private _settleScrollCaptureIfIdle(): void {
    if (this._pendingScrolls.size > 0 || this._pagerCaptureActive) {
      this._setScrollCaptureSuppressed(true, false);
      return;
    }

    let hasDebt: boolean | null = null;
    try {
      hasDebt = this._engine.context.hasPostScrollCapturePending();
    } catch (error) {
      SessionLogger.error('Error reading post-scroll capture debt', error);
    }
    // Unknown debt must not restore an automatic capture as if debt were absent.
    this._setScrollCaptureSuppressed(false, hasDebt === false);
    if (this._isActive && hasDebt === true) {
      this._runCallback('post-scroll stabilization', () =>
        this._armPostScrollCaptureTimer()
      );
    }
  }

  private _armPostScrollCaptureTimer(isRetry: boolean = false): void {
    const generation =
      this._engine.context.currentPostScrollCaptureGeneration();
    if (
      this._postScrollCaptureTimer !== null ||
      !this._isActive ||
      this._pendingScrolls.size > 0 ||
      this._pagerCaptureActive ||
      generation === 0 ||
      (isRetry && this._postScrollRetryUsedGeneration === generation)
    )
      return;

    this._postScrollCaptureTimer = setTimeout(() => {
      this._postScrollCaptureTimer = null;
      if (
        !this._isActive ||
        this._pendingScrolls.size > 0 ||
        this._pagerCaptureActive
      )
        return;

      const currentGeneration =
        this._engine.context.currentPostScrollCaptureGeneration();
      if (currentGeneration === 0) return;
      if (
        this._postScrollRetryUsedGeneration !== null &&
        this._postScrollRetryUsedGeneration !== currentGeneration
      ) {
        this._postScrollRetryUsedGeneration = null;
      }
      if (isRetry) this._postScrollRetryUsedGeneration = currentGeneration;

      const accepted = this._engine.context.requestPostScrollCapture();
      if (
        !accepted &&
        this._isActive &&
        this._pendingScrolls.size === 0 &&
        this._engine.context.currentPostScrollCaptureGeneration() ===
          currentGeneration &&
        this._postScrollRetryUsedGeneration !== currentGeneration
      ) {
        this._armPostScrollCaptureTimer(true);
      }
    }, this._postScrollStabilizationMs);
  }

  private _cancelPostScrollCaptureTimer(): void {
    if (this._postScrollCaptureTimer === null) return;
    clearTimeout(this._postScrollCaptureTimer);
    this._postScrollCaptureTimer = null;
  }

  private _scrollRect(scroll: NativeScrollBounds): Rect {
    return {
      left: scroll.left,
      top: scroll.top,
      width: scroll.width,
      height: scroll.height,
    };
  }

  private _scrollOffset(scroll: NativeScrollBounds): Offset {
    return { x: scroll.scrollX, y: scroll.scrollY };
  }

  public stop(): void | Promise<void> {
    this._runCallback('lifecycle stop activity', () =>
      this._engine.controller.stopReporting(true)
    );
    const pending = this.pause();
    return this._completeLifecycle(pending, () => {
      this._postScrollRetryUsedGeneration = null;
      this._setScrollCaptureSuppressed(false, false);
      this._engine.context.clearPostScrollCapturePending();
      this._pagerSubscription?.remove();
      this._pagerSubscription = null;
      this._gestureSubscription?.remove();
      this._gestureSubscription = null;
      for (const subscription of this._scrollSubscriptions)
        subscription.remove();
      this._scrollSubscriptions = [];
      this._scrollRemoveSubscription?.remove();
      this._scrollRemoveSubscription = null;
      this._clearGestureState();
      this._clearScrollState();
      SessionLogger.info('SessionRecorder Subscriptions removed.');
    });
  }

  public dispose(): void | Promise<void> {
    if (this._isDisposed) return this._lifecycleOperation ?? undefined;
    this._isDisposed = true;
    this._touchActivationToken = null;
    this._isActive = false;
    const timestamp = Date.now();
    const pinchReportingTimestamp = this._pinchSession
      ? this._reportingTimestamp()
      : undefined;

    const cleanup = (action: () => void): void => {
      try {
        action();
      } catch (error) {
        try {
          SessionLogger.error('Error cleaning up gesture collector', error);
        } catch {}
      }
    };

    cleanup(() => this._cancelPostScrollCaptureTimer());
    cleanup(() => this._cancelTapExpiryTimer());
    let pending: void | Promise<void> = this._lifecycleOperation ?? undefined;
    if (!pending) {
      cleanup(() => {
        pending = this._drainScrollSessionsForLifecycle(timestamp);
      });
    }
    return this._completeLifecycle(pending, () => {
      cleanup(() =>
        this._finishPinchForLifecycle(timestamp, pinchReportingTimestamp)
      );
      cleanup(() => this._drainRecognizedGestures());
      cleanup(() => this._drainPendingTaps());
      cleanup(() => this._cancelPostScrollCaptureTimer());
      cleanup(() => this._cancelTapExpiryTimer());
      this._scheduledTapExpiry = null;
      this._clearGestureState();
      this._pendingTaps = [];
      cleanup(() => NativeModules.SessionRecorderLifecycle?.stopListening());
      this._postScrollRetryUsedGeneration = null;
      cleanup(() => this._setScrollCaptureSuppressed(false, false));
      cleanup(() => this._engine.context.clearPostScrollCapturePending());
      cleanup(() => this._pagerSubscription?.remove());
      this._pagerSubscription = null;
      cleanup(() => this._gestureSubscription?.remove());
      this._gestureSubscription = null;
      for (const subscription of this._scrollSubscriptions) {
        cleanup(() => subscription.remove());
      }
      this._scrollSubscriptions = [];
      cleanup(() => this._scrollRemoveSubscription?.remove());
      this._scrollRemoveSubscription = null;
      this._clearScrollState();
      this._pinchMetrics = null;
      this._pinchSession = null;
    });
  }

  public pause(): void | Promise<void> {
    this._touchActivationToken = null;
    this._isActive = false;
    if (this._lifecycleOperation || this._isDisposed) {
      return this._lifecycleOperation ?? undefined;
    }
    const timestamp = Date.now();
    const pinchReportingTimestamp = this._pinchSession
      ? this._reportingTimestamp()
      : undefined;
    // Resolve JS-owned scroll records before native unregister can strand a
    // terminal event in the bridge queue. Late terminals no longer match once
    // this drain removes their tokens.
    const attemptDrains = !this._lifecycleCleanupPending;
    this._lifecycleResumePending = true;
    this._lifecycleCleanupPending = true;
    this._runCallback('lifecycle post-scroll timer cancellation', () =>
      this._cancelPostScrollCaptureTimer()
    );
    this._runCallback('lifecycle tap timer cancellation', () =>
      this._cancelTapExpiryTimer()
    );
    // eslint-disable-next-line no-undef-init
    let pending: void | Promise<void> = undefined;
    if (attemptDrains) {
      this._runCallback('lifecycle scroll drain', () => {
        pending = this._drainScrollSessionsForLifecycle(timestamp);
      });
    }
    return this._completeLifecycle(pending, () => {
      if (attemptDrains) {
        this._runCallback('lifecycle pinch drain', () =>
          this._finishPinchForLifecycle(timestamp, pinchReportingTimestamp)
        );
        this._runCallback('lifecycle gesture drain', () =>
          this._drainRecognizedGestures()
        );
        this._runCallback('lifecycle tap drain', () =>
          this._drainPendingTaps()
        );
      }
      this._cleanupLifecycleState();
      this._isActive = false;
      this._runCallback('lifecycle stop listening', () =>
        NativeModules.SessionRecorderLifecycle.stopListening()
      );
    });
  }

  private _completeLifecycle(
    pending: void | Promise<void>,
    finish: () => void
  ): void | Promise<void> {
    if (!pending) {
      finish();
      return;
    }
    const completion = Promise.resolve(pending)
      .then(finish, (error) => {
        SessionLogger.error('Error awaiting lifecycle drain', error);
        finish();
      })
      .catch((error) => {
        SessionLogger.error('Error completing lifecycle cleanup', error);
      })
      .then(() => {
        if (this._lifecycleOperation === completion) {
          this._lifecycleOperation = null;
        }
      });
    this._lifecycleOperation = completion;
    return completion;
  }

  private _cleanupLifecycleState(): void {
    // Drain attempts are never replayed: an event may already have been stored
    // before its delivery failed. Cleanup consumes the remaining local state.
    const scrollTimer = this._runCallback(
      'lifecycle post-scroll timer cleanup',
      () => this._cancelPostScrollCaptureTimer()
    );
    const tapTimer = this._runCallback('lifecycle tap timer cleanup', () =>
      this._cancelTapExpiryTimer()
    );
    const gestures = this._runCallback('lifecycle gesture cleanup', () =>
      this._clearGestureState()
    );
    this._pendingTaps = [];
    const scrolls = this._runCallback('lifecycle scroll cleanup', () =>
      this._clearScrollState()
    );
    const suppression = this._runCallback('lifecycle suppression cleanup', () =>
      this._setScrollCaptureSuppressed(false, false)
    );
    this._lifecycleCleanupPending = !(
      scrollTimer &&
      tapTimer &&
      gestures &&
      scrolls &&
      suppression &&
      !this._scrollSuppressionPropagationFailed
    );
  }

  private _drainScrollSessionsForLifecycle(
    timestamp: number
  ): void | Promise<void> {
    const reserved: PendingScrollSession[] = [];
    for (const pending of Array.from(this._pendingScrolls.values())) {
      this._runCallback('lifecycle scroll delivery', () => {
        if (this._pendingScrolls.get(pending.token) !== pending) return;
        const ownsView =
          this._scrollTokenByView.get(pending.viewId) === pending.token;
        // Consume both indices before delivery, without normal settlement:
        // teardown must not restore captures or arm a stabilization timer.
        this._removePendingScroll(pending.token, pending.viewId, false);
        if (!ownsView || !pending.validated) return;

        reserved.push({
          ...pending,
          lastKnownViewport: { ...pending.lastKnownViewport },
          lastKnownOffset: { ...pending.lastKnownOffset },
        });
      });
    }

    if (reserved.length === 0) {
      this._setScrollCaptureSuppressed(this._pendingScrolls.size > 0, false);
      return;
    }
    return this._snapshotScrollSessions(reserved).then((results) => {
      for (const pending of reserved) {
        let offset = pending.lastKnownOffset;
        let viewport = pending.lastKnownViewport;
        this._runCallback('lifecycle scroll snapshot metadata', () => {
          if (!Array.isArray(results)) return;
          const snapshot = results.find(
            (item) =>
              item?.viewId === pending.viewId && item?.token === pending.token
          );
          if (!snapshot) return;
          if (
            Number.isFinite(snapshot.offsetX) &&
            Number.isFinite(snapshot.offsetY)
          ) {
            offset = { x: snapshot.offsetX, y: snapshot.offsetY };
          }
          const rect = snapshot.viewport;
          if (
            rect &&
            Number.isFinite(rect.left) &&
            Number.isFinite(rect.top) &&
            Number.isFinite(rect.width) &&
            Number.isFinite(rect.height) &&
            rect.width > 0 &&
            rect.height > 0
          ) {
            viewport = {
              left: rect.left,
              top: rect.top,
              width: rect.width,
              height: rect.height,
            };
          }
        });
        this._runCallback('lifecycle scroll delivery', () => {
          this._engine.context.recordExploration(
            createScrollEvent({
              timestamp,
              viewport,
              offset,
              lomRef: pending.lomRef,
              phase: 'end',
            })
          );
        });
      }
      this._setScrollCaptureSuppressed(this._pendingScrolls.size > 0, false);
    });
  }

  private _snapshotScrollSessions(
    sessions: readonly PendingScrollSession[]
  ): Promise<unknown> {
    return new Promise((resolve) => {
      let completed = false;
      const finish = (results: unknown): void => {
        if (completed) return;
        completed = true;
        clearTimeout(timer);
        resolve(results);
      };
      const timer = setTimeout(() => finish([]), SCROLL_SNAPSHOT_TIMEOUT_MS);
      try {
        const native = NativeModules.SessionRecorderLifecycle;
        if (typeof native?.snapshotScrollSessions !== 'function') {
          finish([]);
          return;
        }
        const requests = sessions.map(({ viewId, token, activationToken }) => ({
          viewId,
          token,
          ...(activationToken !== undefined ? { activationToken } : {}),
        }));
        Promise.resolve(native.snapshotScrollSessions(requests)).then(
          finish,
          () => finish([])
        );
      } catch {
        finish([]);
      }
    });
  }

  public resume() {
    if (this._isDisposed || this._lifecycleOperation) return;
    if (this._isActive && !this._lifecycleResumePending) return;
    this._lifecycleResumePending = true;
    if (this._lifecycleCleanupPending) {
      this._cleanupLifecycleState();
      if (this._lifecycleCleanupPending) return;
    }
    if (!this._isActive) {
      const requested = this._runCallback('lifecycle start listening', () =>
        this._startTouchListening()
      );
      if (!requested) return;
      // This gate permits JS collection after a successful bridge invocation;
      // it does not acknowledge installation on Android's UI thread.
      this._isActive = true;
    }
    this._lifecycleResumePending = !this._runCallback(
      'lifecycle post-scroll rearm',
      () => this._armPostScrollCaptureTimer()
    );
  }
}
