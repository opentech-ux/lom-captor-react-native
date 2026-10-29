#import <React/RCTEventEmitter.h>
#import <React/RCTScheduler.h>
#import <React/RCTSurfacePresenter.h>
#import <ReactCommon/RCTTurboModule.h>
#import <ReactCommon/RCTTurboModuleWithJSIBindings.h>
#import "SessionRecorderTouchObserver.h"
#import "SessionRecorderScrollObserver.h"
#import "SessionRecorderPagerObserver.h"
#import "SessionRecorderScreenVisibilityObserver.h"

#include "SessionRecorderIOSOwner.h"

using namespace facebook;
using namespace facebook::react;

// This companion has no public JS export and does not change the main Codegen Spec.
// Its small method table uses RN's ObjCTurboModule invocation/conversion boundary.
namespace
{
    class LifecycleTurboModule final : public ObjCTurboModule
    {
    public:
        explicit LifecycleTurboModule(const InitParams &params) : ObjCTurboModule(params)
        {
            methodMap_["prepare"] = {0, [](jsi::Runtime &rt, TurboModule &module,
                const jsi::Value *args, size_t count) -> jsi::Value
            {
                return static_cast<ObjCTurboModule &>(module).invokeObjCMethod(
                    rt, BooleanKind, "prepare", @selector(prepare), args, count);
            }};
            methodMap_["addListener"] = {1, [](jsi::Runtime &rt, TurboModule &module,
                const jsi::Value *args, size_t count) -> jsi::Value
            {
                return static_cast<ObjCTurboModule &>(module).invokeObjCMethod(
                    rt, VoidKind, "addListener", @selector(addListener:), args, count);
            }};
            methodMap_["removeListeners"] = {1, [](jsi::Runtime &rt, TurboModule &module,
                const jsi::Value *args, size_t count) -> jsi::Value
            {
                return static_cast<ObjCTurboModule &>(module).invokeObjCMethod(
                    rt, VoidKind, "removeListeners", @selector(removeListeners:), args, count);
            }};
            methodMap_["startListening"] = {1, [](jsi::Runtime &rt, TurboModule &module,
                const jsi::Value *args, size_t count) -> jsi::Value
            {
                return static_cast<ObjCTurboModule &>(module).invokeObjCMethod(
                    rt, VoidKind, "startListening", @selector(startListening:), args, count);
            }};
            methodMap_["stopListening"] = {0, [](jsi::Runtime &rt, TurboModule &module,
                const jsi::Value *args, size_t count) -> jsi::Value
            {
                return static_cast<ObjCTurboModule &>(module).invokeObjCMethod(
                    rt, VoidKind, "stopListening", @selector(stopListening), args, count);
            }};
            methodMap_["snapshotScrollSessions"] = {1, [](jsi::Runtime &rt, TurboModule &module,
                const jsi::Value *args, size_t count) -> jsi::Value
            {
                return static_cast<ObjCTurboModule &>(module).invokeObjCMethod(
                    rt, PromiseKind, "snapshotScrollSessions", @selector(snapshotScrollSessions:resolve:reject:), args, count);
            }};
        }
    };
}

@interface SessionRecorderLifecycle : RCTEventEmitter <RCTTurboModule, RCTTurboModuleWithJSIBindings>
@property (nonatomic, weak) RCTSurfacePresenter *surfacePresenter;
- (NSNumber *)prepare;
- (void)startListening:(NSString *)activationToken;
- (void)stopListening;
- (void)startTouchObservation:(NSString *)activationToken;
- (void)stopTouchObservation:(NSString *)activationToken;
- (void)startScrollObservation:(NSString *)activationToken;
- (void)stopScrollObservation:(NSString *)activationToken;
- (void)snapshotScrollSessions:(NSArray *)requests resolve:(RCTPromiseResolveBlock)resolve
                        reject:(RCTPromiseRejectBlock)reject;
@end

@implementation SessionRecorderLifecycle
{
    std::shared_ptr<lomtree::SessionRecorderIOSOwner> _owner;
    SessionRecorderTouchObserver *_touchObserver;
    NSString *_touchObservationToken;
    SessionRecorderScrollObserver *_scrollObserver;
    NSString *_scrollObservationToken;
    SessionRecorderPagerObserver *_pagerObserver;
    NSString *_pagerObservationToken;
    std::shared_ptr<lomtree::SessionRecorderScreenVisibilityRegistry> _screenVisibility;
    SessionRecorderScreenVisibilityObserver *_screenVisibilityObserver;
    BOOL _invalidated;
}

RCT_EXPORT_MODULE(SessionRecorderLifecycle)

+ (BOOL)requiresMainQueueSetup
{
    return NO;
}

- (NSArray<NSString *> *)supportedEvents
{
    return @[
        @"SessionRecorderGesture", @"SessionRecorderScrollStart",
        @"SessionRecorderScrollValidated", @"SessionRecorderScrollEnd",
        @"SessionRecorderScrollCancel", @"SessionRecorderScrollRemoved", @"SessionRecorderPagerState"
    ];
}

- (std::shared_ptr<TurboModule>)getTurboModule:(const ObjCTurboModule::InitParams &)params
{
    return std::make_shared<LifecycleTurboModule>(params);
}

- (void)installJSIBindingsWithRuntime:(jsi::Runtime &)runtime
                          callInvoker:(const std::shared_ptr<CallInvoker> &)callInvoker
{
    @synchronized (self)
    {
        if (_invalidated) return;
        if (!_owner) _owner = std::make_shared<lomtree::SessionRecorderIOSOwner>();
        runtime.global().setProperty(runtime, "__sessionRecorderIOSOwner",
            jsi::Object::createFromHostObject(runtime, _owner));
    }
}

- (NSNumber *)prepare
{
    @synchronized (self)
    {
        if (_invalidated || !_owner) return @NO;
        // RN injects this presenter through setSurfacePresenter: in bridgeless mode.
        // Its scheduler getter is thread-safe; no legacy bridge/runtime lookup is used.
        @try
        {
            try
            {
                RCTScheduler *scheduler = self.surfacePresenter.scheduler;
                if (!scheduler) return @NO;
                auto manager = [scheduler uiManager];
                if (!_owner->prepare(manager)) return @NO;
                if (!_screenVisibility) {
                    auto visibility = std::make_shared<lomtree::SessionRecorderScreenVisibilityRegistry>();
                    auto visibilityObserver = [[SessionRecorderScreenVisibilityObserver alloc]
                        initWithPresenter:self.surfacePresenter registry:visibility];
                    __weak SessionRecorderLifecycle *weakSelf = self;
                    std::weak_ptr<lomtree::SessionRecorderIOSOwner> weakOwner = _owner;
                    visibilityObserver.onPublished = ^(NSInteger surface) {
                        SessionRecorderLifecycle *module = weakSelf;
                        if (!module) return;
                        SessionRecorderTouchObserver *touchObserver;
                        @synchronized (module) { touchObserver = module->_touchObserver; }
                        if (!touchObserver) return;
                        const auto recovery = [touchObserver recoveryForSurface:surface];
                        if (recovery) {
                            if (auto owner = weakOwner.lock()) owner->requestTouchCapture(*recovery);
                        }
                    };
                    manager->getContextContainer()->erase("SessionRecorderScreenVisibilityRegistry");
                    manager->getContextContainer()->insert<std::weak_ptr<lomtree::SessionRecorderScreenVisibilityRegistry>>(
                        "SessionRecorderScreenVisibilityRegistry", visibility);
                    _screenVisibility = std::move(visibility);
                    _screenVisibilityObserver = visibilityObserver;
                } else {
                    // A new JSI handoff must not inherit a previous activation's observations.
                    [_screenVisibilityObserver stop];
                }
                return @YES;
            }
            catch (...)
            {
                return @NO;
            }
        }
        @catch (NSException *exception)
        {
            return @NO;
        }
    }
}

- (void)invalidate
{
    std::shared_ptr<lomtree::SessionRecorderIOSOwner> owner;
    SessionRecorderTouchObserver *touchObserver;
    SessionRecorderScrollObserver *scrollObserver;
    @synchronized (self)
    {
        if (_invalidated) return;
        _invalidated = YES;
        _touchObservationToken = nil;
        touchObserver = _touchObserver;
        _touchObserver = nil;
        _scrollObservationToken = nil;
        scrollObserver = _scrollObserver;
        _scrollObserver = nil;
        owner = std::move(_owner);
        self.surfacePresenter = nil;
    }
    [_screenVisibilityObserver stop];
    [_pagerObserver stop];
    [touchObserver invalidate];
    [scrollObserver invalidate];
    if (owner) owner->invalidate();
    [super invalidate];
}

- (void)startListening:(NSString *)activationToken
{
    @synchronized (self) {
        if (!_invalidated && activationToken.length && self.surfacePresenter && _owner) {
            [_screenVisibilityObserver start:activationToken];
            auto registry = _owner->pagerRegistry();
            if (registry && !_pagerObserver) {
                __weak SessionRecorderLifecycle *weakSelf = self;
                _pagerObserver = [[SessionRecorderPagerObserver alloc] initWithPresenter:self.surfacePresenter
                    registry:registry sink:^(NSDictionary *event) {
                        SessionRecorderLifecycle *module = weakSelf;
                        if (!module) return;
                        @synchronized (module) {
                            if (module->_invalidated || ![module->_pagerObservationToken isEqualToString:event[@"activationToken"]]) return;
                            [module sendEventWithName:@"SessionRecorderPagerState" body:event];
                        }
                    }];
            }
            _pagerObservationToken = [activationToken copy];
            [_pagerObserver start:activationToken];
        }
        [self startTouchObservation:activationToken];
        [self startScrollObservation:activationToken];
    }
}

- (void)stopListening
{
    @synchronized (self) {
        [_screenVisibilityObserver stop];
        _pagerObservationToken = nil;
        [_pagerObserver stop];
        [self stopTouchObservation:_touchObservationToken];
        [self stopScrollObservation:_scrollObservationToken];
    }
}

// Internal token-scoped helpers, called through startListening/stopListening.
- (void)startTouchObservation:(NSString *)activationToken
{
    @try {
        try {
            @synchronized (self) {
                if (_invalidated || activationToken.length == 0 || !self.surfacePresenter) return;
                if (!_touchObserver) {
                    __weak SessionRecorderLifecycle *weakSelf = self;
                    _touchObserver = [[SessionRecorderTouchObserver alloc]
                        initWithSurfacePresenter:self.surfacePresenter eventSink:^(NSDictionary *event) {
                            SessionRecorderLifecycle *module = weakSelf;
                            if (!module) return;
                            @synchronized (module) {
                                if (module->_invalidated || ![module->_touchObservationToken
                                    isEqualToString:event[@"activationToken"]]) return;
                                // RCTEventEmitter queues RN runtime delivery; no synchronous JS call.
                                [module sendEventWithName:@"SessionRecorderGesture" body:event];
                            }
                        }];
                }
                __weak SessionRecorderTouchObserver *weakTouchObserver = _touchObserver;
                _owner->setActiveTouchCallback([weakTouchObserver](const std::string &activation, uint64_t pointerId,
                    SurfaceId surface, const std::string &instance, uint64_t captureGeneration,
                    bool awaitingLom, const std::function<void()> &perform) {
                    SessionRecorderTouchObserver *observer = weakTouchObserver;
                    if (!observer) return false;
                    NSString *token = [NSString stringWithUTF8String:activation.c_str()];
                    return (bool)[observer withActiveTouch:pointerId activation:token surface:surface
                        instance:instance captureGeneration:captureGeneration awaitingLom:awaitingLom perform:^{ perform(); }];
                });
                _owner->setTouchRecoveryCallbacks(
                    [weakTouchObserver](const lomtree::PointerDownRecovery &recovery) {
                        SessionRecorderTouchObserver *observer = weakTouchObserver;
                        if (!observer) return false;
                        const auto current = [observer recoveryForSurface:recovery.surface];
                        return current && current->matches(recovery);
                    },
                    [weakTouchObserver](const lomtree::PointerDownRecovery &recovery) {
                        SessionRecorderTouchObserver *observer = weakTouchObserver;
                        if (observer) [observer finishRecovery:recovery];
                    });
                _owner->setScanClock([weakTouchObserver]() -> lomtree::ScanClockSample {
                    SessionRecorderTouchObserver *observer = weakTouchObserver;
                    return observer ? [observer scanClockSample] : lomtree::ScanClockSample{};
                });
                _touchObservationToken = [activationToken copy];
                [_touchObserver startTouchObservation:_touchObservationToken];
            }
        } catch (...) {}
    } @catch (NSException *exception) {}
}

- (void)stopTouchObservation:(NSString *)activationToken
{
    @try {
        try {
            @synchronized (self) {
                if (![_touchObservationToken isEqualToString:activationToken]) return;
                _touchObservationToken = nil;
                [_touchObserver stopTouchObservation:activationToken];
            }
        } catch (...) {}
    } @catch (NSException *exception) {}
}

// Internal token-scoped scroll activation.
- (void)startScrollObservation:(NSString *)activationToken
{
    @try {
        try {
            @synchronized (self) {
                if (_invalidated || activationToken.length == 0 || !self.surfacePresenter) return;
                if (!_scrollObserver) {
                    __weak SessionRecorderLifecycle *weakSelf = self;
                    _scrollObserver = [[SessionRecorderScrollObserver alloc]
                        initWithSurfacePresenter:self.surfacePresenter eventSink:^(NSString *name, NSDictionary *event) {
                            SessionRecorderLifecycle *module = weakSelf;
                            if (!module) return;
                            @synchronized (module) {
                                if (module->_invalidated || ![module->_scrollObservationToken
                                    isEqualToString:event[@"activationToken"]]) return;
                                [module sendEventWithName:name body:event];
                            }
                        }];
                }
                _scrollObservationToken = [activationToken copy];
                [_scrollObserver startScrollObservation:_scrollObservationToken];
            }
        } catch (...) {}
    } @catch (NSException *exception) {}
}

- (void)stopScrollObservation:(NSString *)activationToken
{
    @try {
        try {
            @synchronized (self) {
                if (![_scrollObservationToken isEqualToString:activationToken]) return;
                _scrollObservationToken = nil;
                [_scrollObserver stopScrollObservation:activationToken];
            }
        } catch (...) {}
    } @catch (NSException *exception) {}
}

- (void)snapshotScrollSessions:(NSArray *)requests resolve:(RCTPromiseResolveBlock)resolve
                        reject:(RCTPromiseRejectBlock)reject
{
    @try {
        try {
            SessionRecorderScrollObserver *observer;
            @synchronized (self) { observer = _invalidated ? nil : _scrollObserver; }
            if (!observer || ![requests isKindOfClass:[NSArray class]]) { resolve(@[]); return; }
            [observer snapshotScrollSessions:requests completion:^(NSArray *results) { resolve(results); }];
        } catch (...) { resolve(@[]); }
    } @catch (NSException *exception) { resolve(@[]); }
}

@end
