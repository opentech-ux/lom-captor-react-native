/// The total movement constant
export const touchSlop: number = 8.0;

/// The total pinch constant
export const pinchSlop: number = 20.0;

/// The total movement constant
export const doubleTapTouchSlop: number = 18.0;

/// The maximum duration for a second touch
export const doubleTapTimeoutMs: number = 300;

/// The time before a long press gesture attempts to win
export const longPressTimeoutMs: number = 500;

/// Minimum of pixels if scroll has moved
export const scrollSlop: number = 10.0;

export const debounceTimeMs: number = 300;
export const cooldownTimeMs: number = 400;

export const doubleTapSlop: number = 18.0;

// * PINCH CONSTS

/// Ratio between radial and tangential movement required
/// to consider the gesture a valid zoom.
///
/// If radial movement is smaller than [tangentialRms] * [radialToTang],
/// the gesture is treated as noise or rotation instead.
export const radialToTang: number = 1.5;

/// Minimum fraction of fingers that must move coherently
/// (in the same radial direction) for the gesture to be consistent.
///
/// Example: 0.6 = at least 60% of the fingers agree.
export const consistencyFraction: number = 0.6;

/// Minimum relative pinch change to trigger a zoom event.
///
/// Example: 0.05 = 5% zoom in or out.
export const pinchThreshold: number = 0.05;

/// Minimum absolute change in average distance (in pixels)
/// required to trigger a zoom gesture — useful for small or slow movements.
export const pinchPxThreshold: number = 6.0;
