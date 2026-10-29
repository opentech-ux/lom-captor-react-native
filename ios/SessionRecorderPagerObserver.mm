#import "SessionRecorderPagerObserver.h"
#import <React/RCTSurfacePresenter.h>
#import <React/RCTScheduler.h>
#import <React/RCTMountingManager.h>
#import <React/RCTComponentViewRegistry.h>
#import <objc/runtime.h>
#include <climits>

@class SessionRecorderPagerObserver;
@interface SessionRecorderPagerRecord : NSObject
@property (nonatomic, weak) SessionRecorderPagerObserver *owner;
@property (nonatomic, weak) UIView *view;
@property (nonatomic) NSInteger surface;
@property (nonatomic) NSInteger tag;
@property (nonatomic) uint64_t instance;
@property (nonatomic) uint64_t nativeInstance;
@property (nonatomic, copy) NSString *activation;
@end
@implementation SessionRecorderPagerRecord
@end

@interface SessionRecorderPagerObserver () <RCTSurfacePresenterObserver>
- (void)reconcile;
- (void)publish:(SessionRecorderPagerRecord *)record phase:(int)phase index:(int)index;
@end

namespace {
    const void *RecordKey() { return NSSelectorFromString(@"SessionRecorderPagerRecordAssociation"); }
    bool InstallPagerHooks()
    {
        Class cls = NSClassFromString(@"RNCPagerViewComponentView");
        if (!cls) return false;
        @synchronized (cls) {
            if (objc_getAssociatedObject(cls, NSSelectorFromString(@"SessionRecorderPagerHooksInstalled"))) return true;
            SEL selected = NSSelectorFromString(@"onPageSelectedWithPosition:");
            SEL phase = NSSelectorFromString(@"onPageScrollStateChangedWithState:");
            Method selectedMethod = class_getInstanceMethod(cls, selected);
            Method phaseMethod = class_getInstanceMethod(cls, phase);
            if (!selectedMethod || !phaseMethod || method_getNumberOfArguments(selectedMethod) != 3 ||
                method_getNumberOfArguments(phaseMethod) != 3) return false;
            IMP originalSelected = method_getImplementation(selectedMethod);
            IMP originalPhase = method_getImplementation(phaseMethod);
            method_setImplementation(selectedMethod, imp_implementationWithBlock(^(id view, NSInteger index) {
                @try {
                    if ([NSThread isMainThread] && object_getClass(view) == cls) {
                        SessionRecorderPagerRecord *record = objc_getAssociatedObject(view, RecordKey());
                        [record.owner publish:record phase:-1 index:(index >= 0 && index <= INT_MAX ? (int)index : INT_MIN)];
                    }
                } @finally { ((void (*)(id, SEL, NSInteger))originalSelected)(view, selected, index); }
            }));
            method_setImplementation(phaseMethod, imp_implementationWithBlock(^(id view, NSInteger state) {
                @try {
                    if ([NSThread isMainThread] && object_getClass(view) == cls && state >= 0 && state <= 2) {
                        SessionRecorderPagerRecord *record = objc_getAssociatedObject(view, RecordKey());
                        [record.owner publish:record phase:(int)state index:-1];
                    }
                } @finally { ((void (*)(id, SEL, NSInteger))originalPhase)(view, phase, state); }
            }));
            objc_setAssociatedObject(cls, NSSelectorFromString(@"SessionRecorderPagerHooksInstalled"), @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            return true;
        }
    }
}

@implementation SessionRecorderPagerObserver
{
    __weak RCTSurfacePresenter *_presenter;
    std::shared_ptr<lomtree::SessionRecorderPagerRegistry> _registry;
    void (^_sink)(NSDictionary *);
    NSString *_activation;
    NSMutableDictionary<NSNumber *, SessionRecorderPagerRecord *> *_records;
    BOOL _observing;
    uint64_t _lastVersion;
    uint64_t _nextNativeInstance;
}
- (instancetype)initWithPresenter:(RCTSurfacePresenter *)presenter
                         registry:(std::shared_ptr<lomtree::SessionRecorderPagerRegistry>)registry
                             sink:(void (^)(NSDictionary *))sink
{
    if ((self = [super init])) {
        _presenter = presenter;
        _registry = std::move(registry);
        _sink = [sink copy];
        _records = [NSMutableDictionary new];
    }
    return self;
}
- (void)start:(NSString *)activation
{
    @synchronized (self) { _activation = [activation copy]; }
    _registry->start(activation.UTF8String);
    dispatch_async(dispatch_get_main_queue(), ^{
        @synchronized (self) { if (![self->_activation isEqualToString:activation]) return; }
        if (!InstallPagerHooks()) return;
        if (!self->_observing) { [self->_presenter addObserver:self]; self->_observing = YES; }
        [self reconcile];
    });
}
- (void)stop
{
    NSString *activation;
    @synchronized (self) { activation = _activation; _activation = nil; }
    if (activation) _registry->stop(activation.UTF8String);
    dispatch_async(dispatch_get_main_queue(), ^{
        for (SessionRecorderPagerRecord *record in self->_records.allValues) {
            if (![record.activation isEqualToString:activation]) continue;
            if (objc_getAssociatedObject(record.view, RecordKey()) == record)
                objc_setAssociatedObject(record.view, RecordKey(), nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [self->_records removeObjectForKey:@(record.tag)];
        }
        @synchronized (self) { if (self->_activation) return; }
        if (self->_observing) [self->_presenter removeObserver:self];
        self->_observing = NO;
    });
}
- (void)emit:(lomtree::SessionRecorderPagerRegistry::Update)update activation:(NSString *)activation
{
    if (update.version == _lastVersion && !update.capture) return;
    _lastVersion = update.version;
    _sink(@{@"activationToken": activation, @"version": @(update.version),
            @"active": @(update.active), @"capture": @(update.capture)});
}
- (void)publish:(SessionRecorderPagerRecord *)record phase:(int)phase index:(int)index
{
    NSString *activation;
    @synchronized (self) { activation = _activation; }
    if (!activation || ![activation isEqualToString:record.activation] || !record.view ||
        _records[@(record.tag)] != record) return;
    auto update = _registry->publish(activation.UTF8String, (int)record.surface, (int)record.tag, record.instance,
                                    record.nativeInstance, phase, index);
    [self emit:update activation:activation];
}
- (void)reconcile
{
    NSString *activation;
    @synchronized (self) { activation = _activation; }
    if (!activation) return;
    auto manager = _presenter.scheduler.uiManager;
    if (!manager) { [self stop]; return; }
    auto candidates = _registry->reconcile(activation.UTF8String, *manager);
    RCTComponentViewRegistry *views = _presenter.mountingManager.componentViewRegistry;
    NSMutableSet<NSNumber *> *seen = [NSMutableSet new];
    Class cls = NSClassFromString(@"RNCPagerViewComponentView");
    for (const auto &candidate : candidates) {
        UIView *view = [views findComponentViewWithTag:candidate.tag];
        NSNumber *tag = @(candidate.tag);
        SessionRecorderPagerRecord *record = _records[tag];
        if (!view || object_getClass(view) != cls || view.tag != candidate.tag) continue;
        [seen addObject:tag];
        if (record && (record.view != view || record.instance != candidate.instance ||
            ![record.activation isEqualToString:activation])) {
            _registry->detach(record.activation.UTF8String, (int)record.surface, (int)record.tag, record.instance,
                              record.nativeInstance);
            if (objc_getAssociatedObject(record.view, RecordKey()) == record)
                objc_setAssociatedObject(record.view, RecordKey(), nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            record = nil;
        }
        if (!record) {
            record = [SessionRecorderPagerRecord new];
            record.owner = self; record.view = view; record.surface = candidate.surface;
            record.tag = candidate.tag; record.instance = candidate.instance; record.activation = activation;
            record.nativeInstance = ++_nextNativeInstance;
            _records[tag] = record;
            objc_setAssociatedObject(view, RecordKey(), record, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            _registry->publish(activation.UTF8String, candidate.surface, candidate.tag, candidate.instance,
                               record.nativeInstance, -1, -1);
        }
    }
    for (NSNumber *tag in _records.allKeys) if (![seen containsObject:tag]) {
        SessionRecorderPagerRecord *record = _records[tag];
        _registry->detach(record.activation.UTF8String, (int)record.surface, (int)record.tag, record.instance,
                          record.nativeInstance);
        if (objc_getAssociatedObject(record.view, RecordKey()) == record)
            objc_setAssociatedObject(record.view, RecordKey(), nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [_records removeObjectForKey:tag];
    }
    [self emit:_registry->status() activation:activation];
}

- (void)didMountComponentsWithRootTag:(NSInteger)rootTag
{
    [self reconcile];
    dispatch_async(dispatch_get_main_queue(), ^{ [self reconcile]; });
}
@end
