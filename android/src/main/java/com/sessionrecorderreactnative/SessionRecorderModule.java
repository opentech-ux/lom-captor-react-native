package com.sessionrecorderreactnative;

import android.app.Activity;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Window;

import androidx.annotation.NonNull;

import com.facebook.react.bridge.LifecycleEventListener;
import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.Promise;
import com.facebook.react.bridge.ReadableArray;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.ReactContextBaseJavaModule;
import com.facebook.react.bridge.ReactMethod;

import java.util.Collections;
import java.util.ArrayList;
import java.util.Map;
import java.util.Set;
import java.util.WeakHashMap;

// Main module exposing methods to React Native and handling the lifecycle
public class SessionRecorderModule extends ReactContextBaseJavaModule implements LifecycleEventListener {

  private static final Handler MAIN_HANDLER = new Handler(Looper.getMainLooper());
  private final boolean hostDebuggable;
  private final SessionRecorderPagerObserver pagerObserver;
  private final SessionRecorderScreenVisibilityObserver screenVisibilityObserver;
  private final SessionRecorderExtraWindowEvents extraWindowEvents;
  // Only Windows previously notified by RN; never enumerate process windows.
  private final Set<Window> knownModalWindows = Collections.newSetFromMap(new WeakHashMap<Window, Boolean>());

  // Requested by the JS runtime, not by Activity lifecycle or interceptor registration.
  private volatile boolean captureRequested = false;
  private volatile String touchActivationToken;

  public SessionRecorderModule(ReactApplicationContext reactContext, boolean hostDebuggable) {
    super(reactContext);
    this.hostDebuggable = hostDebuggable;
    this.pagerObserver = new SessionRecorderPagerObserver(reactContext);
    this.screenVisibilityObserver = new SessionRecorderScreenVisibilityObserver(reactContext);
    this.extraWindowEvents = new SessionRecorderExtraWindowEvents(reactContext,
        this::onModalWindowCreate, this::onModalWindowDestroy);
    reactContext.addLifecycleEventListener(this);
  }

  private void registerInterceptors(String requestedToken) {
    if (!captureRequested || requestedToken == null || !requestedToken.equals(touchActivationToken)) return;
    Activity activity = getReactApplicationContext().getCurrentActivity();
    if (activity != null) {
      activity.runOnUiThread(() -> {
        // A pause or cleanup may have revoked activation while this work was queued.
        if (!captureRequested || !requestedToken.equals(touchActivationToken)
            || getReactApplicationContext().getCurrentActivity() != activity) return;
        try {
          SessionRecorderTouchInterceptor.register(activity, getReactApplicationContext(), requestedToken);
          extraWindowEvents.start();
          for (Window window : new ArrayList<>(knownModalWindows)) {
            if (!SessionRecorderTouchInterceptor.registerModal(window, activity, getReactApplicationContext(), requestedToken))
              knownModalWindows.remove(window);
          }
        } catch (RuntimeException e) {
          Log.e("SessionRecorder", "Error registering touch interceptor", e);
        }
        pagerObserver.start(requestedToken);
        screenVisibilityObserver.start(requestedToken);
        try {
          SessionRecorderScrollInterceptor.register(activity, getReactApplicationContext());
        } catch (RuntimeException e) {
          Log.e("SessionRecorder", "Error registering scroll interceptor", e);
        }
      });
    }
  }

  private void onModalWindowCreate(Window window) {
    String token = touchActivationToken;
    if (!captureRequested || token == null) return;
    Activity activity = getReactApplicationContext().getCurrentActivity();
    if (SessionRecorderTouchInterceptor.registerModal(window, activity, getReactApplicationContext(), token))
      knownModalWindows.add(window);
  }

  private void onModalWindowDestroy(Window window) {
    knownModalWindows.remove(window);
    SessionRecorderTouchInterceptor.unregisterModal(window, getReactApplicationContext());
  }

  private void unregisterInterceptors() {
    Runnable cleanup = () -> {
      extraWindowEvents.stop();
      pagerObserver.stop();
      screenVisibilityObserver.stop();
      try {
        SessionRecorderTouchInterceptor.unregister(getReactApplicationContext());
      } catch (RuntimeException e) {
        Log.e("SessionRecorder", "Error removing touch interceptor", e);
      }
      try {
        SessionRecorderScrollInterceptor.unregister();
      } catch (RuntimeException e) {
        Log.e("SessionRecorder", "Error removing scroll interceptor", e);
      }
    };
    if (Looper.myLooper() == Looper.getMainLooper()) {
      cleanup.run();
    } else {
      MAIN_HANDLER.post(cleanup);
    }
  }

  @ReactMethod
  public void addListener(String eventName) {
  }

  @ReactMethod
  public void removeListeners(Integer count) {
  }

  @NonNull
  @Override
  public String getName() {
    return "SessionRecorderLifecycle";
  }

  @NonNull
  @Override
  public Map<String, Object> getConstants() {
    return Collections.<String, Object>singletonMap(
        "buildMode", hostDebuggable ? "debug" : "release");
  }

  @Override
  public void onHostResume() {
    registerInterceptors(touchActivationToken);
  }

  @ReactMethod
  public void startListening(String activationToken) {
    touchActivationToken = activationToken;
    captureRequested = true;
    registerInterceptors(activationToken);
  }

  @ReactMethod
  public void stopListening() {
    touchActivationToken = null;
    captureRequested = false;
    unregisterInterceptors();
  }

  @ReactMethod
  public void snapshotScrollSessions(ReadableArray requests, Promise promise) {
    Runnable snapshot = () -> {
      try {
        if (!getReactApplicationContext().hasActiveReactInstance()) {
          promise.resolve(Arguments.createArray());
          return;
        }
        promise.resolve(SessionRecorderScrollInterceptor.snapshotScrollSessions(requests));
      } catch (RuntimeException e) {
        try {
          promise.reject("E_SCROLL_SNAPSHOT", e);
        } catch (RuntimeException ignored) {
          // The React instance may already have been invalidated.
        }
      }
    };
    try {
      if (Looper.myLooper() == Looper.getMainLooper()) {
        snapshot.run();
      } else if (!MAIN_HANDLER.post(snapshot)) {
        promise.reject("E_SCROLL_SNAPSHOT", "UI thread is unavailable");
      }
    } catch (RuntimeException e) {
      try {
        promise.reject("E_SCROLL_SNAPSHOT", e);
      } catch (RuntimeException ignored) {}
    }
  }

  @Override
  public void onHostPause() {
  }

  @Override
  public void onHostDestroy() {
    // Activity lifetime is independent of the React runtime. Keep JS authorization
    // and the lifecycle listener so a later Activity can resume the same runtime.
    unregisterInterceptors();
  }

  // Direct JNI bridge to C++ for emergency shutdown
  private native void nativeCleanup();

  @Override
  public void invalidate() {
    touchActivationToken = null;
    captureRequested = false;
    try {
      // Module invalidation is definitive: shut down C++ before RN destroys the runtime.
      nativeCleanup();
    } catch (Exception e) {
    }

    // Disconnect UI-owned interceptors on the main thread during bridge invalidation.
    unregisterInterceptors();
    getReactApplicationContext().removeLifecycleEventListener(this);
    super.invalidate();
  }
}
