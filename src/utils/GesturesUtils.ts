import type { GestureTrace, TimedPosition } from '../models/gestures';
import type { Offset } from '../types/geometry';

const FALLBACK_POSITION: TimedPosition = {
  timestamp: 0,
  reportingTimestamp: 0,
  position: { x: 0, y: 0 },
  viewport: { top: 0, left: 0, width: 0, height: 0 },
};

export const getFirst = (trace: GestureTrace): TimedPosition =>
  trace.positions[0] ?? FALLBACK_POSITION;

export const getLast = (trace: GestureTrace): TimedPosition =>
  trace.latestRaw ?? FALLBACK_POSITION;

export const getFirstPosition = (trace: GestureTrace): Offset =>
  getFirst(trace).position;
export const getLastPosition = (trace: GestureTrace): Offset =>
  getLast(trace).position;

export const getFirstTimestamp = (trace: GestureTrace): number =>
  getFirst(trace).timestamp;
export const getLastTimestamp = (trace: GestureTrace): number =>
  getLast(trace).timestamp;

export const isEmpty = (trace: GestureTrace): boolean =>
  trace.positions.length === 0;
export const length = (trace: GestureTrace): number => trace.positions.length;

export const duration = (trace: GestureTrace): number => {
  if (isEmpty(trace)) return 0;
  // if (length(trace) < 2) {
  //   return getFirstTimestamp(trace) - getFirstTimestamp(trace);
  // }
  return getLastTimestamp(trace) - getFirstTimestamp(trace);
};

export const distance = (trace: GestureTrace): number => {
  if (isEmpty(trace)) return 0.0;

  const first = getFirstPosition(trace);
  const last = getLastPosition(trace);

  const dx = last.x - first.x;
  const dy = last.y - first.y;
  return Math.sqrt(dx * dx + dy * dy);
};
