#import "SessionRecorderScreenVisibilityObserver.h"
#import <React/RCTScheduler.h>
#import <React/RCTMountingManager.h>
#import <React/RCTComponentViewRegistry.h>
#import <objc/runtime.h>
#include <react/renderer/mounting/ShadowTree.h>
#include <react/renderer/components/root/RootShadowNode.h>
#include <cstdlib>
#include <cstring>

using namespace facebook::react;
using Registry = lomtree::SessionRecorderScreenVisibilityRegistry;

static BOOL VisibilityStep(dispatch_block_t step)
{
    @try { try { step(); return YES; } catch (...) {} }
    @catch (NSException *exception) {}
    return NO;
}

@interface SessionRecorderScreenVisibilityRecord : NSObject
{
@public
    Registry::Identity identity;
}
@property (nonatomic, weak) UIView *view;
@property (nonatomic, weak) SessionRecorderScreenVisibilityObserver *owner;
@property (nonatomic, copy) NSString *activation;
@property (nonatomic) uint64_t instance;
@end
@implementation SessionRecorderScreenVisibilityRecord
@end

@interface SessionRecorderScreenVisibilityObserver () <RCTSurfacePresenterObserver>
- (void)resolve:(const std::vector<Registry::Identity> &)candidates activation:(NSString *)activation;
- (void)willMove:(SessionRecorderScreenVisibilityRecord *)record;
- (void)didMove:(SessionRecorderScreenVisibilityRecord *)record;
@end

namespace {
    const void *RecordKey() { return NSSelectorFromString(@"SessionRecorderScreenVisibilityRecord"); }

    bool InstallWindowHook(Class cls, SEL selector, bool willMove)
    {
        if (!cls || cls == [UIView class]) return false;
        @synchronized (cls) {
            const void *key = NSSelectorFromString(willMove
                ? @"SessionRecorderScreenWillMoveHook" : @"SessionRecorderScreenDidMoveHook");
            if (objc_getAssociatedObject(cls, key)) return true;
            Method inherited = class_getInstanceMethod(cls, selector);
            if (!inherited || method_getNumberOfArguments(inherited) != (willMove ? 3u : 2u)) return false;
            IMP original = method_getImplementation(inherited);
            Method ownMethod = nullptr;
            unsigned count = 0;
            Method *methods = class_copyMethodList(cls, &count);
            for (unsigned i = 0; i < count; ++i)
                if (method_getName(methods[i]) == selector) ownMethod = methods[i];
            free(methods);
            IMP replacement;
            if (willMove) {
                replacement = imp_implementationWithBlock(^(UIView *view, UIWindow *window) {
                    // Invalidate before UIKit changes attachment, including a reattach
                    // with no Fabric commit. Always preserve the original implementation.
                    VisibilityStep(^{
                        SessionRecorderScreenVisibilityRecord *record = objc_getAssociatedObject(view, RecordKey());
                        [record.owner willMove:record];
                    });
                    ((void (*)(id, SEL, UIWindow *))original)(view, selector, window);
                });
            } else {
                replacement = imp_implementationWithBlock(^(UIView *view) {
                    ((void (*)(id, SEL))original)(view, selector);
                    VisibilityStep(^{
                        SessionRecorderScreenVisibilityRecord *record = objc_getAssociatedObject(view, RecordKey());
                        [record.owner didMove:record];
                    });
                });
            }
            // Never change an inherited UIView method globally.
            if (ownMethod) method_setImplementation(ownMethod, replacement);
            else if (!class_addMethod(cls, selector, replacement, method_getTypeEncoding(inherited))) {
                imp_removeBlock(replacement);
                return false;
            }
            objc_setAssociatedObject(cls, key, @YES, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            return true;
        }
    }

    bool MatchesMountedNode(const Registry::Identity &identity, const RootShadowNode &root)
    {
        if (!identity.family || root.getSurfaceId() != identity.surface || root.getFamilyShared() != identity.rootFamily)
            return false;
        auto ancestors = identity.family->getAncestors(root);
        if (ancestors.empty()) return false;
        const auto &parent = ancestors.back();
        const auto &children = parent.first.get().getChildren();
        if (parent.second < 0 || static_cast<size_t>(parent.second) >= children.size()) return false;
        const auto &node = children[parent.second];
        const char *name = node ? node->getComponentName() : nullptr;
        return node && node->getFamilyShared() == identity.family && node->getTag() == identity.tag &&
            node->getSurfaceId() == identity.surface && name && std::strcmp(name, "RNSScreen") == 0;
    }
}

@implementation SessionRecorderScreenVisibilityObserver
{
    __weak RCTSurfacePresenter *_presenter;
    std::weak_ptr<UIManager> _manager;
    std::shared_ptr<Registry> _registry;
    NSString *_activation; // Protected by @synchronized(self).
    NSString *_installedActivation; // Main-thread only below.
    NSMutableDictionary<NSNumber *, SessionRecorderScreenVisibilityRecord *> *_records;
    NSMutableSet<NSNumber *> *_mounting;
    BOOL _observing;
    uint64_t _nextInstance;
}

- (instancetype)initWithPresenter:(RCTSurfacePresenter *)presenter registry:(std::shared_ptr<Registry>)registry
{
    if ((self = [super init])) {
        _presenter = presenter;
        auto scheduler = presenter.scheduler;
        _manager = scheduler ? scheduler.uiManager : nullptr;
        _registry = std::move(registry);
        _records = [NSMutableDictionary new];
        _mounting = [NSMutableSet new];
        __weak SessionRecorderScreenVisibilityObserver *weakOwner = self;
        _registry->setRequestCallback([weakOwner](std::string token, std::vector<Registry::Identity> candidates) {
            SessionRecorderScreenVisibilityObserver *target = weakOwner;
            if (!target) return;
            // Order discovery after the main-thread installation queued by start.
            @synchronized (target) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    SessionRecorderScreenVisibilityObserver *owner = weakOwner;
                    if (!owner) return;
                    NSString *activation = [NSString stringWithUTF8String:token.c_str()];
                    if (!VisibilityStep(^{ [owner resolve:candidates activation:activation]; })) {
                        // Unknown after an unsuccessful resolution; never publish a negative.
                        for (const auto &identity : candidates) {
                            owner->_registry->invalidateSurface(identity.surface);
                            owner->_registry->complete(token, identity);
                        }
                    }
                });
            }
        });
    }
    return self;
}

- (BOOL)isActive:(NSString *)activation
{
    @synchronized (self) { return activation && [_activation isEqualToString:activation]; }
}

- (void)forgetRecord:(SessionRecorderScreenVisibilityRecord *)record
{
    if (!record) return;
    if (objc_getAssociatedObject(record.view, RecordKey()) == record)
        objc_setAssociatedObject(record.view, RecordKey(), nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    NSNumber *key = @(Registry::key(record->identity.surface, record->identity.tag));
    if (_records[key] == record) [_records removeObjectForKey:key];
}

- (void)removeRecord:(SessionRecorderScreenVisibilityRecord *)record
{
    if (!record) return;
    _registry->remove(record.activation.UTF8String, record->identity, record.instance);
    [self forgetRecord:record];
}

- (void)start:(NSString *)activation
{
    if (!activation.length) return;
    @synchronized (self) {
        _activation = [activation copy];
        _registry->start(activation.UTF8String);
        dispatch_async(dispatch_get_main_queue(), ^{
            VisibilityStep(^{
                if (![self isActive:activation]) return;
                if (![self->_installedActivation isEqualToString:activation]) {
                    for (SessionRecorderScreenVisibilityRecord *record in self->_records.allValues) [self removeRecord:record];
                    [self->_mounting removeAllObjects];
                    self->_installedActivation = [activation copy];
                }
                if (!self->_observing) {
                    [self->_presenter addObserver:self];
                    self->_observing = YES;
                }
            });
        });
    }
}

- (void)stop
{
    NSString *activation;
    @synchronized (self) { activation = _activation; _activation = nil; }
    if (activation) _registry->stop(activation.UTF8String);
    dispatch_async(dispatch_get_main_queue(), ^{
        VisibilityStep(^{
            for (SessionRecorderScreenVisibilityRecord *record in self->_records.allValues)
                if ([record.activation isEqualToString:activation]) [self removeRecord:record];
            if ([self isActive:self->_installedActivation]) return;
            if (self->_observing) [self->_presenter removeObserver:self];
            self->_observing = NO;
            self->_installedActivation = nil;
            [self->_mounting removeAllObjects];
        });
    });
}

// Called only on main, outside mount instructions or from the post-mount hook.
// On iOS pullTransaction and mutations run synchronously on main; _mounting
// prevents treating its advanced base revision as completed during mutations.
- (BOOL)readSurface:(SurfaceId)surface root:(RootShadowNode::Shared &)root revision:(int64_t &)revision
{
    if ([_mounting containsObject:@(surface)]) return NO;
    auto manager = _manager.lock();
    RCTScheduler *scheduler = _presenter.scheduler;
    if (!manager || !scheduler || scheduler.uiManager != manager) return NO;
    manager->getShadowTreeRegistry().visit(surface, [&](const ShadowTree &tree) {
        auto mounted = tree.getMountingCoordinator()->getBaseRevision();
        root = mounted.rootShadowNode;
        revision = mounted.number;
    });
    return root != nullptr;
}

- (BOOL)ownsRecord:(SessionRecorderScreenVisibilityRecord *)record
{
    return record && [self isActive:record.activation] &&
        [_installedActivation isEqualToString:record.activation] && record.view &&
        _records[@(Registry::key(record->identity.surface, record->identity.tag))] == record;
}

- (BOOL)readRecord:(SessionRecorderScreenVisibilityRecord *)record root:(const RootShadowNode &)root
          revision:(int64_t)revision observation:(Registry::Observation &)observation
{
    UIView *view = record.view;
    if (![self ownsRecord:record] || view.tag != record->identity.tag ||
        !MatchesMountedNode(record->identity, root) ||
        [_presenter.mountingManager.componentViewRegistry findComponentViewWithTag:record->identity.tag] != view) {
        return NO;
    }
    observation = {record->identity, record.instance, revision, true, view.window != nil};
    return YES;
}

- (void)publish:(SessionRecorderScreenVisibilityRecord *)record root:(const RootShadowNode &)root revision:(int64_t)revision
{
    Registry::Observation observation;
    if ([self readRecord:record root:root revision:revision observation:observation]) {
        _registry->publish(record.activation.UTF8String, std::move(observation));
        if (self.onPublished) self.onPublished(record->identity.surface);
    } else [self removeRecord:record];
}

- (void)resolve:(const std::vector<Registry::Identity> &)candidates activation:(NSString *)activation
{
    if (![self isActive:activation] || ![_installedActivation isEqualToString:activation]) {
        for (const auto &identity : candidates) _registry->complete(activation.UTF8String, identity);
        return;
    }
    RootShadowNode::Shared root;
    int64_t revision = -1;
    SurfaceId lastSurface = -1;
    for (const auto &identity : candidates) {
        if (identity.surface != lastSurface) {
            lastSurface = identity.surface;
            root.reset();
            revision = -1;
            [self readSurface:identity.surface root:root revision:revision];
        }
        if (root && MatchesMountedNode(identity, *root)) {
            UIView *view = [_presenter.mountingManager.componentViewRegistry findComponentViewWithTag:identity.tag];
            if (view && view.tag == identity.tag &&
                InstallWindowHook(object_getClass(view), @selector(willMoveToWindow:), true) &&
                InstallWindowHook(object_getClass(view), @selector(didMoveToWindow), false)) {
                NSNumber *key = @(Registry::key(identity.surface, identity.tag));
                SessionRecorderScreenVisibilityRecord *record = _records[key];
                if (record && (record.view != view || !record->identity.matches(identity) ||
                    ![record.activation isEqualToString:activation])) {
                    [self removeRecord:record];
                    record = nil;
                }
                // A view already accredited by a different runtime is ambiguous.
                id associated = objc_getAssociatedObject(view, RecordKey());
                if (!associated || associated == record) {
                    if (!record) {
                        record = [SessionRecorderScreenVisibilityRecord new];
                        record->identity = identity;
                        record.view = view;
                        record.owner = self;
                        record.activation = activation;
                        record.instance = ++_nextInstance;
                        _records[key] = record;
                        objc_setAssociatedObject(view, RecordKey(), record, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                    }
                    [self publish:record root:*root revision:revision];
                }
            }
        }
        _registry->complete(activation.UTF8String, identity);
    }
}

- (void)willMove:(SessionRecorderScreenVisibilityRecord *)record
{
    if ([self ownsRecord:record])
        _registry->invalidate(record.activation.UTF8String, record->identity, record.instance);
}

- (void)didMove:(SessionRecorderScreenVisibilityRecord *)record
{
    if (![self ownsRecord:record] || [_mounting containsObject:@(record->identity.surface)]) return;
    RootShadowNode::Shared root;
    int64_t revision = -1;
    if ([self readSurface:record->identity.surface root:root revision:revision])
        [self publish:record root:*root revision:revision];
    else [self removeRecord:record];
}

- (void)willMountComponentsWithRootTag:(NSInteger)rootTag
{
    if (![self isActive:_installedActivation]) return;
    // Mounting alone does not change RNSScreen visibility evidence.
    // Keep reads gated until didMount publishes the completed mounted revision.
    [_mounting addObject:@(rootTag)];
}

- (void)didMountComponentsWithRootTag:(NSInteger)rootTag
{
    if (![self isActive:_installedActivation]) return;
    [_mounting removeObject:@(rootTag)];
    if (!VisibilityStep(^{
        RootShadowNode::Shared root;
        int64_t revision = -1;
        BOOL mounted = [self readSurface:(SurfaceId)rootTag root:root revision:revision];
        std::vector<Registry::Observation> observations;
        for (SessionRecorderScreenVisibilityRecord *record in self->_records.allValues) {
            if (record->identity.surface != rootTag) continue;
            Registry::Observation observation;
            if (mounted && [self readRecord:record root:*root revision:revision observation:observation])
                observations.push_back(std::move(observation));
            else [self forgetRecord:record];
        }
        if (mounted) {
            self->_registry->publishSurface(self->_installedActivation.UTF8String,
                (SurfaceId)rootTag, std::move(observations));
            if (self.onPublished) self.onPublished(rootTag);
        } else self->_registry->removeSurface((SurfaceId)rootTag);
    })) _registry->invalidateSurface((SurfaceId)rootTag);
}
@end
