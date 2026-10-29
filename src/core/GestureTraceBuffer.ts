import type { GestureTrace, TimedPosition } from '../models/gestures';

const MAX_RETAINED_POSITIONS = 256;

export function _recordPosition(
  buffer: Pick<
    GestureTrace,
    'positions' | 'latestRaw' | 'previousRaw' | 'sampleIntervalMs'
  >,
  point: TimedPosition
): void {
  buffer.previousRaw = buffer.latestRaw;
  buffer.latestRaw = point;

  const lastRetained = buffer.positions[buffer.positions.length - 1];
  if (
    !lastRetained ||
    point.timestamp - lastRetained.timestamp < buffer.sampleIntervalMs
  ) {
    return;
  }

  if (buffer.positions.length >= MAX_RETAINED_POSITIONS) {
    _compactPositions(buffer);
  }
  const compactedLast = buffer.positions[buffer.positions.length - 1]!;
  if (point.timestamp - compactedLast.timestamp >= buffer.sampleIntervalMs) {
    buffer.positions.push(point);
  }
}

export function _appendTerminalPosition(
  buffer: Pick<GestureTrace, 'positions' | 'latestRaw' | 'sampleIntervalMs'>,
  terminal: TimedPosition = buffer.latestRaw
): void {
  const last = buffer.positions[buffer.positions.length - 1];
  if (
    last &&
    last.timestamp === terminal.timestamp &&
    last.position.x === terminal.position.x &&
    last.position.y === terminal.position.y &&
    last.viewport.left === terminal.viewport.left &&
    last.viewport.top === terminal.viewport.top &&
    last.viewport.width === terminal.viewport.width &&
    last.viewport.height === terminal.viewport.height
  ) {
    return;
  }
  if (buffer.positions.length >= MAX_RETAINED_POSITIONS) {
    _compactPositions(buffer);
  }
  buffer.positions.push(terminal);
}

function _compactPositions(
  buffer: Pick<GestureTrace, 'positions' | 'sampleIntervalMs'>
): void {
  const positions = buffer.positions;
  if (positions.length < MAX_RETAINED_POSITIONS) return;

  const compacted: TimedPosition[] = [positions[0]!];
  for (let index = 2; index < positions.length - 1; index += 2) {
    compacted.push(positions[index]!);
  }
  const last = positions[positions.length - 1]!;
  if (compacted[compacted.length - 1] !== last) compacted.push(last);
  positions.splice(0, positions.length, ...compacted);
  buffer.sampleIntervalMs = Math.min(
    Number.MAX_VALUE,
    buffer.sampleIntervalMs * 2
  );
}
