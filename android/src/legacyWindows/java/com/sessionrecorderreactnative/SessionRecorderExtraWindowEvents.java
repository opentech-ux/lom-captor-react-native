package com.sessionrecorderreactnative;

import android.view.Window;
import com.facebook.react.bridge.ReactApplicationContext;
import java.util.function.Consumer;

// Older RN: retain Activity capture, without linking unavailable Dialog hooks.
final class SessionRecorderExtraWindowEvents {
  SessionRecorderExtraWindowEvents(ReactApplicationContext context, Consumer<Window> create, Consumer<Window> destroy) {}
  void start() {}
  void stop() {}
}
