package com.sessionrecorderreactnative;

import android.os.Handler;
import android.os.Looper;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;
import android.text.TextUtils;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.animation.Animation;
import android.widget.ImageButton;
import android.widget.TextView;
import androidx.appcompat.widget.Toolbar;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.FragmentManager;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.UIManager;
import com.facebook.react.bridge.UIManagerListener;
import com.facebook.react.uimanager.UIManagerHelper;
import com.facebook.react.uimanager.PixelUtil;
import com.facebook.react.uimanager.common.UIManagerType;
import com.facebook.react.uimanager.events.EventDispatcher;
import com.facebook.react.uimanager.events.EventDispatcherListener;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

// UI-thread evidence only. Tags are candidates from Fabric, never navigation order.
@androidx.annotation.Keep
final class SessionRecorderScreenVisibilityObserver implements UIManagerListener {
  private final ReactApplicationContext context;
  private final SessionRecorderAlertObserver alerts;
  private final Handler main = new Handler(Looper.getMainLooper());
  private final Map<Integer, Record> records = new HashMap<>();
  private final Set<Integer> seen = new HashSet<>();
  private UIManager manager;
  private EventDispatcher dispatcher;
  private String activation;
  private long nextInstance;
  private long nextBackInstance;
  private long nextTitleInstance;
  private boolean refreshing;
  private final EventDispatcherListener transitions = event -> {
    if ("topFinishTransitioning".equals(event.getEventName())) queueRefresh();
  };

  SessionRecorderScreenVisibilityObserver(ReactApplicationContext context) {
    this.context = context;
    this.alerts = new SessionRecorderAlertObserver(context);
  }
  void start(String token) {
    if (token.equals(activation)) { alerts.start(token); refresh(); return; }
    stop();
    activation = token;
    nativeLifecycle(token, true);
    manager = UIManagerHelper.getUIManager(context, UIManagerType.FABRIC);
    if (manager != null) manager.addUIManagerEventListener(this);
    dispatcher = UIManagerHelper.getEventDispatcher(context, UIManagerType.FABRIC);
    if (dispatcher != null) dispatcher.addListener(transitions);
    alerts.start(token);
    refresh();
  }
  void stop() {
    String token = activation;
    activation = null; // Revoke before listener cleanup.
    alerts.stop();
    if (token != null) nativeLifecycle(token, false);
    if (dispatcher != null) dispatcher.removeListener(transitions);
    dispatcher = null;
    if (manager != null) manager.removeUIManagerEventListener(this);
    manager = null;
    for (Record record : records.values()) record.remove();
    records.clear();
  }
  private void queueRefresh() {
    final String token = activation;
    if (token == null) return;
    main.post(() -> { if (token.equals(activation)) refresh(); });
  }
  private void refresh() {
    if (activation == null || manager == null || refreshing) return;
    refreshing = true;
    seen.clear();
    try { nativeRefresh(activation); alerts.refresh(); }
    catch (RuntimeException ignored) { /* Unknown evidence must never become a negative. */ }
    finally {
      refreshing = false;
      for (Integer tag : new HashSet<>(records.keySet()))
        if (!seen.contains(tag)) records.remove(tag).remove();
    }
  }
  // Called synchronously by nativeRefresh on UI, outside Fabric registry locks.
  @SuppressWarnings("unused")
  @androidx.annotation.Keep
  private long probe(int tag, int rootTag) {
    try {
      View screen = manager.resolveView(tag), root = manager.resolveView(rootTag);
      if (screen == null || root == null || screen.getId() != tag) return 0;
      Record record = records.get(tag);
      if (record != null && (record.view != screen || record.root != root)) {
        record.remove(); records.remove(tag); record = null;
      }
      if (record == null) { record = new Record(screen, root); records.put(tag, record); }
      seen.add(tag);
      record.watchDraw();
      if (!root.isAttachedToWindow()) {
        record.setBack(null);
        record.setTitle(null);
        return record.instance << 2; // No trustworthy Surface window.
      }
      if (screen.isAttachedToWindow()) {
        View ancestor = screen;
        while (ancestor != null && ancestor != root)
          ancestor = ancestor.getParent() instanceof View ? (View) ancestor.getParent() : null;
        boolean inWindow = ancestor == root && screen.getWindowToken() == root.getWindowToken();
        record.setBack(inWindow ? record.readBack() : null);
        record.setTitle(inWindow ? record.readTitle() : null);
        return (record.instance << 2) | (inWindow ? 1 : 0);
      }
      record.setBack(null);
      record.setTitle(null);
      // A removing fragment may still be drawn by a disappearing-view animation.
      for (View view = screen; view != null;) {
        Animation animation = view.getAnimation();
        if (animation != null && !animation.hasEnded()) return record.instance << 2;
        view = view.getParent() instanceof View ? (View) view.getParent() : null;
      }
      return (record.instance << 2) | 2; // Definitively detached, not merely non-top/STARTED.
    } catch (RuntimeException unavailable) {
      Record record = records.get(tag);
      if (record != null) { record.setBack(null); record.setTitle(null); }
      return 0;
    }
  }
  @SuppressWarnings("unused")
  @androidx.annotation.Keep
  private double[] probeBack(int tag) {
    Record record = records.get(tag);
    if (record == null || record.back == null) return null;
    RectF rect = record.back.rect;
    return new double[] {record.backInstance, PixelUtil.toDIPFromPixel(rect.left),
        PixelUtil.toDIPFromPixel(rect.top), PixelUtil.toDIPFromPixel(rect.width()),
        PixelUtil.toDIPFromPixel(rect.height())};
  }
  @SuppressWarnings("unused")
  @androidx.annotation.Keep
  private double[] probeTitle(int tag) {
    Record record = records.get(tag);
    if (record == null || record.title == null) return null;
    RectF rect = record.title.rect;
    return new double[] {record.titleInstance, PixelUtil.toDIPFromPixel(rect.left),
        PixelUtil.toDIPFromPixel(rect.top), PixelUtil.toDIPFromPixel(rect.width()),
        PixelUtil.toDIPFromPixel(rect.height())};
  }
  private static final class BackEvidence {
    final Toolbar toolbar;
    final ImageButton button;
    final Drawable icon;
    final RectF rect;
    BackEvidence(Toolbar toolbar, ImageButton button, Drawable icon, RectF rect) {
      this.toolbar = toolbar; this.button = button; this.icon = icon; this.rect = rect;
    }
  }
  private static boolean sameBack(BackEvidence a, BackEvidence b) {
    return a == b || (a != null && b != null && a.toolbar == b.toolbar &&
        a.button == b.button && a.icon == b.icon && a.rect.equals(b.rect));
  }
  private static final class TitleEvidence {
    final Toolbar toolbar;
    final TextView view;
    final RectF rect;
    TitleEvidence(Toolbar toolbar, TextView view, RectF rect) {
      this.toolbar = toolbar; this.view = view; this.rect = rect;
    }
  }
  private static boolean sameTitle(TitleEvidence a, TitleEvidence b) {
    return a == b || (a != null && b != null && a.toolbar == b.toolbar &&
        a.view == b.view && a.rect.equals(b.rect));
  }
  // Public View matrices only. Reject non-rectangular projections rather than
  // estimating a control's visible box during rotation/perspective/legacy animation.
  private static Matrix toRoot(View view, View root) {
    Matrix matrix = new Matrix();
    while (view != root) {
      if (!(view.getParent() instanceof View)) return null;
      View parent = (View) view.getParent();
      matrix.postConcat(view.getMatrix());
      matrix.postTranslate(view.getLeft() - parent.getScrollX(), view.getTop() - parent.getScrollY());
      view = parent;
    }
    return matrix.isAffine() && matrix.rectStaysRect() ? matrix : null;
  }
  private static RectF visibleBackRect(View button, View root) {
    if (!button.isEnabled() || !button.isClickable()) return null;
    return visibleHeaderRect(button, root);
  }
  static RectF visibleHeaderRect(View button, View root) {
    if (!root.isAttachedToWindow() || !button.isAttachedToWindow() || !button.isShown() ||
        button.getWindowToken() != root.getWindowToken() || !button.isLaidOut() ||
        button.isLayoutRequested()) return null;
    Matrix matrix = toRoot(button, root);
    if (matrix == null) return null;
    RectF rect = new RectF(0, 0, button.getWidth(), button.getHeight());
    matrix.mapRect(rect);
    for (View view = button; ; view = (View) view.getParent()) {
      Animation animation = view.getAnimation();
      if (view.getVisibility() != View.VISIBLE || view.getAlpha() <= 0 || view.isLayoutRequested() ||
          view.getClipToOutline() || (animation != null && !animation.hasEnded())) return null;
      Rect explicitClip = view.getClipBounds();
      boolean paddingClip = view instanceof ViewGroup && ((ViewGroup) view).getClipToPadding() &&
          (view.getPaddingLeft() != 0 || view.getPaddingTop() != 0 ||
           view.getPaddingRight() != 0 || view.getPaddingBottom() != 0);
      boolean clips = view == root || (view != button && view instanceof ViewGroup &&
          (((ViewGroup) view).getClipChildren() || paddingClip));
      if (clips || explicitClip != null) {
        RectF clip = clips ? new RectF(0, 0, view.getWidth(), view.getHeight()) : new RectF(explicitClip);
        if (clips && paddingClip)
          clip.set(view.getPaddingLeft(), view.getPaddingTop(), view.getWidth() - view.getPaddingRight(),
              view.getHeight() - view.getPaddingBottom());
        if (explicitClip != null && !clip.intersect(new RectF(explicitClip))) return null;
        Matrix ancestorMatrix = toRoot(view, root);
        if (ancestorMatrix == null) return null;
        ancestorMatrix.mapRect(clip);
        if (!rect.intersect(clip)) return null;
      }
      if (view == root) break;
    }
    Rect rootVisible = new Rect();
    if (!root.getLocalVisibleRect(rootVisible) || !rect.intersect(new RectF(rootVisible)) ||
        !Float.isFinite(rect.left) || !Float.isFinite(rect.top) ||
        !Float.isFinite(rect.right) || !Float.isFinite(rect.bottom) || rect.isEmpty()) return null;
    return rect;
  }
  private final class Record implements View.OnAttachStateChangeListener, ViewTreeObserver.OnPreDrawListener {
    final View view, root;
    final long instance = ++nextInstance;
    BackEvidence back;
    long backInstance;
    TitleEvidence title;
    long titleInstance;
    ViewTreeObserver drawObserver;
    Record(View view, View root) { this.view = view; this.root = root; view.addOnAttachStateChangeListener(this); }
    void watchDraw() {
      ViewTreeObserver observer = root.getViewTreeObserver();
      if (observer == drawObserver) return;
      if (drawObserver != null && drawObserver.isAlive()) drawObserver.removeOnPreDrawListener(this);
      drawObserver = observer;
      if (observer.isAlive()) {
        // Android may transfer listeners from the floating observer on attach.
        observer.removeOnPreDrawListener(this);
        observer.addOnPreDrawListener(this);
      }
    }
    void setBack(BackEvidence next) {
      if (next != null && (back == null || back.toolbar != next.toolbar || back.button != next.button || back.icon != next.icon))
        backInstance = ++nextBackInstance;
      back = next;
    }
    void setTitle(TitleEvidence next) {
      if (next != null && (title == null || title.toolbar != next.toolbar || title.view != next.view))
        titleInstance = ++nextTitleInstance;
      title = next;
    }
    Toolbar readToolbar() {
        if (Looper.myLooper() != Looper.getMainLooper() || !view.isAttachedToWindow() || !view.isShown()) return null;
        Fragment fragment = FragmentManager.findFragment(view);
        if (!"com.swmansion.rnscreens.ScreenStackFragment".equals(fragment.getClass().getName()) ||
            !fragment.isAdded() || fragment.isRemoving() || fragment.isHidden() ||
            !(fragment.getView() instanceof ViewGroup) || view.getParent() != fragment.getView()) return null;
        // The standard ScreenStackFragment owns Screen + AppBar as siblings.
        // Search only its header branch, never the screen content or nested stacks.
        ViewGroup owner = (ViewGroup) fragment.getView();
        Toolbar toolbar = null;
        for (int i = 0; i < owner.getChildCount(); ++i) {
          View branch = owner.getChildAt(i);
          if (branch == view || !(branch instanceof ViewGroup)) continue;
          ViewGroup group = (ViewGroup) branch;
          for (int j = 0; j < group.getChildCount(); ++j) {
            View child = group.getChildAt(j);
            if (child instanceof Toolbar) {
              if (toolbar != null) return null;
              toolbar = (Toolbar) child;
            }
          }
        }
        return toolbar != null && toolbar.isAttachedToWindow() && toolbar.isShown() ? toolbar : null;
    }
    BackEvidence readBack() {
      try {
        Toolbar toolbar = readToolbar();
        if (toolbar == null) return null;
        Drawable icon = toolbar.getNavigationIcon();
        if (icon == null) return null;
        ImageButton button = null;
        for (int i = 0; i < toolbar.getChildCount(); ++i) {
          View child = toolbar.getChildAt(i);
          if (child instanceof ImageButton && ((ImageButton) child).getDrawable() == icon) {
            if (button != null) return null;
            button = (ImageButton) child;
          }
        }
        if (button == null) return null;
        RectF rect = visibleBackRect(button, root);
        return rect == null ? null : new BackEvidence(toolbar, button, icon, rect);
      } catch (RuntimeException unavailable) { return null; }
    }
    TitleEvidence readTitle() {
      try {
        Toolbar toolbar = readToolbar();
        if (toolbar == null || TextUtils.isEmpty(toolbar.getTitle())) return null;
        TextView titleView = null;
        for (int i = 0; i < toolbar.getChildCount(); ++i) {
          View child = toolbar.getChildAt(i);
          if (child instanceof TextView && TextUtils.equals(((TextView) child).getText(), toolbar.getTitle())) {
            if (titleView != null) return null;
            titleView = (TextView) child;
          }
        }
        // The standard AppCompat title is untagged. Fabric-mounted TextViews
        // have React tags; never emit a second native node for those Views.
        if (titleView == null || titleView.getId() != View.NO_ID) return null;
        RectF rect = visibleHeaderRect(titleView, root);
        return rect == null ? null : new TitleEvidence(toolbar, titleView, rect);
      } catch (RuntimeException unavailable) { return null; }
    }
    public boolean onPreDraw() {
      // Render-driven observation, no timer/polling or per-frame Fabric traversal.
      // Stable evidence does not request a capture or touch the native registry.
      if (activation != null && !refreshing) {
        BackEvidence nextBack = readBack();
        TitleEvidence nextTitle = readTitle();
        if (!sameBack(back, nextBack) || !sameTitle(title, nextTitle)) refresh();
      }
      return true;
    }
    public void onViewAttachedToWindow(View view) { refresh(); queueRefresh(); }
    public void onViewDetachedFromWindow(View view) { refresh(); queueRefresh(); }
    void remove() {
      view.removeOnAttachStateChangeListener(this);
      if (drawObserver != null && drawObserver.isAlive()) drawObserver.removeOnPreDrawListener(this);
      drawObserver = null;
      back = null;
      title = null;
    }
  }
  public void willDispatchViewUpdates(UIManager manager) {}
  public void willMountItems(UIManager manager) {}
  public void didMountItems(UIManager manager) { refresh(); queueRefresh(); }
  public void didDispatchMountItems(UIManager manager) {}
  public void didScheduleMountItems(UIManager manager) {}
  private static native void nativeLifecycle(String token, boolean start);
  private native void nativeRefresh(String token);
}
