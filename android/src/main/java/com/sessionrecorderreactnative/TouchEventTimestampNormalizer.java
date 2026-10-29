package com.sessionrecorderreactnative;

/** Converts MotionEvent uptime timestamps to epoch milliseconds. */
final class TouchEventTimestampNormalizer {
  private long epochOffsetMillis;
  private boolean hasEpochOffset;

  long toEpochMillis(
    long eventUptimeMillis,
    long epochNowMillis,
    long uptimeNowMillis,
    boolean startsGesture,
    boolean endsGesture
  ) {
    if (startsGesture || !hasEpochOffset) {
      epochOffsetMillis = epochNowMillis - uptimeNowMillis;
      hasEpochOffset = true;
    }

    long eventEpochMillis = epochOffsetMillis + eventUptimeMillis;

    if (endsGesture) {
      hasEpochOffset = false;
    }

    return eventEpochMillis;
  }
}
