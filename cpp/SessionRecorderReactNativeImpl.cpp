#include "SessionRecorderReactNativeImpl.h"
#include "SessionRecorderPlatformLog.h"
#include <react/renderer/uimanager/UIManagerBinding.h>
#include <mutex>
#include <cstring>
#include <stdlib.h>
#if defined(__ANDROID__)
#include <jni.h>
#include <cmath>

#define LOG_DEBUG(...) __android_log_print(ANDROID_LOG_DEBUG, "SessionRecorderTAG", __VA_ARGS__)
#endif

namespace facebook::react
{
#if defined(__ANDROID__)
  // Singleton Pattern: Creates a global, eternal observer.
  // Prevents RN's internal thread from crashing on destroyed memory during reload.
  std::shared_ptr<lomtree::SessionRecorderObserver> getGlobalObserver()
  {
    static std::shared_ptr<lomtree::SessionRecorderObserver> instance = nullptr;
    static std::once_flag flag;
    std::call_once(flag, []()
                   {
          lomtree::ObserverConfig config;
          config.phase = lomtree::DebouncePhase::IDLE;
          config.debounceMs = 150;
          config.maxNodes = 10000;
          instance = std::make_shared<lomtree::SessionRecorderObserver>(config); });
    return instance;
  }

  // Global variables used by the emergency shutdown triggered from Java
  static std::shared_ptr<std::atomic<bool>> globalIsAlive = nullptr;
#endif

  SessionRecorderReactNativeImpl::SessionRecorderReactNativeImpl(std::shared_ptr<CallInvoker> jsInvoker)
      : NativeSessionRecorderReactNativeCxxSpec(jsInvoker), jsInvoker_(jsInvoker)
  {
    isAlive_ = std::make_shared<std::atomic<bool>>(true);
#if defined(__ANDROID__)
    globalIsAlive = isAlive_;
#endif
  }

  std::string SessionRecorderReactNativeImpl::getUuidRandomHex(jsi::Runtime &rt)
  {
#if defined(__APPLE__) || defined(__ANDROID__)
    // Available before initJSI: session IDs are allocated during Context construction.
    // Both platforms provide this CSPRNG in libc; no JS crypto polyfill is needed.
    (void)rt;
    unsigned char bytes[16];
    arc4random_buf(bytes, sizeof(bytes));
    constexpr char hex[] = "0123456789abcdef";
    std::string result(sizeof(bytes) * 2, '0');
    for (size_t i = 0; i < sizeof(bytes); ++i)
    {
      result[i * 2] = hex[bytes[i] >> 4];
      result[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    return result;
#else
    throw jsi::JSError(rt, "Native UUID entropy requires Android or iOS");
#endif
  }

  bool SessionRecorderReactNativeImpl::initJSI(jsi::Runtime &rt)
  {
#if defined(__APPLE__)
    try
    {
      // Explicitly resolve the companion; optional environment metadata is not bootstrap.
      auto proxy = rt.global().getProperty(rt, "nativeModuleProxy");
      if (!proxy.isObject())
        return false;
      auto module = proxy.asObject(rt).getProperty(rt, "SessionRecorderLifecycle");
      if (!module.isObject())
        return false;
      auto lifecycle = module.asObject(rt);
      auto prepare = lifecycle.getProperty(rt, "prepare");
      if (!prepare.isObject() || !prepare.asObject(rt).isFunction(rt))
        return false;
      auto ready = prepare.asObject(rt).asFunction(rt).callWithThis(rt, lifecycle);
      if (!ready.isBool() || !ready.getBool())
        return false;

      auto owner = rt.global().getProperty(rt, "__sessionRecorderIOSOwner");
      auto binding = UIManagerBinding::getBinding(rt);
      if (!binding || !owner.isObject() ||
          !owner.asObject(rt).isHostObject<lomtree::SessionRecorderIOSOwner>(rt))
        return false;
      iosOwner_ = owner.asObject(rt).getHostObject<lomtree::SessionRecorderIOSOwner>(rt);
      // Old HostObjects/deliveries keep their old token even within the same runtime.
      isAlive_ = std::make_shared<std::atomic<bool>>(true);
      return iosOwner_->install(binding->getUIManager(), isAlive_,
                                [this, &rt](std::shared_ptr<lomtree::SessionRecorderObserver> observer)
                                {
                                  observer_ = std::move(observer);
                                  return installJSIBindings(rt);
                                });
    }
    catch (...)
    {
      LOG_ERROR("iOS JSI bootstrap unavailable");
      return false;
    }
#elif defined(__ANDROID__)
    observer_ = getGlobalObserver();
    return installJSIBindings(rt);
#else
    return false;
#endif
  }

  bool SessionRecorderReactNativeImpl::installJSIBindings(jsi::Runtime &rt)
  {

    // Cleans up garbage callbacks from previous reloads before starting
    observer_->clearSnapshotCallback();
    observer_->clearCaptureResultCallback();

    auto hostObject = std::make_shared<lomtree::SessionRecorderTreeHostObject>(observer_, jsInvoker_, isAlive_
#if defined(__APPLE__)
                                                                               ,
                                                                               iosOwner_
#endif
    );

    rt.global().setProperty(
        rt, "__viewTreeScanner",
        jsi::Object::createFromHostObject(rt, std::move(hostObject)));

    std::weak_ptr<CallInvoker> weakInvoker = jsInvoker_;
    std::weak_ptr<lomtree::SessionRecorderObserver> weakObserver = observer_;
    std::shared_ptr<std::atomic<bool>> isAlive = isAlive_;

    observer_->setSnapshotCallback(
        [weakInvoker, weakObserver, isAlive](
            SurfaceId surfaceId,
            std::vector<lomtree::Root> roots,
            float viewportWidth,
            float viewportHeight,
            uint64_t signature,
            uint64_t requestId,
            uint64_t debtGeneration,
            uint64_t captureGeneration,
            int64_t revisionNumber,
            const char *source,
            std::string surfaceInstance,
            uint64_t pagerVersion,
            lomtree::ScanTiming scanTiming
#if defined(ANDROID) || defined(__APPLE__)
            , uint64_t visibilityGeneration
#endif
        )
        {
          // If Java pushed the panic button, cancel the entire trip to JS
          auto observer = weakObserver.lock();
          // Keep mutation eligibility only until this delivery finishes, including
          // rejection, missing callbacks and discarded runtime work. Coverage is
          // still established exclusively by the existing ACK/dedup guards.
          std::shared_ptr<uint64_t> mutationCompletion;
          if (observer && std::strcmp(source, "mutation") == 0)
          {
            mutationCompletion = std::shared_ptr<uint64_t>(new uint64_t{0},
                [weakObserver, surfaceId](uint64_t *deliveryId)
                {
                  if (auto owner = weakObserver.lock())
                    owner->completeMutationDelivery(surfaceId, *deliveryId);
                  delete deliveryId;
                });
            *mutationCompletion = observer->beginMutationDelivery(
                surfaceId, surfaceInstance, captureGeneration, revisionNumber);
          }
          if (!isAlive || !(*isAlive) || !observer ||
              !observer->isCaptureGenerationCurrent(captureGeneration))
          {
            return;
          }

          auto safeInvoker = weakInvoker.lock();
          if (!safeInvoker)
          {
            return;
          }

          try
          {
            safeInvoker->invokeAsync(
                [weakObserver, surfaceId, roots = std::move(roots), viewportWidth, viewportHeight, signature, requestId, debtGeneration, captureGeneration, revisionNumber, source, pagerVersion, scanTiming = std::move(scanTiming), surfaceInstance = std::move(surfaceInstance), isAlive, mutationCompletion = std::move(mutationCompletion)
#if defined(ANDROID) || defined(__APPLE__)
                 , visibilityGeneration
#endif
                ](jsi::Runtime &safeRt) mutable
                {
                  [[maybe_unused]] auto completion = std::move(mutationCompletion);
                  // Double-check in case the app died while the event was waiting in the queue
                  auto observer = weakObserver.lock();
                  if (!isAlive || !(*isAlive) || !observer ||
                      !observer->isCaptureGenerationCurrent(captureGeneration))
                  {
                    return;
                  }

                  try
                  {
                    if (!observer->pagerRegistry()->current(pagerVersion) ||
                        !observer->isSurfaceInstanceCurrent(surfaceId, surfaceInstance))
                      return;
#if defined(ANDROID)
                    if (!observer->androidVisibility->current(visibilityGeneration)) return;
#elif defined(__APPLE__)
                    if (!observer->isIOSVisibilityGenerationCurrent(visibilityGeneration)) return;
#endif
                    jsi::Value jsArray = lomtree::SessionRecorderTreeHostObject::toObjectArray(safeRt, roots);

                    jsi::Object payload(safeRt);
                    payload.setProperty(safeRt, "signature", jsi::String::createFromUtf8(safeRt, std::to_string(signature)));
                    payload.setProperty(safeRt, "roots", std::move(jsArray));

                    jsi::Object metadata(safeRt);
                    if (!scanTiming.activation.empty()) {
                      metadata.setProperty(safeRt, "scanActivation", jsi::String::createFromUtf8(safeRt, scanTiming.activation));
                      metadata.setProperty(safeRt, "scanStart", jsi::Value(scanTiming.scanStart));
                      metadata.setProperty(safeRt, "scanEnd", jsi::Value(scanTiming.scanEnd));
                    }
                    metadata.setProperty(safeRt, "source", jsi::String::createFromAscii(safeRt, source));
                    metadata.setProperty(safeRt, "surfaceInstance", jsi::String::createFromUtf8(safeRt, surfaceInstance));
                    metadata.setProperty(safeRt, "requestId", jsi::Value((double)requestId));
                    metadata.setProperty(safeRt, "debtGeneration", jsi::Value((double)debtGeneration));
                    metadata.setProperty(safeRt, "captureGeneration", jsi::Value((double)captureGeneration));
                    metadata.setProperty(safeRt, "pagerVersion", jsi::Value((double)pagerVersion));
#if defined(ANDROID) || defined(__APPLE__)
                    metadata.setProperty(safeRt, "visibilityGeneration", jsi::Value((double)visibilityGeneration));
#endif
                    metadata.setProperty(safeRt, "revisionNumber", jsi::Value((double)revisionNumber));
                    metadata.setProperty(safeRt, "viewportWidth", jsi::Value((double)viewportWidth));
                    metadata.setProperty(safeRt, "viewportHeight", jsi::Value((double)viewportHeight));
                    jsi::Value callback = safeRt.global().getProperty(safeRt, "__onSessionRecorderTreeUpdate");

                    if (callback.isObject() && callback.asObject(safeRt).isFunction(safeRt))
                    {
                      callback.asObject(safeRt).asFunction(safeRt).call(
                          safeRt,
                          jsi::Value((double)surfaceId),
                          std::move(payload),
                          std::move(metadata));
                    }
                  }
                  catch (...)
                  {
                    LOG_ERROR("JSI Error blocked from save");
                  }
                });
          }
          catch (...)
          {
            LOG_ERROR("Error async blocked");
          }
        });

    observer_->setCaptureResultCallback(
        [weakInvoker, weakObserver, isAlive](uint64_t requestId, uint64_t debtGeneration, lomtree::CaptureResult result, uint64_t captureGeneration, std::vector<lomtree::ActiveSurface> surfaces, uint64_t pagerVersion)
        {
          auto observer = weakObserver.lock();
          if (!isAlive || !(*isAlive) || !observer ||
              !observer->isCaptureGenerationCurrent(captureGeneration))
            return;

          auto safeInvoker = weakInvoker.lock();
          if (!safeInvoker)
            return;

          const char *status = result == lomtree::CaptureResult::Captured
                                   ? "captured"
                               : result == lomtree::CaptureResult::Deduplicated
                                   ? "deduplicated"
                                   : "unavailable";
          try
          {
            safeInvoker->invokeAsync(
                [weakObserver, requestId, debtGeneration, captureGeneration, pagerVersion, status = std::string(status), surfaces = std::move(surfaces), isAlive](jsi::Runtime &safeRt)
                {
                  auto observer = weakObserver.lock();
                  if (!isAlive || !(*isAlive) || !observer ||
                      !observer->isCaptureGenerationCurrent(captureGeneration))
                    return;
                  try
                  {
                    bool current = true;
                    for (const auto &surface : surfaces)
                      if (!observer->isSurfaceInstanceCurrent(surface.surfaceId, surface.surfaceInstance))
                        current = false;
#if defined(__APPLE__)
                    if (status != "unavailable")
                      for (const auto &surface : surfaces)
                        if (!observer->isIOSVisibilityGenerationCurrent(surface.visibilityGeneration))
                          current = false;
#endif
                    if (!current && requestId == 0)
                      return;
                    // A retired participant cannot satisfy an aggregate debt.
                    // Use existing Unavailable settlement, without resetting it.
                    const std::string effectiveStatus = current && observer->pagerRegistry()->current(pagerVersion) ? status : "unavailable";
                    jsi::Value callback = safeRt.global().getProperty(safeRt, "__onSessionRecorderCaptureResult");
                    if (callback.isObject() && callback.asObject(safeRt).isFunction(safeRt))
                    {
                      callback.asObject(safeRt).asFunction(safeRt).call(
                          safeRt,
                          jsi::Value((double)requestId),
                          jsi::Value((double)debtGeneration),
                          jsi::String::createFromUtf8(safeRt, effectiveStatus),
                          jsi::Value((double)captureGeneration));
                    }
                  }
                  catch (...)
                  {
                    LOG_ERROR("Capture result callback failed");
                  }
                });
          }
          catch (...)
          {
            LOG_ERROR("Capture result async dispatch failed");
          }
        });

    return true;
  }

  SessionRecorderReactNativeImpl::~SessionRecorderReactNativeImpl()
  {
    if (isAlive_)
      *isAlive_ = false;
#if defined(__APPLE__)
    if (iosOwner_)
      iosOwner_->invalidate();
#endif
  }
}

#if defined(__ANDROID__)
// Emergency shutdown called synchronously from Java (invalidate method)
namespace {
  std::string PagerActivation(JNIEnv *env, jstring token)
  {
    if (!token) return {};
    const char *utf = env->GetStringUTFChars(token, nullptr);
    if (!utf) return {};
    std::string value(utf);
    env->ReleaseStringUTFChars(token, utf);
    return value;
  }
  jlongArray PagerStatus(JNIEnv *env, lomtree::SessionRecorderPagerRegistry::Update update)
  {
    jlong values[]{update.active ? 1 : 0, update.capture ? 1 : 0, static_cast<jlong>(update.version)};
    auto result = env->NewLongArray(3);
    if (result) env->SetLongArrayRegion(result, 0, 3, values);
    return result;
  }
}
extern "C" JNIEXPORT void JNICALL
Java_com_sessionrecorderreactnative_SessionRecorderPagerObserver_nativeStart(JNIEnv *env, jclass, jstring token)
{
  if (facebook::react::globalIsAlive && facebook::react::globalIsAlive->load()) facebook::react::getGlobalObserver()->pagerRegistry()->start(PagerActivation(env, token));
}
extern "C" JNIEXPORT void JNICALL
Java_com_sessionrecorderreactnative_SessionRecorderPagerObserver_nativeStop(JNIEnv *env, jclass, jstring token)
{ facebook::react::getGlobalObserver()->pagerRegistry()->stop(PagerActivation(env, token)); }
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_sessionrecorderreactnative_SessionRecorderPagerObserver_nativeCandidates(JNIEnv *env, jclass, jstring token)
{
  auto candidates = facebook::react::globalIsAlive && facebook::react::globalIsAlive->load() ? facebook::react::getGlobalObserver()->reconcilePagers(PagerActivation(env, token)) :
      std::vector<lomtree::SessionRecorderPagerRegistry::Pager>{};
  auto arrayClass = env->FindClass("[J");
  if (!arrayClass) return nullptr;
  auto result = env->NewObjectArray(static_cast<jsize>(candidates.size()), arrayClass, nullptr);
  env->DeleteLocalRef(arrayClass);
  if (!result) return nullptr;
  for (size_t i = 0; i < candidates.size(); ++i) {
    const auto &pager = candidates[i];
    jlong fields[]{pager.surface, pager.tag, static_cast<jlong>(pager.instance)};
    auto item = env->NewLongArray(3);
    if (!item) return result;
    env->SetLongArrayRegion(item, 0, 3, fields);
    env->SetObjectArrayElement(result, static_cast<jsize>(i), item);
    env->DeleteLocalRef(item);
  }
  return result;
}
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_sessionrecorderreactnative_SessionRecorderPagerObserver_nativePublish(JNIEnv *env, jclass, jstring token,
    jint surface, jint tag, jlong instance, jlong nativeInstance, jint phase, jint index)
{
  auto registry = facebook::react::getGlobalObserver()->pagerRegistry();
  if (!facebook::react::globalIsAlive || !facebook::react::globalIsAlive->load()) return PagerStatus(env, registry->status());
  return PagerStatus(env, registry->publish(PagerActivation(env, token), surface, tag, instance, nativeInstance, phase, index));
}
extern "C" JNIEXPORT void JNICALL
Java_com_sessionrecorderreactnative_SessionRecorderPagerObserver_nativeDetach(JNIEnv *env, jclass, jstring token,
    jint surface, jint tag, jlong instance, jlong nativeInstance)
{ facebook::react::getGlobalObserver()->pagerRegistry()->detach(PagerActivation(env, token), surface, tag, instance, nativeInstance); }
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_sessionrecorderreactnative_SessionRecorderPagerObserver_nativeStatus(JNIEnv *env, jclass)
{ return PagerStatus(env, facebook::react::getGlobalObserver()->pagerRegistry()->status()); }

extern "C" JNIEXPORT void JNICALL
Java_com_sessionrecorderreactnative_SessionRecorderScreenVisibilityObserver_nativeLifecycle(
    JNIEnv *env, jclass, jstring token, jboolean start)
{
  try {
    if (!facebook::react::globalIsAlive || !facebook::react::globalIsAlive->load()) return;
    auto registry = facebook::react::getGlobalObserver()->androidVisibility;
    if (start) registry->start(PagerActivation(env, token));
    else registry->stop(PagerActivation(env, token));
  } catch (...) {}
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_sessionrecorderreactnative_SessionRecorderAlertObserver_nativeRefresh(
    JNIEnv *env, jobject observer, jstring token, jboolean present)
{
  try {
    if (!facebook::react::globalIsAlive || !facebook::react::globalIsAlive->load()) return false;
    auto owner = facebook::react::getGlobalObserver();
    auto activation = PagerActivation(env, token);
    auto cls = env->GetObjectClass(observer);
    auto probe = env->GetMethodID(cls, "probe", "(I)[D");
    if (env->ExceptionCheck()) { env->ExceptionClear(); probe = nullptr; }
    env->DeleteLocalRef(cls);
    return owner->refreshAndroidAlerts(activation, present, [&](int rootTag) {
      lomtree::SessionRecorderScreenVisibilityRegistry::Snapshot::Alert alert;
      if (!probe) return alert;
      auto values = static_cast<jdoubleArray>(env->CallObjectMethod(observer, probe, rootTag));
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        if (values) env->DeleteLocalRef(values);
        return alert;
      }
      jdouble fields[28]{};
      const auto length = values ? env->GetArrayLength(values) : 0;
      const bool complete = length == 22 || length == 23 || length == 28;
      if (complete) env->GetDoubleArrayRegion(values, 0, length, fields);
      if (values) env->DeleteLocalRef(values);
      if (env->ExceptionCheck()) { env->ExceptionClear(); return alert; }
      if (!complete || !std::isfinite(fields[0]) || !std::isfinite(fields[1]) || fields[0] <= 0 || fields[1] <= 0)
        return alert;
      if ((length == 28 && fields[22] != 1) || (length == 23 && fields[22] != 2)) return alert;
      alert.datePicker = length == 28;
      alert.timePicker = length == 23;
      alert.viewportWidth = static_cast<float>(fields[0]);
      alert.viewportHeight = static_cast<float>(fields[1]);
      auto element = [&](size_t offset) {
        lomtree::SessionRecorderScreenVisibilityRegistry::Observation::BackButton box;
        if (!std::isfinite(fields[offset]) || fields[offset] <= 0 || fields[offset] > 9007199254740991.0 ||
            fields[offset] != std::floor(fields[offset])) return box;
        for (size_t i = 1; i < 5; ++i) if (!std::isfinite(fields[offset + i])) return box;
        if (fields[offset + 1] < 0 || fields[offset + 2] < 0 || fields[offset + 3] <= 0 || fields[offset + 4] <= 0 ||
            fields[offset + 1] + fields[offset + 3] > fields[0] + 0.001 ||
            fields[offset + 2] + fields[offset + 4] > fields[1] + 0.001) return box;
        return lomtree::SessionRecorderScreenVisibilityRegistry::Observation::BackButton{
            static_cast<uint64_t>(fields[offset]), static_cast<float>(fields[offset + 1]),
            static_cast<float>(fields[offset + 2]), static_cast<float>(fields[offset + 3]),
            static_cast<float>(fields[offset + 4])};
      };
      alert.dialog = element(2);
      for (size_t i = 0; i < alert.buttons.size(); ++i) alert.buttons[i] = element(7 + 5 * i);
      if (alert.datePicker) alert.picker = element(23);
      return alert;
    });
  } catch (...) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    return false;
  }
}
extern "C" JNIEXPORT void JNICALL
Java_com_sessionrecorderreactnative_SessionRecorderScreenVisibilityObserver_nativeRefresh(
    JNIEnv *env, jobject observer, jstring token)
{
  try {
    if (!facebook::react::globalIsAlive || !facebook::react::globalIsAlive->load()) return;
    auto owner = facebook::react::getGlobalObserver();
    auto activation = PagerActivation(env, token);
    if (owner->androidVisibility->snapshot()->activation != activation) return;
    auto cls = env->GetObjectClass(observer);
    auto probe = env->GetMethodID(cls, "probe", "(II)J");
    if (env->ExceptionCheck()) { env->ExceptionClear(); probe = nullptr; }
    if (!probe) { env->DeleteLocalRef(cls); return; }
    auto probeBack = env->GetMethodID(cls, "probeBack", "(I)[D");
    if (env->ExceptionCheck()) { env->ExceptionClear(); probeBack = nullptr; }
    auto probeTitle = env->GetMethodID(cls, "probeTitle", "(I)[D");
    if (env->ExceptionCheck()) { env->ExceptionClear(); probeTitle = nullptr; }
    env->DeleteLocalRef(cls);
    auto readHeaderRect = [&](jmethodID method, int tag) {
      lomtree::SessionRecorderScreenVisibilityRegistry::Observation::BackButton button;
      if (!method) return button;
      auto values = static_cast<jdoubleArray>(env->CallObjectMethod(observer, method, tag));
      if (env->ExceptionCheck()) { env->ExceptionClear(); if (values) env->DeleteLocalRef(values); return button; }
      if (values && env->GetArrayLength(values) == 5) {
        jdouble rect[5];
        env->GetDoubleArrayRegion(values, 0, 5, rect);
        if (!env->ExceptionCheck() && std::isfinite(rect[0]) && rect[0] > 0 &&
            rect[0] <= 9007199254740991.0 && rect[0] == std::floor(rect[0]) &&
            std::isfinite(rect[1]) && std::isfinite(rect[2]) &&
            std::isfinite(rect[3]) && std::isfinite(rect[4]) && rect[3] > 0 && rect[4] > 0) {
          button = {static_cast<uint64_t>(rect[0]), static_cast<float>(rect[1]), static_cast<float>(rect[2]),
                    static_cast<float>(rect[3]), static_cast<float>(rect[4])};
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
      }
      if (values) env->DeleteLocalRef(values);
      return button;
    };
    owner->refreshAndroidScreens(activation, [&](int tag, int rootTag) -> long long {
      auto result = env->CallLongMethod(observer, probe, tag, rootTag);
      if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
      return result;
    }, [&](int tag) { return readHeaderRect(probeBack, tag); },
       [&](int tag) { return readHeaderRect(probeTitle, tag); });
  } catch (...) {}
}

extern "C" JNIEXPORT void JNICALL
Java_com_sessionrecorderreactnative_SessionRecorderModule_nativeCleanup(JNIEnv *env, jobject thiz)
{
  // Kills async tasks currently en route to JS
  if (facebook::react::globalIsAlive)
  {
    *facebook::react::globalIsAlive = false;
  }

  // Clears ghost pointers from the old UIManager and deletes the JS callback
  auto observer = facebook::react::getGlobalObserver();
  if (observer)
  {
    // Invalidate queued JS deliveries before resetUIManager() advances the
    // capture generation and unregisters the Fabric hook outside its mutex.
    observer->clearSnapshotCallback();
    observer->clearCaptureResultCallback();
    observer->resetUIManager();
  }
  LOG_DEBUG("JAVA BUTTON CLEAN UP");
}
#endif
