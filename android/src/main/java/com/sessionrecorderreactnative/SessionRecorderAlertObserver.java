package com.sessionrecorderreactnative;

import android.app.Activity;
import android.app.DatePickerDialog;
import android.app.Dialog;
import android.app.TimePickerDialog;
import android.content.DialogInterface;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.Window;
import android.view.animation.Animation;
import android.widget.Button;
import androidx.fragment.app.DialogFragment;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.FragmentActivity;
import androidx.fragment.app.FragmentManager;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.UIManager;
import com.facebook.react.uimanager.PixelUtil;
import com.facebook.react.uimanager.UIManagerHelper;
import com.facebook.react.uimanager.common.UIManagerType;
import java.util.Arrays;

// Exact standard Alert/DatePicker/TimePicker fragments only. No window/view enumeration or content reads.
@androidx.annotation.Keep
final class SessionRecorderAlertObserver {
  private static final String ALERT_FRAGMENT = "com.facebook.react.modules.dialog.AlertFragment";
  private static final String DATE_PICKER_FRAGMENT = "com.reactcommunity.rndatetimepicker.RNDatePickerDialogFragment";
  private static final String TIME_PICKER_FRAGMENT = "com.reactcommunity.rndatetimepicker.RNTimePickerDialogFragment";
  private final ReactApplicationContext context;
  private FragmentActivity activity;
  private FragmentManager fragments;
  private String activation;
  private Record record;
  private boolean refreshing, accredited;
  private long nextInstance;
  private final FragmentManager.FragmentLifecycleCallbacks lifecycle = new FragmentManager.FragmentLifecycleCallbacks() {
    @Override public void onFragmentStarted(FragmentManager manager, Fragment fragment) { changed(fragment); }
    @Override public void onFragmentStopped(FragmentManager manager, Fragment fragment) { changed(fragment); }
    @Override public void onFragmentDestroyed(FragmentManager manager, Fragment fragment) { changed(fragment); }
    private void changed(Fragment fragment) {
      String name = fragment.getClass().getName();
      if (ALERT_FRAGMENT.equals(name) || DATE_PICKER_FRAGMENT.equals(name) || TIME_PICKER_FRAGMENT.equals(name)) refresh();
    }
  };

  SessionRecorderAlertObserver(ReactApplicationContext context) { this.context = context; }

  void start(String token) {
    Activity current = context.getCurrentActivity();
    if (token.equals(activation) && current == activity) { refresh(); return; }
    stop();
    if (!(current instanceof FragmentActivity)) return;
    activity = (FragmentActivity) current;
    activation = token;
    try {
      fragments = activity.getSupportFragmentManager();
      fragments.registerFragmentLifecycleCallbacks(lifecycle, false);
      refresh(); // Existing standard Alerts are discovered on activation, too.
    } catch (RuntimeException unavailable) { stop(); }
  }

  void stop() {
    SessionRecorderTouchInterceptor.setAlertPresence(context, activation, false);
    activation = null;
    accredited = false;
    if (record != null) { record.remove(); record = null; }
    if (fragments != null) {
      try { fragments.unregisterFragmentLifecycleCallbacks(lifecycle); } catch (RuntimeException ignored) {}
    }
    fragments = null;
    activity = null;
  }

  void refresh() {
    if (activation == null || fragments == null || refreshing) return;
    refreshing = true;
    accredited = false;
    try {
      Dialog candidate = null;
      boolean candidateDatePicker = false;
      boolean candidateTimePicker = false;
      int count = 0;
      if (activity == context.getCurrentActivity() && !activity.isFinishing() && !activity.isDestroyed()) {
        // RN DialogModule uses this Activity's supportFragmentManager. No child-manager traversal.
        for (Fragment fragment : fragments.getFragments()) {
          String name = fragment.getClass().getName();
          boolean datePicker = DATE_PICKER_FRAGMENT.equals(name);
          boolean timePicker = TIME_PICKER_FRAGMENT.equals(name);
          if ((!ALERT_FRAGMENT.equals(name) && !datePicker && !timePicker) || !(fragment instanceof DialogFragment)) continue;
          Dialog dialog = ((DialogFragment) fragment).getDialog(); // Public AndroidX API, not RN Modal's testing getter.
          if (dialog != null && dialog.isShowing()) {
            candidate = dialog; candidateDatePicker = datePicker; candidateTimePicker = timePicker; ++count;
          }
        }
      }
      if (count != 1 || record == null || record.dialog != candidate || record.window != candidate.getWindow() ||
          record.datePicker != candidateDatePicker || record.timePicker != candidateTimePicker) {
        if (record != null) { record.remove(); record = null; }
        if (count == 1 && (candidateDatePicker ? candidate instanceof DatePickerDialog :
            candidateTimePicker ? candidate instanceof TimePickerDialog :
            candidate instanceof androidx.appcompat.app.AlertDialog || candidate instanceof android.app.AlertDialog)) {
          Window window = candidate.getWindow();
          View viewport = activity.findViewById(android.R.id.content);
          View decor = window == null ? null : window.peekDecorView();
          if (viewport != null && decor != null)
            record = new Record(candidate, window, decor, viewport, candidateDatePicker, candidateTimePicker);
        }
      }
      SessionRecorderTouchInterceptor.setAlertPresence(context, activation, count != 0);
      accredited = nativeRefresh(activation, count != 0);
      Record current = record;
      if (current != null) {
        if (current.geometryChanged || current.geometry == null) SessionRecorderTouchInterceptor.unregisterModal(current.window, context);
        current.geometryChanged = false;
        if (current.geometry != null) {
          final String token = activation;
          SessionRecorderTouchInterceptor.registerAlert(current.window, activity, context, current.viewport, token, () -> {
            if (!token.equals(activation) || record != current) return false;
            refresh(); // Event-driven remeasurement before any MotionEvent is observed.
            return accredited && record == current && token.equals(activation);
          });
        }
      }
    } catch (RuntimeException unavailable) {
      if (record != null) { record.remove(); record = null; }
      // Publish an unknown modal barrier rather than reusing the underlying Fabric geometry.
      SessionRecorderTouchInterceptor.setAlertPresence(context, activation, true);
      try { nativeRefresh(activation, true); } catch (RuntimeException ignored) {}
    } finally { refreshing = false; }
  }

  @SuppressWarnings("unused")
  @androidx.annotation.Keep
  private double[] probe(int rootTag) {
    Record current = record;
    if (current == null || activity != context.getCurrentActivity()) return null;
    try {
      UIManager manager = UIManagerHelper.getUIManager(context, UIManagerType.FABRIC);
      View root = manager == null ? null : manager.resolveView(rootTag);
      View viewport = current.viewport;
      if (root == null || !root.isAttachedToWindow() || !viewport.isAttachedToWindow() ||
          root.getRootView() != viewport.getRootView() || root.getWindowToken() != viewport.getWindowToken() ||
          !current.dialog.isShowing() || current.dialog.getWindow() != current.window ||
          current.window.peekDecorView() != current.decor || !current.decor.hasWindowFocus()) return current.update(null);
      // MotionEvents use a translation-only screen→Activity conversion. Reject a
      // transformed Activity baseline rather than infer an inverse projection.
      for (View view = viewport; view != null; view = view.getParent() instanceof View ? (View) view.getParent() : null)
        if (!view.getMatrix().isIdentity()) return current.update(null);
      RectF rootRect = SessionRecorderScreenVisibilityObserver.visibleHeaderRect(root, viewport);
      // Match the existing Activity stream's logical viewport, never select a Surface by geometry.
      if (rootRect == null || rootRect.left != 0 || rootRect.top != 0 ||
          rootRect.width() != viewport.getWidth() || rootRect.height() != viewport.getHeight() ||
          root.getWidth() != viewport.getWidth() || root.getHeight() != viewport.getHeight()) return current.update(null);
      // Preserve Alert/DatePicker shapes. TimePicker adds only kind: no public selector getter.
      double[] values = new double[current.datePicker ? 28 : current.timePicker ? 23 : 22];
      values[0] = PixelUtil.toDIPFromPixel(viewport.getWidth());
      values[1] = PixelUtil.toDIPFromPixel(viewport.getHeight());
      if (!current.measure(current.decor, current.instance, values, 2)) return current.update(null);
      int[] roles = {DialogInterface.BUTTON_POSITIVE, DialogInterface.BUTTON_NEGATIVE, DialogInterface.BUTTON_NEUTRAL};
      for (int i = 0; i < roles.length; ++i) {
        Button button = current.dialog instanceof androidx.appcompat.app.AlertDialog
            ? ((androidx.appcompat.app.AlertDialog) current.dialog).getButton(roles[i])
            : ((android.app.AlertDialog) current.dialog).getButton(roles[i]);
        if (current.buttons[i] != button) { current.buttons[i] = button; current.buttonInstances[i] = ++nextInstance; }
        if (button != null) current.measure(button, current.buttonInstances[i], values, 7 + 5 * i);
      }
      if (current.datePicker) {
        values[22] = 1;
        try {
          View picker = ((DatePickerDialog) current.dialog).getDatePicker();
          if (current.picker != picker) { current.picker = picker; current.pickerInstance = ++nextInstance; }
          if (picker != null) current.measure(picker, current.pickerInstance, values, 23);
        } catch (RuntimeException unavailable) {
          // Only the selector is unknown; keep the independently measured dialog/buttons.
          current.picker = null;
          current.pickerInstance = 0;
        }
      }
      if (current.timePicker) values[22] = 2;
      return current.update(values);
    } catch (RuntimeException unavailable) { return current.update(null); }
  }

  private final class Record implements ViewTreeObserver.OnGlobalLayoutListener,
      ViewTreeObserver.OnWindowFocusChangeListener, View.OnAttachStateChangeListener {
    final Dialog dialog;
    final Window window;
    final View decor, viewport;
    final boolean datePicker, timePicker;
    final long instance = ++nextInstance;
    final Button[] buttons = new Button[3];
    final long[] buttonInstances = new long[3];
    View picker;
    long pickerInstance;
    final ViewTreeObserver decorObserver, viewportObserver;
    double[] geometry;
    boolean geometryChanged;
    Record(Dialog dialog, Window window, View decor, View viewport, boolean datePicker, boolean timePicker) {
      this.dialog = dialog; this.window = window; this.decor = decor; this.viewport = viewport;
      this.datePicker = datePicker;
      this.timePicker = timePicker;
      decorObserver = decor.getViewTreeObserver();
      viewportObserver = viewport.getViewTreeObserver();
      decorObserver.addOnGlobalLayoutListener(this);
      decorObserver.addOnWindowFocusChangeListener(this);
      viewportObserver.addOnGlobalLayoutListener(this);
      decor.addOnAttachStateChangeListener(this);
      viewport.addOnAttachStateChangeListener(this);
    }
    double[] update(double[] next) {
      geometryChanged |= !Arrays.equals(geometry, next);
      geometry = next;
      return next;
    }
    boolean measure(View view, long identity, double[] values, int offset) {
      try {
        if (!decor.getMatrix().isIdentity()) return false;
        RectF rect = visibleAlertRect(view);
        if (rect == null) return false;
        int[] dialogOrigin = new int[2], viewportOrigin = new int[2];
        decor.getLocationOnScreen(dialogOrigin);
        viewport.getLocationOnScreen(viewportOrigin);
        rect.offset(dialogOrigin[0] - viewportOrigin[0], dialogOrigin[1] - viewportOrigin[1]);
        if (!rect.intersect(0, 0, viewport.getWidth(), viewport.getHeight())) return false;
        values[offset] = identity;
        values[offset + 1] = PixelUtil.toDIPFromPixel(rect.left);
        values[offset + 2] = PixelUtil.toDIPFromPixel(rect.top);
        values[offset + 3] = PixelUtil.toDIPFromPixel(rect.width());
        values[offset + 4] = PixelUtil.toDIPFromPixel(rect.height());
        return true;
      } catch (RuntimeException unavailable) {
        return false; // An unmeasurable button must not discard the measured Dialog.
      }
    }
    private Matrix toDecor(View view) {
      Matrix matrix = new Matrix();
      while (view != decor) {
        if (!(view.getParent() instanceof View)) return null;
        View parent = (View) view.getParent();
        matrix.postConcat(view.getMatrix());
        matrix.postTranslate(view.getLeft() - parent.getScrollX(), view.getTop() - parent.getScrollY());
        view = parent;
      }
      return matrix.isAffine() && matrix.rectStaysRect() ? matrix : null;
    }
    private RectF visibleAlertRect(View element) {
      if (!decor.isAttachedToWindow() || !element.isAttachedToWindow() || !element.isShown() ||
          element.getWindowToken() != decor.getWindowToken() || !element.isLaidOut() ||
          element.isLayoutRequested()) return null;
      Matrix matrix = toDecor(element);
      if (matrix == null) return null;
      RectF rect = new RectF(0, 0, element.getWidth(), element.getHeight());
      matrix.mapRect(rect);
      for (View view = element; ; view = (View) view.getParent()) {
        Animation animation = view.getAnimation();
        if (view.getVisibility() != View.VISIBLE || view.getAlpha() <= 0 || view.isLayoutRequested() ||
            (view != decor && view.getClipToOutline()) || (animation != null && !animation.hasEnded())) return null;
        // Only this accredited Alert decor may contribute an outline envelope.
        // Its native rectangle is not a promise of hit-test precision at rounded corners.
        Rect explicitClip = view.getClipBounds();
        boolean paddingClip = view instanceof ViewGroup && ((ViewGroup) view).getClipToPadding() &&
            (view.getPaddingLeft() != 0 || view.getPaddingTop() != 0 ||
             view.getPaddingRight() != 0 || view.getPaddingBottom() != 0);
        boolean clips = view == decor || (view != element && view instanceof ViewGroup &&
            (((ViewGroup) view).getClipChildren() || paddingClip));
        if (clips || explicitClip != null) {
          RectF clip = clips ? new RectF(0, 0, view.getWidth(), view.getHeight()) : new RectF(explicitClip);
          if (clips && paddingClip)
            clip.set(view.getPaddingLeft(), view.getPaddingTop(), view.getWidth() - view.getPaddingRight(),
                view.getHeight() - view.getPaddingBottom());
          if (explicitClip != null && !clip.intersect(new RectF(explicitClip))) return null;
          Matrix ancestorMatrix = toDecor(view);
          if (ancestorMatrix == null) return null;
          ancestorMatrix.mapRect(clip);
          if (!rect.intersect(clip)) return null;
        }
        if (view == decor) break;
      }
      Rect visible = new Rect();
      if (!decor.getLocalVisibleRect(visible) || !rect.intersect(new RectF(visible)) ||
          !Float.isFinite(rect.left) || !Float.isFinite(rect.top) ||
          !Float.isFinite(rect.right) || !Float.isFinite(rect.bottom) || rect.isEmpty()) return null;
      return rect;
    }
    public void onGlobalLayout() { if (record == this) refresh(); }
    public void onWindowFocusChanged(boolean focused) { if (record == this) refresh(); }
    public void onViewAttachedToWindow(View view) { if (record == this) refresh(); }
    public void onViewDetachedFromWindow(View view) {
      if (record != this) return;
      SessionRecorderTouchInterceptor.unregisterModal(window, context);
      refresh();
    }
    void remove() {
      SessionRecorderTouchInterceptor.unregisterModal(window, context);
      if (decorObserver.isAlive()) decorObserver.removeOnGlobalLayoutListener(this);
      if (decorObserver.isAlive()) decorObserver.removeOnWindowFocusChangeListener(this);
      if (viewportObserver.isAlive()) viewportObserver.removeOnGlobalLayoutListener(this);
      // A floating observer may have transferred its listeners on attachment.
      if (decor.getViewTreeObserver().isAlive()) decor.getViewTreeObserver().removeOnGlobalLayoutListener(this);
      if (decor.getViewTreeObserver().isAlive()) decor.getViewTreeObserver().removeOnWindowFocusChangeListener(this);
      if (viewport.getViewTreeObserver().isAlive()) viewport.getViewTreeObserver().removeOnGlobalLayoutListener(this);
      decor.removeOnAttachStateChangeListener(this);
      viewport.removeOnAttachStateChangeListener(this);
      geometry = null;
    }
  }

  private native boolean nativeRefresh(String token, boolean present);
}
