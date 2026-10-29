package com.sessionrecorderreactnative;

import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import androidx.viewpager2.widget.ViewPager2;
import com.facebook.react.bridge.Arguments;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.bridge.UIManager;
import com.facebook.react.bridge.UIManagerListener;
import com.facebook.react.bridge.WritableMap;
import com.facebook.react.bridge.WritableArray;
import com.facebook.react.uimanager.events.Event;
import com.facebook.react.uimanager.events.EventDispatcher;
import com.facebook.react.uimanager.events.EventDispatcherListener;
import com.facebook.react.uimanager.events.RCTEventEmitter;
import com.facebook.react.uimanager.UIManagerHelper;
import com.facebook.react.uimanager.IllegalViewOperationException;
import com.facebook.react.uimanager.common.UIManagerType;
import com.facebook.react.modules.core.DeviceEventManagerModule;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

// UI-thread lifetime, independent of the package's own ViewPager2 callback.
final class SessionRecorderPagerObserver implements UIManagerListener {
  private final ReactApplicationContext context;
  private final Handler main = new Handler(Looper.getMainLooper());
  private final Map<Integer, Record> records = new HashMap<>();
  private UIManager manager;
  private EventDispatcher dispatcher;
  private final EventDispatcherListener composeEvents = this::observeComposeEvent;
  private String activation;
  private long nextNativeInstance;
  private long lastVersion = -1;
  private boolean reconcileQueued;

  SessionRecorderPagerObserver(ReactApplicationContext context) { this.context = context; }

  void start(String token) {
    if (token.equals(activation)) { reconcile(); return; }
    stop();
    activation = token;
    nativeStart(token);
    manager = UIManagerHelper.getUIManager(context, UIManagerType.FABRIC);
    if (manager != null) manager.addUIManagerEventListener(this);
    dispatcher = UIManagerHelper.getEventDispatcher(context, UIManagerType.FABRIC);
    if (dispatcher != null) dispatcher.addListener(composeEvents);
    reconcile();
  }
  void stop() {
    String token = activation;
    activation = null;
    if (dispatcher != null) dispatcher.removeListener(composeEvents);
    dispatcher = null;
    if (manager != null) manager.removeUIManagerEventListener(this);
    manager = null;
    for (Record record : records.values()) record.remove();
    records.clear();
    if (token != null) nativeStop(token);
  }
  private void emit(long[] status) {
    if (activation == null || status == null || status.length != 3 ||
        (lastVersion == status[2] && status[1] == 0)) return;
    lastVersion = status[2];
    if (!context.hasActiveReactInstance()) return;
    WritableMap event = Arguments.createMap();
    event.putString("activationToken", activation);
    event.putDouble("version", status[2]);
    event.putBoolean("active", status[0] != 0);
    event.putBoolean("capture", status[1] != 0);
    context.getJSModule(DeviceEventManagerModule.RCTDeviceEventEmitter.class)
        .emit("SessionRecorderPagerState", event);
  }
  private void reconcile() {
    if (activation == null || manager == null) return;
    long[][] candidates = nativeCandidates(activation);
    Set<Integer> seen = new HashSet<>();
    if (candidates == null) return;
    for (long[] candidate : candidates) {
      if (candidate == null || candidate.length != 3) continue;
      int surface = (int) candidate[0], tag = (int) candidate[1];
      View view;
      try { view = manager.resolveView(tag); }
      catch (IllegalViewOperationException unmounted) { continue; }
      if (!(view instanceof ViewGroup) || !view.isAttachedToWindow() || view.getId() != tag ||
          UIManagerHelper.getSurfaceId(view) != surface) continue;
      // The candidate list comes exclusively from exact RNCViewPager Fabric nodes.
      ViewPager2 pager = null;
      ViewGroup host = (ViewGroup) view;
      for (int i = 0; i < host.getChildCount(); ++i) {
        if (host.getChildAt(i) instanceof ViewPager2) {
          if (pager != null) { pager = null; break; }
          pager = (ViewPager2) host.getChildAt(i);
        }
      }
      boolean compose = "com.reactnativepagerview.ComposePagerView".equals(host.getClass().getName());
      if (pager == null && !compose) continue;
      seen.add(tag);
      Record record = records.get(tag);
      if (record != null && (record.host != host || record.pager != pager ||
          record.instance != candidate[2] || record.surface != surface)) {
        record.remove(); records.remove(tag); record = null;
      }
      if (record == null) {
        record = new Record(host, pager, surface, tag, candidate[2]);
        records.put(tag, record);
        if (pager != null) pager.registerOnPageChangeCallback(record);
        host.addOnAttachStateChangeListener(record);
        if (pager != null) record.publish(pager.getScrollState(), pager.getCurrentItem());
        // Compose has no public current-state getter. Register as unknown,
        // including after resume/replacement, until official events accredit it.
        else record.publish(-1, -2);
      }
    }
    for (Integer tag : new HashSet<>(records.keySet())) if (!seen.contains(tag)) {
      records.remove(tag).remove();
    }
    emit(nativeStatus());
  }
  private void queueReconcile() {
    if (reconcileQueued) return;
    reconcileQueued = true;
    main.post(() -> { reconcileQueued = false; reconcile(); });
  }

  @SuppressWarnings("deprecation")
  private void observeComposeEvent(Event<?> event) {
    // Compose 9.x dispatches on UI. Do not queue an Event (disposed immediately
    // after listeners), or bind an old tag to a replacement View on another thread.
    String name = event.getEventName();
    if (!"topPageSelected".equals(name) && !"topPageScroll".equals(name) &&
        !"topPageScrollStateChanged".equals(name)) return;
    if (Looper.myLooper() != Looper.getMainLooper() || activation == null) return;
    Record record = records.get(event.getViewTag());
    if (record == null || record.pager != null || !record.host.isAttachedToWindow()) return;
    try {
      if (manager == null || manager.resolveView(record.tag) != record.host) return;
      // 9.0.7's official Event subclasses serialize through this public method.
      // Use a private receiver; never replace the host emitter, dispose the
      // original event, or retain its map beyond this synchronous observation.
      event.dispatch(new RCTEventEmitter() {
        @Override public void receiveEvent(int tag, String eventName, WritableMap data) {
          if (tag == record.tag && name.equals(eventName) && data != null) {
            record.composeEvent(name, data);
          }
        }
        @Override public void receiveTouches(String eventName, WritableArray touches, WritableArray changed) {}
      });
    } catch (RuntimeException unavailable) {
      record.idleSeen = false;
      record.composeMoving = false;
      record.lastOfficialSelected = record.aligned = -1;
      try { record.publish(0, -2); } // Unknown selection, not invented movement/suppression.
      catch (RuntimeException ignored) { /* Never fail the host's dispatcher. */ }
    }
  }
  @Override public void willDispatchViewUpdates(UIManager manager) {}
  @Override public void willMountItems(UIManager manager) {}
  @Override public void didMountItems(UIManager manager) { reconcile(); queueReconcile(); }
  @Override public void didDispatchMountItems(UIManager manager) {}
  @Override public void didScheduleMountItems(UIManager manager) {}

  private final class Record extends ViewPager2.OnPageChangeCallback implements View.OnAttachStateChangeListener {
    final ViewGroup host;
    final ViewPager2 pager;
    final int surface, tag;
    final long instance, nativeInstance = ++nextNativeInstance;
    final String token = activation;
    boolean idleSeen;
    boolean composeMoving;
    int lastOfficialSelected = -1, aligned = -1;
    Record(ViewGroup host, ViewPager2 pager, int surface, int tag, long instance) {
      this.host = host; this.pager = pager; this.surface = surface; this.tag = tag; this.instance = instance;
    }
    void publish(int phase, int index) {
      if (!token.equals(activation) || records.get(tag) != this || host.getId() != tag ||
          UIManagerHelper.getSurfaceId(host) != surface) return;
      long[] status = nativePublish(token, surface, tag, instance, nativeInstance, phase, index);
      emit(status);
    }
    void composeEvent(String name, WritableMap data) {
      if (!token.equals(activation) || records.get(tag) != this) return;
      if ("topPageScrollStateChanged".equals(name)) {
        String state = data.getString("pageScrollState");
        if ("dragging".equals(state) || "settling".equals(state)) {
          if ("dragging".equals(state) || !composeMoving) {
            idleSeen = false;
            aligned = -1;
          }
          composeMoving = true;
          publish("dragging".equals(state) ? 1 : 2, -2);
          return;
        }
        if (!"idle".equals(state)) {
          return;
        }
        idleSeen = true;
      } else if ("topPageSelected".equals(name)) {
        // Compose may omit a repeated selection when a drag returns to the same page.
        lastOfficialSelected = data.getInt("position");
      } else {
        int position = data.getInt("position");
        double offset = data.getDouble("offset");
        if (!Double.isFinite(offset) || offset != 0) {
          // Independent collectors can deliver this after idle. It revokes
          // alignment, not the idle/selection evidence of this interaction.
          aligned = -1;
          composeMoving = true;
          publish(2, -2);
          return;
        }
        aligned = position >= 0 ? position : -1;
      }
      // Re-evaluate after any of the three official evidences, in any order.
      if (idleSeen && aligned >= 0 && lastOfficialSelected >= 0 && lastOfficialSelected == aligned) {
        composeMoving = false;
        publish(0, lastOfficialSelected);
      }
    }
    @Override public void onPageSelected(int position) { publish(-1, position); }
    @Override public void onPageScrollStateChanged(int state) { publish(state, pager.getCurrentItem()); }
    @Override public void onViewAttachedToWindow(View view) { queueReconcile(); }
    @Override public void onViewDetachedFromWindow(View view) {
      if (records.get(tag) != this) return;
      records.remove(tag); remove(); emit(nativeStatus());
    }
    void remove() {
      if (pager == null) {
        lastOfficialSelected = aligned = -1;
        idleSeen = composeMoving = false;
      }
      if (pager != null) pager.unregisterOnPageChangeCallback(this);
      host.removeOnAttachStateChangeListener(this);
      nativeDetach(token, surface, tag, instance, nativeInstance);
    }
  }
  private static native void nativeStart(String activation);
  private static native void nativeStop(String activation);
  private static native long[][] nativeCandidates(String activation);
  private static native long[] nativePublish(String activation, int surface, int tag, long instance,
                                            long nativeInstance, int phase, int index);
  private static native void nativeDetach(String activation, int surface, int tag, long instance, long nativeInstance);
  private static native long[] nativeStatus();
}
