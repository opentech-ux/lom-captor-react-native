import type { Offset, Rect } from '../types/geometry';
import { getLast, isEmpty } from '../utils/GesturesUtils';

export type NativeGesturesEventType =
  | 'pointerDown'
  | 'pointerMove'
  | 'pointerUp'
  | 'pointerCancel';

export type GesturesType =
  | 'none'
  | 'tap'
  | 'doubleTap'
  | 'longPress'
  | 'drag'
  | 'pinch'
  | 'scroll'
  | 'scrollStart'
  | 'scrollEnd';

interface NativePointer {
  pointerId: number;
  x: number;
  y: number;
  isActive: boolean;
}

export interface NativeScrollBounds {
  viewId: number;
  left: number;
  top: number;
  width: number;
  height: number;
  scrollX: number;
  scrollY: number;
}

export interface NativeScrollSignal extends NativeScrollBounds {
  /** iOS listening activation; absent on the unchanged Android legacy bridge. */
  activationToken?: string;
  token: number;
  timestamp: number;
}

export interface NativeScrollRemovedSignal {
  activationToken?: string;
  viewId: number;
}

export interface NativeGesturesEvent {
  /** Listening activation identity, private to the native touch bridge. */
  activationToken: string;
  type: NativeGesturesEventType;
  timestamp: number;
  actionPointerId: number;
  /** Private Cancel scope; omitted means the legacy Android stream-wide Cancel. */
  cancelScope?: 'pointer' | 'stream';
  pointers: NativePointer[];
  viewport: Rect;
}

/** Private iOS ownership cleanup, not a physical pointerCancel or wire event. */
export interface NativeTouchOwnershipAbandonedEvent {
  activationToken: string;
  type: 'ownershipAbandoned';
  pointerIds: number[];
}

export interface TimedPosition {
  /** Physical/native epoch time, used exclusively by internal gesture logic. */
  timestamp: number;
  /** JS sample-processing time, used only when constructing reported events. */
  reportingTimestamp: number;
  position: Offset;
  viewport: Rect;
}

export interface GestureTrace {
  pointerId: number;
  downOrder: number;
  type: GesturesType;
  isOrphanedPointer: boolean;
  lomRef: string | null;
  /** Confirmation frozen with the Down association; never serialized. */
  readonly lomResolvedAtDown: boolean;
  /** Bounded output samples retained from this gesture phase's anchor. */
  positions: TimedPosition[];
  /** Latest physical position; recognition never depends on compacted samples. */
  latestRaw: TimedPosition;
  /** Raw predecessor used to keep the G5 Drag -> Pinch boundary exact. */
  previousRaw: TimedPosition | null;
  /** Adaptive interval used by the bounded retained-sample buffer. */
  sampleIntervalMs: number;
}

export const splitForTransition = (
  trace: GestureTrace,
  newType: GesturesType,
  overrideProps?: Partial<GestureTrace>
): GestureTrace => {
  const newPositions = isEmpty(trace) ? [] : [getLast(trace)];

  return {
    ...trace,
    type: newType,
    positions: newPositions,
    latestRaw: getLast(trace),
    previousRaw: null,
    ...overrideProps,
  };
};

export interface ActionEventParams {
  readonly timestamp: number;
  readonly lomRef: string;
  readonly viewport: Rect;
  readonly position: Offset;
}

export interface TapEvent extends ActionEventParams {
  readonly type: 'tap';
}
export interface DoubleTapEvent extends Omit<ActionEventParams, 'position'> {
  readonly type: 'doubleTap';
  readonly secondTimestamp: number;
  readonly positions: readonly [Offset, Offset];
}
export interface LongPressEvent extends ActionEventParams {
  readonly type: 'longPress';
  readonly duration: number;
}

export type ActionEvent = TapEvent | DoubleTapEvent | LongPressEvent;

export type ScrollPhase = 'start' | 'end';

export interface ExplorationEventParams {
  readonly timestamp: number;
  readonly viewport: Rect;
}

export interface DragExplorationEvent extends ExplorationEventParams {
  readonly type: 'drag';
  readonly lomRef: string;
  readonly position: Offset;
  readonly pointer: number;
}
export interface PinchExplorationEvent extends ExplorationEventParams {
  readonly type: 'pinch';
  readonly endTimestamp: number;
  readonly lomRef: string;
  readonly tracks: PinchTrack[];
}
export interface PinchTrack {
  readonly pointerId: number;
  readonly entryDelta: number;
  readonly exitDelta: number;
  readonly positions: Offset[];
}
export interface ScrollExplorationEvent extends ExplorationEventParams {
  readonly type: 'scroll';
  readonly phase: ScrollPhase;
  readonly offset: Offset;
  readonly lomRef: string;
}

export type ExplorationEvent =
  | DragExplorationEvent
  | PinchExplorationEvent
  | ScrollExplorationEvent;

type ActionParams = Omit<ActionEventParams, 'type'>;
type ExplorationParams = Omit<ExplorationEventParams, 'type'>;

// * ACTIONS

export const createTapEvent = (params: ActionParams): TapEvent => ({
  ...params,
  type: 'tap',
});
export const createDoubleTapEvent = (
  params: Omit<ActionEventParams, 'position'> & {
    secondTimestamp: number;
    positions: readonly [Offset, Offset];
  }
): DoubleTapEvent => ({ ...params, type: 'doubleTap' });

export const createLongPressEvent = (
  params: ActionParams & { duration: number }
): LongPressEvent => ({
  ...params,
  type: 'longPress',
});

const wireLomRef = (lomRef: string | null | undefined): string => lomRef ?? '';

export const concatenateAction = (action: ActionEvent): string => {
  const prefix = [
    action.timestamp.toString(),
    action.type,
    `${Math.trunc(action.viewport.left)},${Math.trunc(action.viewport.top)}`,
  ];

  if (action.type === 'doubleTap') {
    const positions = action.positions
      .map((position) => `${Math.trunc(position.x)},${Math.trunc(position.y)}`)
      .join('|');
    return [
      ...prefix,
      positions,
      action.secondTimestamp.toString(),
      wireLomRef(action.lomRef),
    ].join(':');
  }

  const base = [
    ...prefix,
    `${Math.trunc(action.position.x)},${Math.trunc(action.position.y)}`,
  ].join(':');

  if (action.type === 'longPress') {
    return `${base}:${action.duration}:${wireLomRef(action.lomRef)}`;
  }

  return `${base}:${wireLomRef(action.lomRef)}`;
};

// * EXPLORATIONS

export const createDragEvent = (
  params: ExplorationParams & {
    lomRef: string;
    position: Offset;
    pointer: number;
  }
): DragExplorationEvent => ({
  ...params,
  type: 'drag',
  position: params.position,
  pointer: params.pointer,
});
export const createPinchEvent = (
  params: ExplorationParams & {
    endTimestamp: number;
    lomRef: string;
    tracks: PinchTrack[];
  }
): PinchExplorationEvent => ({
  ...params,
  type: 'pinch',
  endTimestamp: params.endTimestamp,
  tracks: params.tracks,
});

export interface PinchMetricsBaseline {
  /// The geometric center (average position) of all active fingers.
  readonly centroid: Offset | null;

  /// The average distance from each active finger to the centroid.
  readonly avgDistance: number | null;

  /// The collection of all the initial positions.
  readonly initialPositions: ReadonlyMap<number, Offset> | null;
}

export const createZeroPinchBaseline = (): PinchMetricsBaseline => ({
  centroid: null,
  avgDistance: null,
  initialPositions: null,
});

export interface PinchStats {
  readonly avgRadial: number;
  readonly tangentialRms: number;
  readonly consistency: number;
}

export const createScrollEvent = (
  params: ExplorationParams & {
    phase: ScrollPhase;
    offset: Offset;
    lomRef: string;
  }
): ScrollExplorationEvent => ({
  ...params,
  type: 'scroll',
  phase: params.phase,
});

export const getViewportLT = (viewport: Rect): string => {
  return `${Math.trunc(viewport.left)},${Math.trunc(viewport.top)}`;
};
export const getViewportLTWH = (viewport: Rect): string => {
  return `${Math.trunc(viewport.left)},${Math.trunc(viewport.top)},${Math.trunc(viewport.width)},${Math.trunc(viewport.height)}`;
};

export const concatenateExploration = (
  exploration: ExplorationEvent
): string => {
  switch (exploration.type) {
    case 'scroll':
      const phaseName =
        exploration.phase === 'end' ? 'scrollEnd' : 'scrollStart';
      return `${exploration.timestamp}:${phaseName}:${getViewportLTWH(exploration.viewport)}:${Math.trunc(exploration.offset.x)},${Math.trunc(exploration.offset.y)}:${wireLomRef(exploration.lomRef)}`;

    case 'drag':
      return `${exploration.timestamp}:${exploration.type}:${exploration.pointer}:${getViewportLT(exploration.viewport)}:${Math.trunc(exploration.position.x)},${Math.trunc(exploration.position.y)}:${wireLomRef(exploration.lomRef)}`;

    case 'pinch':
      const tracksString = exploration.tracks
        .map((track) => {
          const positionsString = track.positions
            .map((p) => `${Math.trunc(p.x)},${Math.trunc(p.y)}`)
            .join('|');
          return `${track.pointerId},${track.entryDelta},${track.exitDelta}@${positionsString}`;
        })
        .join(';');
      return `${exploration.timestamp}:${exploration.type}:${getViewportLT(exploration.viewport)}:${tracksString}:${exploration.endTimestamp}:${wireLomRef(exploration.lomRef)}`;
  }
};
