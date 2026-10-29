package com.sessionrecorderreactnative;

import android.app.Activity;
import android.graphics.Rect;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;

import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReadableArray;
import com.facebook.react.bridge.ReadableMap;
import com.facebook.react.bridge.WritableArray;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.modules.core.DeviceEventManagerModule;
import com.facebook.react.uimanager.PixelUtil;
import com.facebook.react.views.scroll.ReactHorizontalScrollView;
import com.facebook.react.views.scroll.ReactScrollView;
import com.facebook.react.views.scroll.ReactScrollViewHelper;
import com.facebook.react.views.scroll.ScrollEventType;

import java.util.IdentityHashMap;
import java.util.Map;

public class SessionRecorderScrollInterceptor {
  private static final long IDLE_TIMEOUT_MS = 120L;
  private static final float MOVEMENT_THRESHOLD_DIP = 1.0f;
  private static final Handler MAIN_HANDLER = new Handler(Looper.getMainLooper());

  private static final Map<View, ObservedScrollView> observedScrollViews = new IdentityHashMap<>();
  // 'volatile' ensures all CPU threads instantly know if registration is turned off
  private static volatile boolean isRegistered = false;
  private static long nextToken = 1L;
  private static ReactApplicationContext reactContext;
  private static ViewGroup applicationViewport;

  private static final ReactScrollViewHelper.ScrollListener scrollLifecycleListener =
    new ReactScrollViewHelper.ScrollListener() {
      @Override
      public void onScroll(
        ViewGroup scrollView,
        ScrollEventType eventType,
        float xVelocity,
        float yVelocity
      ) {
        if (!isRegistered || scrollView == null) return;
        if (!isSupportedScrollView(scrollView)) return;

        if (eventType == ScrollEventType.BEGIN_DRAG) {
          ObservedScrollView observed = observeScrollView(scrollView);
          observed.lastX = scrollView.getScrollX();
          observed.lastY = scrollView.getScrollY();
          beginSession(scrollView, observed, observed.lastX, observed.lastY, false);
        } else if (eventType == ScrollEventType.SCROLL) {
          onScrollChanged(scrollView, scrollView.getScrollX(), scrollView.getScrollY());
        } else if (eventType == ScrollEventType.END_DRAG) {
          onScrollEndDrag(scrollView);
        } else if (eventType == ScrollEventType.MOMENTUM_END) {
          finishSession(scrollView, null);
        }
      }

      @Override
      public void onLayout(ViewGroup scrollView) {
        if (!isRegistered || scrollView == null || !isSupportedScrollView(scrollView)) return;

        ObservedScrollView observed = observeScrollView(scrollView);
        observed.lastX = scrollView.getScrollX();
        observed.lastY = scrollView.getScrollY();
      }
    };

  private static final View.OnAttachStateChangeListener attachStateListener =
    new View.OnAttachStateChangeListener() {
      @Override
      public void onViewAttachedToWindow(View view) {
        // Observation starts from the additive RN callbacks or the initial one-shot discovery.
      }

      @Override
      public void onViewDetachedFromWindow(View view) {
        if (!isRegistered) return;
        removeObservedScrollView(view, true);
      }
    };

  public static void register(Activity activity, ReactApplicationContext context) {
    if (isRegistered || activity == null || context == null) return;

    ViewGroup root = activity.findViewById(android.R.id.content);
    if (root == null) return;

    reactContext = context;
    applicationViewport = root;
    isRegistered = true;

    ReactScrollViewHelper.addScrollListener(scrollLifecycleListener);

    // Seed baselines for standard RN scroll views already mounted before init.
    // Future views are discovered by the additive RN callbacks; no host listener is replaced.
    discoverInitialScrollViews(root);
  }

  public static void unregister() {
    if (!isRegistered) return;
    isRegistered = false;

    // Best-effort drain while native state and the React instance still exist.
    try {
      for (View view : observedScrollViews.keySet().toArray(new View[0])) {
        try {
          finishSession(view, null);
        } catch (RuntimeException e) {
          Log.e("SessionRecorder", "Error draining scroll during cleanup", e);
        }
      }
    } finally {
      try {
        ReactScrollViewHelper.removeScrollListener(scrollLifecycleListener);
      } catch (RuntimeException e) {
        Log.e("SessionRecorder", "Error removing scroll listener", e);
      }
      try {
        clearObservedScrollViews();
      } catch (RuntimeException e) {
        Log.e("SessionRecorder", "Error clearing observed scroll views", e);
      } finally {
        reactContext = null;
        applicationViewport = null;
      }
    }
  }

  // Called on the UI thread. Reads metadata only; never mutates a session.
  public static WritableArray snapshotScrollSessions(ReadableArray requests) {
    WritableArray results = Arguments.createArray();
    if (!isRegistered || requests == null) return results;

    for (int i = 0; i < requests.size(); i++) {
      try {
        ReadableMap request = requests.getMap(i);
        if (request == null) continue;
        double requestedViewId = request.getDouble("viewId");
        double requestedToken = request.getDouble("token");
        if (!Double.isFinite(requestedViewId) || requestedViewId != (int) requestedViewId
            || !Double.isFinite(requestedToken) || requestedToken < 1
            || requestedToken > 9007199254740991d || requestedToken != Math.rint(requestedToken)) continue;

        for (Map.Entry<View, ObservedScrollView> entry : observedScrollViews.entrySet()) {
          View view = entry.getKey();
          ScrollSession session = entry.getValue().session;
          if (view.getId() != (int) requestedViewId || session == null
              || session.token != (long) requestedToken || !view.isAttachedToWindow()) continue;

          OffsetPx offset = effectiveOffset(view, view.getScrollX(), view.getScrollY());
          WritableMap result = Arguments.createMap();
          result.putInt("viewId", view.getId());
          result.putDouble("token", session.token);
          result.putDouble("offsetX", PixelUtil.toDIPFromPixel(offset.x));
          result.putDouble("offsetY", PixelUtil.toDIPFromPixel(offset.y));
          ScrollRect rect = null;
          try {
            rect = visibleScrollRect(view);
          } catch (RuntimeException ignored) {
            // Keep a valid offset even when viewport metadata is unavailable.
          }
          if (rect != null) {
            WritableMap viewport = Arguments.createMap();
            viewport.putDouble("left", rect.left);
            viewport.putDouble("top", rect.top);
            viewport.putDouble("width", rect.width);
            viewport.putDouble("height", rect.height);
            result.putMap("viewport", viewport);
          }
          results.pushMap(result);
          break;
        }
      } catch (RuntimeException e) {
        // An unavailable/malformed entry falls back independently in JS.
      }
    }
    return results;
  }

  private static void discoverInitialScrollViews(View view) {
    if (isSupportedScrollView(view)) {
      observeScrollView(view);
    }

    if (view instanceof ViewGroup group) {
      for (int i = 0; i < group.getChildCount(); i++) {
        discoverInitialScrollViews(group.getChildAt(i));
      }
    }
  }

  private static ObservedScrollView observeScrollView(View view) {
    ObservedScrollView observed = observedScrollViews.get(view);
    if (observed != null) return observed;

    observed = new ObservedScrollView(view.getScrollX(), view.getScrollY());
    observedScrollViews.put(view, observed);
    view.addOnAttachStateChangeListener(attachStateListener);
    return observed;
  }

  private static void onScrollChanged(View view, int scrollX, int scrollY) {
    if (!isRegistered) return;

    ObservedScrollView observed = observedScrollViews.get(view);
    if (observed == null) {
      // RN's additive listener does not expose the previous offset. Seed a trustworthy
      // baseline and wait for the next change instead of inventing one.
      observeScrollView(view);
      return;
    }

    ScrollSession session = observed.session;
    if (session == null) {
      session = beginSession(view, observed, observed.lastX, observed.lastY, true);
      if (session == null) {
        observed.lastX = scrollX;
        observed.lastY = scrollY;
        return;
      }
    }

    OffsetPx current = effectiveOffset(view, scrollX, scrollY);
    session.currentX = current.x;
    session.currentY = current.y;
    session.lastTimestamp = session.nowEpochMillis();
    observed.lastX = scrollX;
    observed.lastY = scrollY;

    validateSessionMovement(session, current);

    if (session.usesIdleFallback) {
      scheduleIdleFallback(view, session);
    }
  }

  private static void onScrollEndDrag(View view) {
    ObservedScrollView observed = observedScrollViews.get(view);
    if (observed == null || observed.session == null) return;

    // RN only emits MOMENTUM_END when momentum events are enabled. Keep the existing idle
    // fallback available after touch release so sessions still terminate otherwise.
    observed.session.usesIdleFallback = true;
    scheduleIdleFallback(view, observed.session);
  }

  private static ScrollSession beginSession(
    View view,
    ObservedScrollView observed,
    int initialX,
    int initialY,
    boolean usesIdleFallback
  ) {
    if (!isRegistered || !isSupportedScrollView(view)) return null;

    if (observed.session != null) {
      finishSession(view, null);
    }

    ScrollRect rect = visibleScrollRect(view);
    if (rect == null) return null;

    OffsetPx initial = effectiveOffset(view, initialX, initialY);
    ScrollSession session = new ScrollSession(
      nextToken++,
      view.getId(),
      initial.x,
      initial.y,
      rect,
      usesIdleFallback
    );
    observed.session = session;
    emitSignal("SessionRecorderScrollStart", session, rect, session.startTimestamp);
    return session;
  }

  private static void scheduleIdleFallback(View view, ScrollSession session) {
    if (session.idleRunnable == null) {
      final long token = session.token;
      session.idleRunnable = () -> finishSession(view, token);
    } else {
      MAIN_HANDLER.removeCallbacks(session.idleRunnable);
    }

    MAIN_HANDLER.postDelayed(session.idleRunnable, IDLE_TIMEOUT_MS);
  }

  private static void finishSession(View view, Long expectedToken) {
    ObservedScrollView observed = observedScrollViews.get(view);
    if (observed == null) return;

    ScrollSession session = observed.session;
    if (session == null || (expectedToken != null && session.token != expectedToken)) return;

    observed.session = null;
    if (session.idleRunnable != null) {
      MAIN_HANDLER.removeCallbacks(session.idleRunnable);
      session.idleRunnable = null;
    }

    ScrollRect endRect = visibleScrollRect(view);
    if (endRect == null) endRect = session.lastRect;

    OffsetPx end = effectiveOffset(view, view.getScrollX(), view.getScrollY());
    session.currentX = end.x;
    session.currentY = end.y;
    session.lastTimestamp = session.nowEpochMillis();
    observed.lastX = view.getScrollX();
    observed.lastY = view.getScrollY();

    // A throttled or missing SCROLL callback must not hide movement observed at the terminal.
    validateSessionMovement(session, end);

    emitSignal(
      session.validated ? "SessionRecorderScrollEnd" : "SessionRecorderScrollCancel",
      session,
      endRect,
      session.lastTimestamp
    );
  }

  private static void validateSessionMovement(ScrollSession session, OffsetPx current) {
    if (session.validated) return;

    double deltaX = current.x - session.initialX;
    double deltaY = current.y - session.initialY;
    double thresholdPx = PixelUtil.toPixelFromDIP(MOVEMENT_THRESHOLD_DIP);
    if ((deltaX * deltaX) + (deltaY * deltaY) > thresholdPx * thresholdPx) {
      session.validated = true;
      emitSignal("SessionRecorderScrollValidated", session, null, session.lastTimestamp);
    }
  }

  private static void removeObservedScrollView(View view, boolean emitRemoval) {
    ObservedScrollView observed = observedScrollViews.get(view);
    if (observed == null) return;

    finishSession(view, null);
    view.removeOnAttachStateChangeListener(attachStateListener);
    observedScrollViews.remove(view);

    if (emitRemoval) {
      WritableMap params = Arguments.createMap();
      params.putInt("viewId", view.getId());
      emit("SessionRecorderScrollRemoved", params);
    }
  }

  private static void emitSignal(
    String eventName,
    ScrollSession session,
    ScrollRect rect,
    long timestamp
  ) {
    WritableMap params = Arguments.createMap();
    params.putDouble("token", session.token);
    params.putInt("viewId", session.viewId);
    params.putDouble("timestamp", timestamp);
    params.putDouble("scrollX", PixelUtil.toDIPFromPixel(session.currentX));
    params.putDouble("scrollY", PixelUtil.toDIPFromPixel(session.currentY));

    ScrollRect effectiveRect = rect != null ? rect : session.lastRect;
    if (effectiveRect != null) {
      params.putDouble("left", effectiveRect.left);
      params.putDouble("top", effectiveRect.top);
      params.putDouble("width", effectiveRect.width);
      params.putDouble("height", effectiveRect.height);
      session.lastRect = effectiveRect;
    }

    emit(eventName, params);
  }

  private static void emit(String eventName, WritableMap params) {
    ReactApplicationContext context = reactContext;
    if (context == null || !context.hasActiveReactInstance()) return;

    context
      .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter.class)
      .emit(eventName, params);
  }

  private static ScrollRect visibleScrollRect(View view) {
    ViewGroup viewport = applicationViewport;
    if (viewport == null || !view.isShown()) return null;

    Rect visible = new Rect();
    Rect appVisible = new Rect();
    if (!view.getGlobalVisibleRect(visible) || !viewport.getGlobalVisibleRect(appVisible)) {
      return null;
    }
    if (!visible.intersect(appVisible) || visible.width() <= 0 || visible.height() <= 0) {
      return null;
    }

    return new ScrollRect(
      PixelUtil.toDIPFromPixel(visible.left - appVisible.left),
      PixelUtil.toDIPFromPixel(visible.top - appVisible.top),
      PixelUtil.toDIPFromPixel(visible.width()),
      PixelUtil.toDIPFromPixel(visible.height())
    );
  }

  private static OffsetPx effectiveOffset(View view, int rawX, int rawY) {
    int viewportWidth = Math.max(0, view.getWidth() - view.getPaddingLeft() - view.getPaddingRight());
    int viewportHeight = Math.max(0, view.getHeight() - view.getPaddingTop() - view.getPaddingBottom());
    int contentWidth = 0;
    int contentHeight = 0;

    if (view instanceof ViewGroup group && group.getChildCount() > 0) {
      View content = group.getChildAt(0);
      contentWidth = content.getWidth();
      contentHeight = content.getHeight();
    }

    int maxX = Math.max(0, contentWidth - viewportWidth);
    int maxY = Math.max(0, contentHeight - viewportHeight);
    return new OffsetPx(clamp(rawX, 0, maxX), clamp(rawY, 0, maxY));
  }

  private static int clamp(int value, int min, int max) {
    return Math.max(min, Math.min(value, max));
  }

  private static boolean isSupportedScrollView(View view) {
    return view instanceof ReactScrollView || view instanceof ReactHorizontalScrollView;
  }

  private static void clearObservedScrollViews() {
    for (Map.Entry<View, ObservedScrollView> entry : observedScrollViews.entrySet()) {
      try {
        entry.getKey().removeOnAttachStateChangeListener(attachStateListener);
      } catch (RuntimeException e) {
        Log.e("SessionRecorder", "Error removing scroll attach listener", e);
      }
      ScrollSession session = entry.getValue().session;
      if (session != null && session.idleRunnable != null) {
        try {
          MAIN_HANDLER.removeCallbacks(session.idleRunnable);
        } catch (RuntimeException e) {
          Log.e("SessionRecorder", "Error removing scroll idle timer", e);
        }
      }
    }
    observedScrollViews.clear();
  }

  private static final class ObservedScrollView {
    int lastX;
    int lastY;
    ScrollSession session;

    ObservedScrollView(int lastX, int lastY) {
      this.lastX = lastX;
      this.lastY = lastY;
    }
  }

  private static final class OffsetPx {
    final int x;
    final int y;

    OffsetPx(int x, int y) {
      this.x = x;
      this.y = y;
    }
  }

  private static final class ScrollRect {
    final double left;
    final double top;
    final double width;
    final double height;

    ScrollRect(double left, double top, double width, double height) {
      this.left = left;
      this.top = top;
      this.width = width;
      this.height = height;
    }
  }

  private static final class ScrollSession {
    final long token;
    final int viewId;
    final int initialX;
    final int initialY;
    final long epochOffsetMillis;
    final long startTimestamp;
    boolean usesIdleFallback;
    int currentX;
    int currentY;
    long lastTimestamp;
    boolean validated;
    ScrollRect lastRect;
    Runnable idleRunnable;

    ScrollSession(
      long token,
      int viewId,
      int initialX,
      int initialY,
      ScrollRect rect,
      boolean usesIdleFallback
    ) {
      this.token = token;
      this.viewId = viewId;
      this.initialX = initialX;
      this.initialY = initialY;
      this.currentX = initialX;
      this.currentY = initialY;
      this.epochOffsetMillis = System.currentTimeMillis() - SystemClock.uptimeMillis();
      this.startTimestamp = nowEpochMillis();
      this.usesIdleFallback = usesIdleFallback;
      this.lastTimestamp = startTimestamp;
      this.lastRect = rect;
      this.validated = false;
    }

    long nowEpochMillis() {
      return epochOffsetMillis + SystemClock.uptimeMillis();
    }
  }
}
