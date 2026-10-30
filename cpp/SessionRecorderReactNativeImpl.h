#pragma once

#include <SessionRecorderReactNativeSpecJSI.h>
#include <SessionRecorderTreeScanner.h>
#include <SessionRecorderObserver.h>
#include <SessionRecorderTreeHostObject.h>

#include <jsi/jsi.h>
#include <memory>
#include <atomic>
#include <string>

namespace facebook::react
{

  class SessionRecorderReactNativeImpl
      : public NativeSessionRecorderReactNativeCxxSpec<SessionRecorderReactNativeImpl>
  {
  public:
    SessionRecorderReactNativeImpl(std::shared_ptr<CallInvoker> jsInvoker);
    ~SessionRecorderReactNativeImpl();

    std::string getUuidRandomHex(jsi::Runtime &rt);
    bool initJSI(jsi::Runtime &rt);

  private:
    bool installJSIBindings(jsi::Runtime &rt);
#if defined(__APPLE__)
    std::shared_ptr<lomtree::SessionRecorderIOSOwner> iosOwner_;
#endif
    std::weak_ptr<CallInvoker> jsInvoker_;
    std::shared_ptr<lomtree::SessionRecorderObserver> observer_;

    // Atomic flag to check if the app is still alive.
    // Protects async events from trying to run in a destroyed JS environment.
    std::shared_ptr<std::atomic<bool>> isAlive_;
  };

}
