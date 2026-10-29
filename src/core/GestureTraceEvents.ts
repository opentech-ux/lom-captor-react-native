import {
  createLongPressEvent,
  createTapEvent,
  type ActionEvent,
  type ActionEventParams,
  type DragExplorationEvent,
  type ExplorationEvent,
  type GestureTrace,
  type TimedPosition,
} from '../models/gestures';
import { duration, getFirst, isEmpty } from '../utils/GesturesUtils';

/**
 * Creates an array of ActionEvent based on the `gestureTrace` type.
 */
export function _createActionEvent(
  gestureTrace: GestureTrace
): ActionEvent | null {
  if (gestureTrace.lomRef === null) return null;

  const firstPosition: TimedPosition = getFirst(gestureTrace);

  const baseParams: ActionEventParams = {
    lomRef: gestureTrace.lomRef,
    timestamp: firstPosition.reportingTimestamp,
    viewport: firstPosition.viewport,
    position: firstPosition.position,
  };

  switch (gestureTrace.type) {
    case 'longPress':
      return createLongPressEvent({
        ...baseParams,
        duration: duration(gestureTrace),
      });

    default:
      return createTapEvent(baseParams);
  }
}

/**
 * Creates an array of ExplorationEvents based on the `gestureTrace` type.
 */
export function _createExplorationEvent(
  gestureTrace: GestureTrace
): ExplorationEvent[] {
  if (gestureTrace.lomRef === null) return [];

  switch (gestureTrace.type) {
    case 'drag':
      return _getDragExploration(gestureTrace, gestureTrace.lomRef);

    default:
      return [];
  }
}

/**
 * Converts the recorded `gestureTrace` data into a list of DragExplorationEvent instances.
 */
function _getDragExploration(
  gestureTrace: GestureTrace,
  lomRef: string
): DragExplorationEvent[] {
  if (isEmpty(gestureTrace)) return [];
  return gestureTrace.positions.map((sample: TimedPosition) => ({
    type: 'drag',
    lomRef,
    timestamp: sample.reportingTimestamp,
    pointer: gestureTrace.pointerId,
    viewport: sample.viewport,
    position: sample.position,
  }));
}
