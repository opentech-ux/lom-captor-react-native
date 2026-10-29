import {
  consistencyFraction,
  pinchPxThreshold,
  pinchSlop,
  pinchThreshold,
  radialToTang,
} from '../const/gestures_constants';
import type {
  GestureTrace,
  PinchMetricsBaseline,
  PinchStats,
} from '../models/gestures';
import type { Offset } from '../types/geometry';
import { getLastPosition } from './GesturesUtils';

const ZERO_STATS: PinchStats = {
  avgRadial: 0,
  tangentialRms: 0,
  consistency: 0,
};

/**
 Calculates the centroid (average of positions) of all active fingers.
 *
  * @param pointers
  * @returns the position
  */
export const getCentroid = (pointers: Map<number, GestureTrace>): Offset => {
  let sumX = 0,
    sumY = 0,
    count = 0;

  for (const trace of pointers.values()) {
    const pos = getLastPosition(trace);
    sumX += pos.x;
    sumY += pos.y;
    count++;
  }

  if (count === 0) return { x: 0, y: 0 };
  return { x: sumX / count, y: sumY / count };
};

/**
  Calculates the average distance of all fingers to the given centroid.

 * @param pointers
 * @param centroid
 * @returns
 */
export const getAverageDistance = (
  pointers: Map<number, GestureTrace>,
  centroid?: Offset
): number => {
  if (pointers.size < 2) return 0;

  const center = centroid ?? getCentroid(pointers);
  let sumDist = 0,
    count = 0;

  for (const trace of pointers.values()) {
    const pos = getLastPosition(trace);
    sumDist += Math.hypot(pos.x - center.x, pos.y - center.y);
    count++;
  }

  return sumDist / count;
};

/**
 * Analyzes the movement of each finger to measure [Radial] and [Tangential],
 * then we calculate how consistent those movements between the fingers are.
 *
 * Fingers can move:
 * - __Radial__ : Outward or inward from the center (zoom in/zoom out)
 * - __Tangential__ : Rotating around the center (rotation)
 * - In chaotic or inconsistent directions, neither clear zoom nor rotation
 *
 * @param pointers
 * @param initialPointers
 * @param initialCentroid
 * @returns a [PinchStats] object.
 */
export const analyzeFingerDirections = (
  pointers: Map<number, GestureTrace>,
  initialPointers: ReadonlyMap<number, Offset>,
  initialCentroid: Offset
): PinchStats => {
  const currentCentroid = getCentroid(pointers);
  let sumRadial = 0,
    sumTangentialSq = 0;
  let positives = 0,
    negatives = 0,
    counted = 0;

  for (const [pointerId, p0] of initialPointers.entries()) {
    const trace = pointers.get(pointerId);
    if (!trace) continue;

    const pNow = getLastPosition(trace);

    /// From initial centroid to initial position
    const rVecX = p0.x - initialCentroid.x;
    const rVecY = p0.y - initialCentroid.y;
    const rDis = Math.hypot(rVecX, rVecY);

    if (rDis <= 1e-6) continue;

    /// Movement relative to the centroids removes shared translation.
    const vX = pNow.x - currentCentroid.x - rVecX;
    const vY = pNow.y - currentCentroid.y - rVecY;
    const vDis = Math.hypot(vX, vY);

    if (vDis < 3.0) continue;

    /// The [Radial Unit Vector]: normalized radial direction (length 1).
    const uX = rVecX / rDis;
    const uY = rVecY / rDis;

    /// How much of the movement is toward/away from the center.
    /// ```
    /// - radial > 0 : zoom out
    /// - radial < 0 : zoom in
    /// - radial = 0 : rotation around or perpendicular displacement
    /// ```
    const radial = vX * uX + vY * uY;
    sumRadial += radial;

    /// Perpendicular part (rotation/lateral displacement).
    ///
    /// This subtraction eliminates the radial part of the movement;
    /// what remains is the perpendicular (tangential) part.
    const tX = vX - radial * uX;
    const tY = vY - radial * uY;
    sumTangentialSq += tX * tX + tY * tY;

    if (radial > 0) positives++;
    else if (radial < 0) negatives++;

    counted++;
  }

  if (counted === 0) return ZERO_STATS;

  /// Average of radial projections (can be positive or negative)
  /// ```
  /// - If positive : on average, the fingers move apart.
  /// - If negative : on average, they move closer together.
  /// ```
  const avgRadial = sumRadial / counted;

  /// √(mean of the squares of tangential components)
  ///
  /// This is the typical magnitude of lateral movement. We use
  /// RMS to obtain a robust measurement in the face of opposing
  /// directions.
  const tangRms = Math.sqrt(sumTangentialSq / counted);

  /// Fraction of fingers that share the same majority radial direction.
  ///
  /// Example :
  /// - 3 fingers, 2 moving away, 1 moving closer → 2/3 = 0.66 consistency
  /// - If everyone does the same → 3/3 = 1.0 consistency
  ///
  /// Measures whether the majority of fingers “agree” to move away or
  /// closer together.
  const consistency = Math.max(positives, negatives) / counted;

  return { avgRadial, tangentialRms: tangRms, consistency };
};

/**
 * Evaluates whether the current set of active pointers constitutes
 * a zoom gesture.
 *
 * This method computes the centroid, mean distances, and directional
 * vectors between fingers to determine if a scaling motion is occurring.
 *
 * Returns `[true]` if the average radial distance exceeds the `pinchSlop`
 * threshold and both fingers move consistently in a scaling direction.
 * @param pointers
 * @param pinchMetrics
 * @returns
 */
export const evaluatePinchGesture = (
  pointers: Map<number, GestureTrace>,
  pinchMetrics: PinchMetricsBaseline
): boolean => {
  if (
    !pinchMetrics.initialPositions ||
    !pinchMetrics.centroid ||
    !pinchMetrics.avgDistance
  ) {
    return false;
  }

  /// This block filters out most insignificant movements: if there is
  /// no relevant change in the average distance, it is not a zoom.
  const d0 = pinchMetrics.avgDistance;

  /// Avoid divided by 0
  if (d0 <= 1e-6) return false;

  const dNow = getAverageDistance(pointers);
  const scale = dNow / d0;

  const scaleSensitivity = Math.abs(scale - 1.0);
  const scalePx = Math.abs(dNow - d0);

  /// If `scaleSensitivity` is less than `pinchThreshold` and `scalePx` is
  /// less than `pinchPxThreshold` we consider it noise and not zoom.
  const maybePinch =
    scaleSensitivity > pinchThreshold || scalePx > pinchPxThreshold;
  if (!maybePinch) return false;

  const stats = analyzeFingerDirections(
    pointers,
    pinchMetrics.initialPositions,
    pinchMetrics.centroid
  );

  /// We check that the absolute value of `avgRadial` is greater than:
  ///
  /// - `touchSlop`
  /// - `radialToTang` * `tangRms` (i.e., that the radial is several times
  ///   greater than the typical lateral movement).
  ///
  /// `math.max()` uses the greater of the two thresholds, so radial
  /// must exceed the more demanding one.
  const radialDominates =
    Math.abs(stats.avgRadial) >
    Math.max(pinchSlop, radialToTang * stats.tangentialRms);

  /// Requires that the fraction of fingers pointing in the same radial
  /// direction be ≥ `consistencyFraction`
  const consistent = stats.consistency >= consistencyFraction;

  return radialDominates && consistent;
};
