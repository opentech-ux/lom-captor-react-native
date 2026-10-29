package com.sessionrecorderreactnative;

import android.app.Activity;
import android.content.Context;
import android.content.ContextWrapper;
import android.os.SystemClock;
import android.view.ActionMode;
import android.view.KeyEvent;
import android.view.KeyboardShortcutGroup;
import android.view.Menu;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.SearchEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.WritableArray;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.bridge.UIManager;
import com.facebook.react.modules.core.DeviceEventManagerModule;
import com.facebook.react.uimanager.PixelUtil;
import com.facebook.react.uimanager.ThemedReactContext;
import com.facebook.react.uimanager.UIManagerHelper;
import com.facebook.react.uimanager.common.UIManagerType;
import com.facebook.react.views.modal.ReactModalHostView;

import java.util.ArrayDeque;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.BooleanSupplier;

public class SessionRecorderTouchInterceptor implements Window.Callback {
  private static Window installedWindow;
  private static SessionRecorderTouchInterceptor installedInterceptor;
  private static final Map<Window, SessionRecorderTouchInterceptor> modalInterceptors = new IdentityHashMap<>();
  // One observed physical stream at a time; never merge pointer IDs from Windows.
  private static SessionRecorderTouchInterceptor streamOwner;
  private static boolean alertPresent;

  private final Window.Callback originalCallback;
  private final String activationToken;
  private final int[] contentOriginOnScreen = new int[2];
  private boolean active = true;
  private ReactApplicationContext reactContext;
  private View applicationViewport;
  private Window modalWindow;
  private ReactModalHostView modalHost;
  private BooleanSupplier alertEvidence;
  private final Set<Integer> ownedPointers = new HashSet<>();
  private TouchEventTimestampNormalizer timestampNormalizer =
    new TouchEventTimestampNormalizer();

  private SessionRecorderTouchInterceptor(
    Window.Callback original,
    ReactApplicationContext ctx,
    View applicationViewport,
    String activationToken
  ) {
    this.originalCallback = original;
    this.reactContext = ctx;
    this.applicationViewport = applicationViewport;
    this.activationToken = activationToken;
  }

  public static void register(Activity activity, ReactApplicationContext ctx, String activationToken) {
    if (activity == null || ctx == null || activationToken == null) return;

    Window window = activity.getWindow();
    if (window == null) return;

    View applicationViewport = activity.findViewById(android.R.id.content);
    if (applicationViewport == null) return;
    if (installedWindow == window && installedInterceptor != null && installedInterceptor.active &&
        installedInterceptor.reactContext == ctx && activationToken.equals(installedInterceptor.activationToken) &&
        installedInterceptor.applicationViewport == applicationViewport) return;

    unregister();

    Window.Callback existing = window.getCallback();

    // A direct stale wrapper can remain after older lifecycle implementations.
    // Unwrap only our own direct callback so Session Recorder never wraps itself.
    if (existing instanceof SessionRecorderTouchInterceptor staleInterceptor) {
      staleInterceptor.deactivate();
      existing = staleInterceptor.originalCallback;
      window.setCallback(existing);
    }

    SessionRecorderTouchInterceptor interceptor =
      new SessionRecorderTouchInterceptor(existing, ctx, applicationViewport, activationToken);
    window.setCallback(interceptor);
    installedWindow = window;
    installedInterceptor = interceptor;
  }

  // Only the notified Dialog's decor is inspected, once per installation.
  // No getDialog() testing API, reflection, Activity-tree search or frame observer.
  public static boolean registerModal(Window window, Activity activity, ReactApplicationContext ctx, String token) {
    if (window == null || activity == null || window == installedWindow || installedInterceptor == null ||
        !installedInterceptor.active || installedInterceptor.reactContext != ctx ||
        !installedInterceptor.activationToken.equals(token) || installedWindow != activity.getWindow()) return false;
    try {
      Context owner = window.getContext();
      while (owner instanceof ContextWrapper && owner != activity) {
        Context base = ((ContextWrapper) owner).getBaseContext();
        if (base == owner) break;
        owner = base;
      }
      if (owner != activity) return false;
      View decor = window.peekDecorView();
      if (!(decor instanceof ViewGroup)) return false;
      ReactModalHostView.DialogRootViewGroup root = null;
      ArrayDeque<View> pending = new ArrayDeque<>();
      pending.add(decor);
      int inspected = 0;
      while (!pending.isEmpty()) {
        if (++inspected > 256) return false;
        View view = pending.removeFirst();
        if (view instanceof ReactModalHostView.DialogRootViewGroup) {
          if (root != null) return false;
          root = (ReactModalHostView.DialogRootViewGroup) view;
          continue; // Never traverse the modal's product content.
        }
        if (view instanceof ViewGroup) {
          ViewGroup group = (ViewGroup) view;
          if (group.getChildCount() > 256 - inspected - pending.size()) return false;
          for (int i = 0; i < group.getChildCount(); ++i) pending.addLast(group.getChildAt(i));
        }
      }
      if (root == null || root.getRootView() != decor || root.getId() <= 0 ||
          !(root.getContext() instanceof ThemedReactContext) ||
          ((ThemedReactContext) root.getContext()).getReactApplicationContext() != ctx) return false;
      if (UIManagerHelper.getSurfaceId(root) <= 0) return false;
      SessionRecorderTouchInterceptor tracked = modalInterceptors.get(window);
      if (tracked != null) {
        if (tracked.active && tracked.reactContext == ctx && tracked.activationToken.equals(token) &&
            tracked.applicationViewport == root) return true;
        if (tracked.reactContext != ctx) return false;
        unregisterModal(window, ctx);
      }
      Window.Callback existing = window.getCallback();
      if (existing instanceof SessionRecorderTouchInterceptor stale) {
        if (stale.active && stale.reactContext != ctx) return false;
        stale.deactivate();
        existing = stale.originalCallback;
      }
      SessionRecorderTouchInterceptor interceptor = new SessionRecorderTouchInterceptor(existing, ctx, root, token);
      interceptor.modalWindow = window;
      window.setCallback(interceptor);
      modalInterceptors.put(window, interceptor);
      if (streamOwner != null) streamOwner.abandonContacts();
      return true;
    } catch (RuntimeException unavailable) { return false; }
  }

  public static void unregisterModal(Window window, ReactApplicationContext ctx) {
    SessionRecorderTouchInterceptor interceptor = modalInterceptors.get(window);
    if (interceptor == null || interceptor.reactContext != ctx) return;
    modalInterceptors.remove(window);
    interceptor.deactivate();
    try {
      if (window.getCallback() == interceptor) window.setCallback(interceptor.originalCallback);
    } catch (RuntimeException ignored) { /* Already passive; do not block other cleanup. */ }
  }

  // Called only for an AlertDialog publicly obtained from an explicitly supported DialogFragment.
  // Shares the Activity viewport and stream ownership; does not pretend to be Fabric Modal.
  static boolean registerAlert(Window window, Activity activity, ReactApplicationContext ctx,
      View viewport, String token, BooleanSupplier evidence) {
    if (window == null || activity == null || evidence == null || viewport == null || token == null ||
        installedInterceptor == null || !installedInterceptor.active || installedWindow != activity.getWindow() ||
        installedInterceptor.reactContext != ctx || !token.equals(installedInterceptor.activationToken) ||
        installedInterceptor.applicationViewport != viewport || window == installedWindow) return false;
    try {
      Context owner = window.getContext();
      while (owner instanceof ContextWrapper && owner != activity) {
        Context base = ((ContextWrapper) owner).getBaseContext();
        if (base == owner) break;
        owner = base;
      }
      if (owner != activity) return false;
      SessionRecorderTouchInterceptor existing = modalInterceptors.get(window);
      if (existing != null && existing.active && existing.reactContext == ctx &&
          token.equals(existing.activationToken) && existing.applicationViewport == viewport && existing.alertEvidence != null) return true;
      if (existing != null && existing.reactContext != ctx) return false;
      unregisterModal(window, ctx);
      Window.Callback callback = window.getCallback();
      if (callback instanceof SessionRecorderTouchInterceptor stale) {
        if (stale.active && stale.reactContext != ctx) return false;
        stale.deactivate();
        callback = stale.originalCallback;
      }
      SessionRecorderTouchInterceptor interceptor = new SessionRecorderTouchInterceptor(callback, ctx, viewport, token);
      interceptor.alertEvidence = evidence;
      window.setCallback(interceptor);
      modalInterceptors.put(window, interceptor);
      if (streamOwner != null) streamOwner.abandonContacts();
      return true;
    } catch (RuntimeException unavailable) { return false; }
  }

  static void setAlertPresence(ReactApplicationContext ctx, String token, boolean present) {
    if (installedInterceptor == null || !installedInterceptor.active || installedInterceptor.reactContext != ctx ||
        token == null || !token.equals(installedInterceptor.activationToken)) return;
    if (alertPresent != present && streamOwner != null) streamOwner.abandonContacts();
    alertPresent = present;
  }

  public static void unregister(ReactApplicationContext ctx) {
    if (installedInterceptor != null && installedInterceptor.reactContext == ctx) unregister();
  }

  public static void unregister() {
    alertPresent = false;
    for (Window modal : modalInterceptors.keySet().toArray(new Window[0])) {
      SessionRecorderTouchInterceptor interceptor = modalInterceptors.remove(modal);
      interceptor.deactivate();
      try {
        if (modal.getCallback() == interceptor) modal.setCallback(interceptor.originalCallback);
      } catch (RuntimeException ignored) { /* Continue removing the remaining Windows. */ }
    }
    Window window = installedWindow;
    SessionRecorderTouchInterceptor interceptor = installedInterceptor;
    installedWindow = null;
    installedInterceptor = null;

    if (interceptor == null) return;

    interceptor.deactivate();
    if (window != null && window.getCallback() == interceptor) {
      window.setCallback(interceptor.originalCallback);
    }
  }

  private void deactivate() {
    abandonContacts();
    active = false;
    reactContext = null;
    applicationViewport = null;
    timestampNormalizer = null;
    modalWindow = null;
    modalHost = null;
    alertEvidence = null;
  }

  private void abandonContacts() {
    if (streamOwner == this) streamOwner = null;
    if (ownedPointers.isEmpty()) return;
    Integer[] ids = ownedPointers.toArray(new Integer[0]);
    ownedPointers.clear();
    try {
      if (!active || reactContext == null || !reactContext.hasActiveReactInstance()) return;
      WritableArray pointers = Arguments.createArray();
      for (int id : ids) pointers.pushInt(id);
      WritableMap event = Arguments.createMap();
      event.putString("type", "ownershipAbandoned");
      event.putString("activationToken", activationToken);
      event.putArray("pointerIds", pointers);
      reactContext.getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter.class)
          .emit("SessionRecorderGesture", event);
    } catch (RuntimeException ignored) { /* Cleanup is best effort, never a host failure. */ }
  }

  @Override
  public boolean dispatchTouchEvent(MotionEvent event) {
    try {
      observeTouchEvent(event);
    } catch (RuntimeException ignored) {
      abandonContacts();
    }
    return dispatchToOriginal(event);
  }

  private void observeTouchEvent(MotionEvent event) {
    if (alertPresent && alertEvidence == null) {
      abandonContacts();
      return; // Unknown/covered windows must not target the Fabric content behind an Alert.
    }
    if (alertEvidence != null && (!alertEvidence.getAsBoolean() || !active)) {
      abandonContacts();
      return;
    }
    ReactApplicationContext context = reactContext;
    View viewportRoot = applicationViewport;
    TouchEventTimestampNormalizer normalizer = timestampNormalizer;
    if (!active || context == null || viewportRoot == null || normalizer == null) {
      return;
    }

    int action = event.getActionMasked();
    int actionIndex = event.getActionIndex();
    if (actionIndex < 0 || actionIndex >= event.getPointerCount()) return;

    String type = switch (action) {
      case MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> "pointerDown";
      case MotionEvent.ACTION_MOVE -> "pointerMove";
      case MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> "pointerUp";
      case MotionEvent.ACTION_CANCEL -> "pointerCancel";
      default -> "";
    };

    if (!type.isEmpty() && context.hasActiveReactInstance()) {
      int contentWidth = viewportRoot.getWidth();
      int contentHeight = viewportRoot.getHeight();
      if (contentWidth <= 0 || contentHeight <= 0 || !viewportRoot.isAttachedToWindow()) {
        abandonContacts();
        return;
      }
      if (modalWindow != null) {
        if (action == MotionEvent.ACTION_DOWN || modalHost == null) {
          // RN may emit onExtraWindowCreate during createView(), before its
          // native host is stored in UIManager. Accredit that host at Down,
          // without a retry/timer or observing the mount queue.
          UIManager manager = UIManagerHelper.getUIManager(context, UIManagerType.FABRIC);
          View resolved = manager == null ? null : manager.resolveView(viewportRoot.getId());
          if (!(resolved instanceof ReactModalHostView) || resolved.getContext() != viewportRoot.getContext() ||
              UIManagerHelper.getSurfaceId(resolved) != UIManagerHelper.getSurfaceId(viewportRoot)) {
            abandonContacts(); return;
          }
          ReactModalHostView host = (ReactModalHostView) resolved;
          ViewGroup root = (ViewGroup) viewportRoot;
          if (host.getChildCount() != root.getChildCount() || (root.getChildCount() > 0 &&
              host.getChildAt(0) != root.getChildAt(0))) { abandonContacts(); return; }
          modalHost = host;
        }
        View decor = modalWindow.peekDecorView();
        if (modalHost == null || !modalHost.isAttachedToWindow() || viewportRoot.getId() != modalHost.getId() ||
            decor == null || viewportRoot.getRootView() != decor || !viewportRoot.isShown() ||
            viewportRoot.getWindowToken() == null || viewportRoot.getWindowToken() != decor.getWindowToken() ||
            !decor.getMatrix().isIdentity()) {
          abandonContacts();
          return;
        }
        // This adapter admits the standard untransformed Dialog root only.
        for (View view = viewportRoot; view != decor;) {
          if (!view.getMatrix().isIdentity() || !(view.getParent() instanceof View)) {
            abandonContacts(); return;
          }
          view = (View) view.getParent();
        }
      }
      if (action == MotionEvent.ACTION_DOWN) {
        if (streamOwner != null && streamOwner != this) streamOwner.abandonContacts();
        if (!ownedPointers.isEmpty()) abandonContacts();
        streamOwner = this;
        ownedPointers.add(event.getPointerId(actionIndex));
      } else if (streamOwner != this || ownedPointers.isEmpty()) {
        return; // No observed Down in this Window/activation; do not invent a contact.
      }
      if (action == MotionEvent.ACTION_POINTER_DOWN) ownedPointers.add(event.getPointerId(actionIndex));
      for (int i = 0; i < event.getPointerCount(); ++i) {
        if (!ownedPointers.contains(event.getPointerId(i))) { abandonContacts(); return; }
      }

      WritableArray pointers = Arguments.createArray();

      viewportRoot.getLocationOnScreen(contentOriginOnScreen);

      // Pointer 0 provides the window-to-screen translation. Applying the
      // same translation to every contact keeps the MotionEvent in one screen
      // space before subtracting the content root origin. A Modal uses its
      // DialogRootViewGroup origin, matching Fabric's Modal RootNodeKind space,
      // not the Activity or Dialog decor origin/system-bar insets.
      float offsetX = event.getRawX() - event.getX();
      float offsetY = event.getRawY() - event.getY();

      for (int i = 0; i < event.getPointerCount(); i++) {
        WritableMap pointer = Arguments.createMap();
        float applicationX = event.getX(i) + offsetX - contentOriginOnScreen[0];
        float applicationY = event.getY(i) + offsetY - contentOriginOnScreen[1];

        pointer.putInt("pointerId", event.getPointerId(i));
        pointer.putDouble("x", PixelUtil.toDIPFromPixel(applicationX));
        pointer.putDouble("y", PixelUtil.toDIPFromPixel(applicationY));
        pointer.putBoolean("isActive", i == actionIndex);
        pointers.pushMap(pointer);
      }

      WritableMap viewport = Arguments.createMap();
      viewport.putDouble("left", 0);
      viewport.putDouble("top", 0);
      viewport.putDouble("width", PixelUtil.toDIPFromPixel(contentWidth));
      viewport.putDouble("height", PixelUtil.toDIPFromPixel(contentHeight));

      WritableMap params = Arguments.createMap();
      long timestamp = normalizer.toEpochMillis(
        event.getEventTime(),
        System.currentTimeMillis(),
        SystemClock.uptimeMillis(),
        action == MotionEvent.ACTION_DOWN,
        action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL
      );

      params.putString("type", type);
      params.putString("activationToken", activationToken);
      params.putLong("timestamp", timestamp);
      params.putInt("actionPointerId", event.getPointerId(actionIndex));
      params.putInt("pointerCount", event.getPointerCount());
      params.putArray("pointers", pointers);
      params.putMap("viewport", viewport);

      if (active && context.hasActiveReactInstance()) {
        context
          .getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter.class)
          .emit("SessionRecorderGesture", params);
      }
      if (action == MotionEvent.ACTION_CANCEL) ownedPointers.clear();
      else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP)
        ownedPointers.remove(event.getPointerId(actionIndex));
      if (ownedPointers.isEmpty() && streamOwner == this) streamOwner = null;
    }
  }

  private boolean dispatchToOriginal(MotionEvent event) {
    return originalCallback != null && originalCallback.dispatchTouchEvent(event);
  }

  @Override
  public boolean dispatchGenericMotionEvent(MotionEvent motionEvent) {
    return originalCallback != null && originalCallback.dispatchGenericMotionEvent(motionEvent);
  }

  @Override
  public boolean dispatchKeyEvent(KeyEvent keyEvent) {
    return originalCallback != null && originalCallback.dispatchKeyEvent(keyEvent);
  }

  @Override
  public boolean dispatchKeyShortcutEvent(KeyEvent keyEvent) {
    return originalCallback != null && originalCallback.dispatchKeyShortcutEvent(keyEvent);
  }

  @Override
  public boolean dispatchPopulateAccessibilityEvent(AccessibilityEvent accessibilityEvent) {
    return originalCallback != null && originalCallback.dispatchPopulateAccessibilityEvent(accessibilityEvent);
  }

  @Override
  public boolean dispatchTrackballEvent(MotionEvent motionEvent) {
    return originalCallback != null && originalCallback.dispatchTrackballEvent(motionEvent);
  }

  @Override
  public void onActionModeFinished(ActionMode actionMode) {
    if (originalCallback != null) originalCallback.onActionModeFinished(actionMode);
  }

  @Override
  public void onActionModeStarted(ActionMode actionMode) {
    if (originalCallback != null) originalCallback.onActionModeStarted(actionMode);
  }

  @Override
  public void onAttachedToWindow() {
    if (originalCallback != null) originalCallback.onAttachedToWindow();
  }

  @Override
  public void onContentChanged() {
    if (originalCallback != null) originalCallback.onContentChanged();
  }

  @Override
  public boolean onCreatePanelMenu(int i, @NonNull Menu menu) {
    return originalCallback != null && originalCallback.onCreatePanelMenu(i, menu);
  }

  @Nullable
  @Override
  public View onCreatePanelView(int i) {
    return originalCallback != null ? originalCallback.onCreatePanelView(i) : null;
  }

  @Override
  public void onDetachedFromWindow() {
    if (originalCallback != null) originalCallback.onDetachedFromWindow();
  }

  @Override
  public boolean onMenuItemSelected(int i, @NonNull MenuItem menuItem) {
    return originalCallback != null && originalCallback.onMenuItemSelected(i, menuItem);
  }

  @Override
  public boolean onMenuOpened(int i, @NonNull Menu menu) {
    return originalCallback != null && originalCallback.onMenuOpened(i, menu);
  }

  @Override
  public void onPanelClosed(int i, @NonNull Menu menu) {
    if (originalCallback != null) originalCallback.onPanelClosed(i, menu);
  }

  @Override
  public boolean onPreparePanel(int i, @Nullable View view, @NonNull Menu menu) {
    return originalCallback != null && originalCallback.onPreparePanel(i, view, menu);
  }

  @Override
  public void onProvideKeyboardShortcuts(List<KeyboardShortcutGroup> data, @Nullable Menu menu, int deviceId) {
    if (originalCallback != null) originalCallback.onProvideKeyboardShortcuts(data, menu, deviceId);
  }

  @Override
  public boolean onSearchRequested() {
    return originalCallback != null && originalCallback.onSearchRequested();
  }

  @Override
  public boolean onSearchRequested(SearchEvent searchEvent) {
    return originalCallback != null && originalCallback.onSearchRequested(searchEvent);
  }

  @Override
  public void onWindowAttributesChanged(WindowManager.LayoutParams layoutParams) {
    if (originalCallback != null) originalCallback.onWindowAttributesChanged(layoutParams);
  }

  @Override
  public void onWindowFocusChanged(boolean b) {
    if (originalCallback != null) originalCallback.onWindowFocusChanged(b);
  }

  @Nullable
  @Override
  public ActionMode onWindowStartingActionMode(ActionMode.Callback callback) {
    return originalCallback != null ? originalCallback.onWindowStartingActionMode(callback) : null;
  }

  @Nullable
  @Override
  public ActionMode onWindowStartingActionMode(ActionMode.Callback callback, int i) {
    return originalCallback != null ? originalCallback.onWindowStartingActionMode(callback, i) : null;
  }
}
