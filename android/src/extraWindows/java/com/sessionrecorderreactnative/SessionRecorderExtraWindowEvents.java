package com.sessionrecorderreactnative;

import android.view.Window;
import com.facebook.react.bridge.ReactApplicationContext;
import com.facebook.react.interfaces.ExtraWindowEventListener;
import java.util.function.Consumer;

// Compiled only when the host RN exposes this public additive API.
final class SessionRecorderExtraWindowEvents implements ExtraWindowEventListener {
  private final ReactApplicationContext context;
  private final Consumer<Window> create, destroy;
  private boolean listening;

  SessionRecorderExtraWindowEvents(ReactApplicationContext context, Consumer<Window> create, Consumer<Window> destroy) {
    this.context = context; this.create = create; this.destroy = destroy;
  }
  void start() {
    if (listening) return;
    context.addExtraWindowEventListener(this);
    listening = true;
  }
  void stop() {
    if (!listening) return;
    listening = false;
    context.removeExtraWindowEventListener(this);
  }
  @Override public void onExtraWindowCreate(Window window) {
    if (!listening) return;
    try { create.accept(window); } catch (RuntimeException ignored) { /* Observational, fail-open. */ }
  }
  @Override public void onExtraWindowDestroy(Window window) {
    if (!listening) return;
    try { destroy.accept(window); } catch (RuntimeException ignored) { /* Observational, fail-open. */ }
  }
}
