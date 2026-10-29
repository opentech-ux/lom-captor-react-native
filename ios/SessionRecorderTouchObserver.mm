#import "SessionRecorderTouchObserver.h"
#import "SessionRecorderInputOwnership.h"

#import <UIKit/UIKit.h>
#import <UIKit/UIGestureRecognizerSubclass.h>
#import <QuartzCore/QuartzCore.h>
#import <React/RCTComponentViewRegistry.h>
#import <React/RCTMountingManager.h>
#import <React/RCTScheduler.h>
#import <React/RCTSurfacePresenter.h>
#import <React/RCTSurfaceTouchHandler.h>
#import <React/RCTSurfaceView.h>
#import <React/UIView+React.h>

#include <react/renderer/mounting/ShadowTree.h>
#include <react/renderer/mounting/ShadowTreeRegistry.h>
#include <react/renderer/uimanager/UIManager.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <mutex>

using namespace facebook::react;
using namespace sessionrecorder;

// Contain only SDK observer work, including Objective-C and C++ discovery errors.
static BOOL TouchStep(dispatch_block_t step)
{
    @try { try { step(); return YES; } catch (...) {} }
    @catch (NSException *exception) {}
    return NO;
}

static RCTSurfaceTouchHandler *TouchHandlerOnView(UIView *view)
{
    for (UIGestureRecognizer *recognizer in view.gestureRecognizers) {
        if ([recognizer isKindOfClass:[RCTSurfaceTouchHandler class]]) {
            return (RCTSurfaceTouchHandler *)recognizer;
        }
    }
    return nil;
}

static UIView *NearestTouchRoot(UIView *view)
{
    for (UIView *candidate = view; candidate; candidate = candidate.superview) {
        RCTSurfaceTouchHandler *handler = nil;
        for (UIGestureRecognizer *recognizer in candidate.gestureRecognizers) {
            if (![recognizer isKindOfClass:[RCTSurfaceTouchHandler class]]) continue;
            if (handler) return nil; // Ambiguous root: do not fall back to an outer owner.
            handler = (RCTSurfaceTouchHandler *)recognizer;
        }
        if (handler) return candidate;
    }
    return nil;
}

// A presented root is either its own Fabric host, or the controller root
// directly containing an exact mounted child of a Fabric ModalHostView.
static BOOL MatchesPresentedAnchor(UIView *touchRoot, UIView *anchor, Tag tag,
                                   const std::shared_ptr<const ShadowNodeFamily> &family,
                                   UIView *modalHost,
                                   const std::shared_ptr<const ShadowNodeFamily> &modalFamily,
                                   const RootShadowNode &mountedRoot,
                                   RCTComponentViewRegistry *registry)
{
    if (!touchRoot || !anchor || !family || anchor.tag != tag ||
        [registry findComponentViewWithTag:tag] != anchor) return NO;
    auto ancestors = family->getAncestors(mountedRoot);
    if (ancestors.empty()) return NO;
    const auto &parent = ancestors.back();
    const auto &children = parent.first.get().getChildren();
    if (parent.second < 0 || static_cast<size_t>(parent.second) >= children.size()) return NO;
    const auto &node = children[parent.second];
    if (!node || node->getFamilyShared() != family || node->getTag() != tag ||
        node->getSurfaceId() != mountedRoot.getSurfaceId()) return NO;
    if (anchor == touchRoot) return !modalHost && !modalFamily;

    const auto &modal = parent.first.get();
    const char *name = modal.getComponentName();
    if (!modalHost || !modalFamily || !name || std::strcmp(name, "ModalHostView") != 0 ||
        modal.getFamilyShared() != modalFamily || modal.getSurfaceId() != mountedRoot.getSurfaceId() ||
        modalHost.tag != modal.getTag() || [registry findComponentViewWithTag:modal.getTag()] != modalHost ||
        anchor.superview != touchRoot || NearestTouchRoot(anchor) != touchRoot ||
        !touchRoot.window || anchor.window != touchRoot.window || modalHost.window != touchRoot.window) return NO;

    UIResponder *responder = touchRoot.nextResponder;
    if (![responder isKindOfClass:[UIViewController class]]) return NO;
    UIViewController *controller = (UIViewController *)responder;
    if (!controller.isViewLoaded || controller.view != touchRoot || !controller.presentingViewController) return NO;
    // UIKit can forward presentation to an ancestor of the host's controller.
    for (UIViewController *presenter = [modalHost reactViewController]; presenter;
         presenter = presenter.parentViewController) {
        if (presenter == controller.presentingViewController && presenter.presentedViewController == controller) return YES;
    }
    return NO;
}

struct PresentedTouchProjection {
    CGRect bounds;
    CGPoint origin;
    CGPoint unitXPoint;
    CGPoint unitYPoint;
};

// Only affine, finite, non-degenerate UIKit projections are accredited. The
// original bounds/projection remain frozen for the lifetime of this registration.
static BOOL ReadPresentedTouchProjection(UIView *view, UIView *viewport, PresentedTouchProjection &projection)
{
    if (!view || !viewport || !view.window || view.window != viewport.window) return NO;
    for (UIView *source : {view, viewport}) {
        for (UIView *ancestor = source; ancestor; ancestor = ancestor.superview) {
            if (!CATransform3DIsAffine(ancestor.layer.transform) ||
                !CATransform3DIsAffine(ancestor.layer.sublayerTransform)) return NO;
            if (ancestor == source.window) break;
        }
    }
    CGRect bounds = view.bounds;
    if (!std::isfinite(bounds.origin.x) || !std::isfinite(bounds.origin.y) ||
        !std::isfinite(bounds.size.width) || !std::isfinite(bounds.size.height) ||
        bounds.size.width <= 0 || bounds.size.height <= 0) return NO;
    CGPoint origin = [view convertPoint:CGPointZero toView:viewport];
    CGPoint unitXPoint = [view convertPoint:CGPointMake(1, 0) toView:viewport];
    CGPoint unitYPoint = [view convertPoint:CGPointMake(0, 1) toView:viewport];
    for (CGPoint point : {origin, unitXPoint, unitYPoint})
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return NO;
    CGFloat determinant = (unitXPoint.x - origin.x) * (unitYPoint.y - origin.y) - (unitXPoint.y - origin.y) * (unitYPoint.x - origin.x);
    if (!std::isfinite(determinant) || determinant == 0) return NO;
    CGRect projectedBounds = [view convertRect:bounds toView:viewport];
    if (!std::isfinite(projectedBounds.origin.x) || !std::isfinite(projectedBounds.origin.y) ||
        !std::isfinite(projectedBounds.size.width) || !std::isfinite(projectedBounds.size.height) ||
        projectedBounds.size.width <= 0 || projectedBounds.size.height <= 0) return NO;
    projection = {bounds, origin, unitXPoint, unitYPoint};
    return YES;
}

static BOOL SamePresentedTouchProjection(const PresentedTouchProjection &a, const PresentedTouchProjection &b)
{
    return CGRectEqualToRect(a.bounds, b.bounds) && CGPointEqualToPoint(a.origin, b.origin) &&
        CGPointEqualToPoint(a.unitXPoint, b.unitXPoint) && CGPointEqualToPoint(a.unitYPoint, b.unitYPoint);
}

@class SessionRecorderPassiveTouchRecognizer;

@interface SessionRecorderTouchRoot : NSObject
{
@public
    // Root family identity, not a fabricated JS surfaceInstance number.
    ShadowNodeFamily::Shared family;
    std::shared_ptr<const InputEpoch> epoch;
    ShadowNodeFamily::Shared presentedFamily;
    ShadowNodeFamily::Shared modalFamily;
    PresentedTouchProjection projection;
}
@property (nonatomic, weak) UIView *view;
@property (nonatomic, weak) UIView *viewportView;
@property (nonatomic, weak) RCTSurfaceTouchHandler *handler;
@property (nonatomic, strong) SessionRecorderPassiveTouchRecognizer *recognizer;
@property (nonatomic, copy) NSString *token;
@property (nonatomic) NSInteger surfaceId;
@property (nonatomic) CGSize viewportSize;
@property (nonatomic) BOOL coordinatesKnown;
@property (nonatomic) BOOL presented;
@property (nonatomic) NSInteger presentedTag;
@property (nonatomic, weak) UIView *presentedAnchor;
@property (nonatomic, weak) UIView *modalHost;
@property (nonatomic, strong) NSPointerArray *presentationPath;
@property (nonatomic) NSUInteger contacts;
@property (nonatomic) NSUInteger generation;
@property (nonatomic) BOOL revoked;
@end
@implementation SessionRecorderTouchRoot
@end

// Pending accreditation never retains a presented UIView or its Fabric family.
@interface SessionRecorderPendingPresentedRoot : NSObject
{
@public
    std::weak_ptr<const ShadowNodeFamily> family;
    std::weak_ptr<const ShadowNodeFamily> modalFamily;
    std::shared_ptr<const InputEpoch> epoch;
}
@property (nonatomic, weak) UIView *view;
@property (nonatomic, weak) UIView *anchor;
@property (nonatomic, weak) UIView *modalHost;
@property (nonatomic, weak) RCTSurfaceTouchHandler *handler;
@property (nonatomic, copy) NSString *token;
@property (nonatomic) Tag tag;
@property (nonatomic) NSUInteger remainingChecks;
@end
@implementation SessionRecorderPendingPresentedRoot
@end

// Frozen opaque Fabric identity; UIKit descendants inside WebKit are not targets.
@interface SessionRecorderWebTouchHost : NSObject
{
@public
    ShadowNodeFamily::Shared family;
    ShadowNodeFamily::Shared rootFamily;
}
@property (nonatomic, weak) UIView *view;
@property (nonatomic) NSInteger tag;
@property (nonatomic) NSInteger surfaceId;
@end
@implementation SessionRecorderWebTouchHost
@end

@interface SessionRecorderPhysicalContact : NSObject
{
@public
    std::shared_ptr<const InputEpoch> epoch;
    std::optional<lomtree::PointerDownRecovery> recovery;
}
@property (nonatomic) NSUInteger pointerId;
@property (nonatomic, strong) SessionRecorderTouchRoot *root;
@property (nonatomic) BOOL reportable;
@property (nonatomic) BOOL downForwarded;
@property (nonatomic) BOOL terminalReceived;
@property (nonatomic) NSUInteger generation;
@property (nonatomic, weak) UIView *beginView;
@property (nonatomic, strong) NSPointerArray *path;
@property (nonatomic, strong) SessionRecorderWebTouchHost *webHost;
@end
@implementation SessionRecorderPhysicalContact
@end

@interface SessionRecorderTouchObserver () <RCTSurfacePresenterObserver>
- (BOOL)acceptsTouch:(UITouch *)touch root:(SessionRecorderTouchRoot *)root;
- (void)observeTouches:(NSSet<UITouch *> *)touches type:(NSString *)type
                  root:(SessionRecorderTouchRoot *)root;
- (void)abandonRoot:(SessionRecorderTouchRoot *)root;
- (SessionRecorderWebTouchHost *)webHostForTouch:(UITouch *)touch root:(SessionRecorderTouchRoot *)root;
- (BOOL)ownsWebHost:(SessionRecorderWebTouchHost *)identity root:(SessionRecorderTouchRoot *)root;
- (BOOL)ownsPresentedRoot:(SessionRecorderTouchRoot *)root;
- (void)schedulePresentedAccreditation:(SessionRecorderPendingPresentedRoot *)pending;
@end

@interface SessionRecorderPassiveTouchRecognizer : UIGestureRecognizer <UIGestureRecognizerDelegate>
@property (nonatomic, weak) SessionRecorderTouchObserver *owner;
@property (nonatomic, weak) SessionRecorderTouchRoot *root;
@end

@implementation SessionRecorderPassiveTouchRecognizer
- (instancetype)init
{
    if ((self = [super initWithTarget:nil action:NULL])) {
        self.cancelsTouchesInView = NO;
        self.delaysTouchesBegan = NO;
        self.delaysTouchesEnded = NO;
        self.delegate = self;
    }
    return self;
}
- (BOOL)canPreventGestureRecognizer:(UIGestureRecognizer *)other { return NO; }
- (BOOL)canBePreventedByGestureRecognizer:(UIGestureRecognizer *)other { return NO; }
- (BOOL)shouldRequireFailureOfGestureRecognizer:(UIGestureRecognizer *)other { return NO; }
- (BOOL)shouldBeRequiredToFailByGestureRecognizer:(UIGestureRecognizer *)other { return NO; }
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)recognizer
    shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)other { return YES; }
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)recognizer
    shouldRequireFailureOfGestureRecognizer:(UIGestureRecognizer *)other { return NO; }
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)recognizer
    shouldBeRequiredToFailByGestureRecognizer:(UIGestureRecognizer *)other { return NO; }
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)recognizer shouldReceiveTouch:(UITouch *)touch
{
    __block BOOL accepted = NO;
    TouchStep(^{ accepted = [self.owner acceptsTouch:touch root:self.root]; });
    return accepted;
}
- (void)observe:(NSSet<UITouch *> *)touches type:(NSString *)type
{
    TouchStep(^{
        [self.owner observeTouches:touches type:type root:self.root];
        // Never recognize. Stay Possible until the last owned physical contact ends.
        if (self.root.contacts == 0) self.state = UIGestureRecognizerStateFailed;
    });
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{ [self observe:touches type:@"pointerDown"]; }
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{ [self observe:touches type:@"pointerMove"]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{ [self observe:touches type:@"pointerUp"]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event
{ [self observe:touches type:@"pointerCancel"]; }
- (void)reset
{
    TouchStep(^{ [self.owner abandonRoot:self.root]; });
    [super reset];
}
@end

@implementation SessionRecorderTouchObserver
{
    __weak RCTSurfacePresenter *_presenter;
    std::weak_ptr<UIManager> _inputManager;
    SessionRecorderTouchEventSink _eventSink;
    NSString *_authorizedToken; // Access only under @synchronized(self).
    BOOL _invalidated;

    // UIKit state is main-thread owned. Contact membership and primitive
    // adoption flags are also read under _contactMutex by the private JSI path.
    NSString *_installedToken;
    BOOL _observingMounts;
    BOOL _discoveryQueued;
    NSMutableArray<id> *_ownershipNotifications;
    NSMapTable<UIView *, SessionRecorderTouchRoot *> *_roots;
    NSMapTable<UITouch *, SessionRecorderPhysicalContact *> *_contacts;
    // Protect only contact membership/revocation and recovery eligibility.
    // Never hold across UIKit work, event delivery or lifecycle authorization.
    std::mutex _contactMutex;
    NSMapTable<UIView *, SessionRecorderWebTouchHost *> *_webHosts;
    // Main-thread, weak-host pending accreditation; independent of installed roots.
    NSMapTable<UIView *, SessionRecorderPendingPresentedRoot *> *_pendingPresentedRoots;
    NSUInteger _nextPointerId;
    NSUInteger _nextRootGeneration;
    NSString *_clockActivation; // Clock/anchors protected by @synchronized(self).
    double _epochAnchorMs;
    double _uptimeAnchor;
}

- (instancetype)initWithSurfacePresenter:(RCTSurfacePresenter *)presenter
                               eventSink:(SessionRecorderTouchEventSink)eventSink
{
    if ((self = [super init])) {
        _presenter = presenter;
        RCTScheduler *scheduler = presenter.scheduler;
        _inputManager = scheduler ? scheduler.uiManager : nullptr;
        _eventSink = [eventSink copy];
        _roots = [NSMapTable weakToStrongObjectsMapTable];
        _contacts = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsStrongMemory |
            NSPointerFunctionsObjectPointerPersonality valueOptions:NSPointerFunctionsStrongMemory];
        _webHosts = [NSMapTable weakToStrongObjectsMapTable];
        _pendingPresentedRoots = [NSMapTable weakToStrongObjectsMapTable];
    }
    return self;
}

- (BOOL)isAuthorized:(NSString *)token
{
    @synchronized (self) {
        return !_invalidated && token != nil && [_authorizedToken isEqualToString:token];
    }
}

- (void)startTouchObservation:(NSString *)activationToken
{
    TouchStep(^{
        if (activationToken.length == 0) return;
        NSString *token = [activationToken copy];
        @synchronized (self) {
            if (self->_invalidated || [self->_authorizedToken isEqualToString:token]) return;
            self->_authorizedToken = token;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            TouchStep(^{
                if (![self isAuthorized:token]) return;
                [self removeObservation];
                self->_installedToken = token;
                self->_nextPointerId = 0;
                @synchronized (self) {
                    self->_uptimeAnchor = NSProcessInfo.processInfo.systemUptime;
                    self->_epochAnchorMs = NSDate.date.timeIntervalSince1970 * 1000.0;
                    self->_clockActivation = token;
                }
                RCTSurfacePresenter *presenter = self->_presenter;
                if (!presenter) return;
                // Additive RN 0.83.2 hook. Never replace the mounting manager delegate.
                self->_observingMounts = YES;
                [presenter addObserver:self];
                self->_ownershipNotifications = [NSMutableArray new];
                __weak SessionRecorderTouchObserver *weakSelf = self;
                for (NSNotificationName name in InputOwnershipNotifications()) {
                    id observer = [NSNotificationCenter.defaultCenter addObserverForName:name object:nil
                        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
                            SessionRecorderTouchObserver *owner = weakSelf;
                            if (!owner) return;
                            TouchStep(^{
                                if (![owner isAuthorized:owner->_installedToken]) return;
                                // Revoke before UIKit can detach; activation may resume later.
                                for (UIView *view in owner->_pendingPresentedRoots.keyEnumerator.allObjects) {
                                    SessionRecorderPendingPresentedRoot *pending = [owner->_pendingPresentedRoots objectForKey:view];
                                    if ((([note.name isEqualToString:UISceneWillDeactivateNotification] ||
                                          [note.name isEqualToString:UISceneDidDisconnectNotification]) &&
                                         note.object == pending->epoch->scene) ||
                                        ([note.name isEqualToString:UIWindowDidBecomeHiddenNotification] &&
                                         note.object == pending->epoch->window))
                                        [owner->_pendingPresentedRoots removeObjectForKey:view];
                                }
                                for (SessionRecorderTouchRoot *root in owner->_roots.objectEnumerator.allObjects) {
                                    if (([note.name isEqualToString:UISceneWillDeactivateNotification] ||
                                         [note.name isEqualToString:UISceneDidDisconnectNotification]) &&
                                        note.object == root->epoch->scene) [owner removeRoot:root];
                                    else if ([note.name isEqualToString:UIWindowDidBecomeHiddenNotification] &&
                                             note.object == root->epoch->window) [owner removeRoot:root];
                                }
                                [owner didMountComponentsWithRootTag:0];
                            });
                        }];
                    [self->_ownershipNotifications addObject:observer];
                }
                [self reconcileRoots];
            });
        });
    });
}

- (void)stopTouchObservation:(NSString *)activationToken
{
    TouchStep(^{
        @synchronized (self) {
            if (![self->_authorizedToken isEqualToString:activationToken]) return;
            self->_authorizedToken = nil; // Revoke before any asynchronous main-thread cleanup.
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            TouchStep(^{
                @synchronized (self) {
                    // A stale stop may not detach a newer activation.
                    if (self->_authorizedToken != nil) return;
                }
                [self removeObservation];
            });
        });
    });
}

- (void)invalidate
{
    TouchStep(^{
        @synchronized (self) {
            if (self->_invalidated) return;
            self->_invalidated = YES;
            self->_authorizedToken = nil;
        }
        // Retain this owner until its recognizers/hooks have actually been removed.
        dispatch_async(dispatch_get_main_queue(), ^{ TouchStep(^{ [self removeObservation]; }); });
    });
}

- (void)forgetTouch:(UITouch *)touch
{
    std::lock_guard<std::mutex> lock(_contactMutex);
    SessionRecorderPhysicalContact *contact = [_contacts objectForKey:touch];
    if (!contact) return;
    if (contact.root.contacts > 0) contact.root.contacts--;
    [_contacts removeObjectForKey:touch];
}

- (lomtree::ScanClockSample)scanClockSample
{
    @synchronized (self) {
        if (_invalidated || !_clockActivation || ![_authorizedToken isEqualToString:_clockActivation]) return {};
        return {_clockActivation.UTF8String,
            _epochAnchorMs + (NSProcessInfo.processInfo.systemUptime - _uptimeAnchor) * 1000.0};
    }
}

- (BOOL)withActiveTouch:(NSUInteger)pointerId activation:(NSString *)activation
               surface:(NSInteger)surface instance:(const std::string &)instance
     captureGeneration:(uint64_t)captureGeneration awaitingLom:(BOOL)awaitingLom perform:(void (^)(void))perform
{
    @synchronized (self) {
        if (_invalidated || !activation || ![_authorizedToken isEqualToString:activation]) return NO;
        std::lock_guard<std::mutex> lock(_contactMutex);
        if (_contacts.count != 1) return NO;
        for (SessionRecorderPhysicalContact *contact in _contacts.objectEnumerator) {
            SessionRecorderTouchRoot *root = contact.root;
            if (contact.pointerId != pointerId || !contact.reportable || contact.terminalReceived ||
                root.revoked || root.surfaceId != surface || ![root.token isEqualToString:activation] ||
                contact.generation != root.generation || contact->epoch != root->epoch) return NO;
            if (awaitingLom)
                contact->recovery = lomtree::PointerDownRecovery{activation.UTF8String, pointerId,
                    (SurfaceId)surface, instance, captureGeneration};
            else contact->recovery.reset();
            perform();
            return YES;
        }
        return NO;
    }
}

- (std::optional<lomtree::PointerDownRecovery>)recoveryForSurface:(NSInteger)surface
{
    @synchronized (self) {
        if (_invalidated || !_authorizedToken) return std::nullopt;
        std::lock_guard<std::mutex> lock(_contactMutex);
        if (_contacts.count != 1) return std::nullopt;
        for (SessionRecorderPhysicalContact *contact in _contacts.objectEnumerator) {
            SessionRecorderTouchRoot *root = contact.root;
            if (contact->recovery && contact.reportable &&
                !contact.terminalReceived && !root.revoked && root.surfaceId == surface &&
                [root.token isEqualToString:_authorizedToken] && contact.generation == root.generation &&
                contact->epoch == root->epoch)
                return contact->recovery;
        }
        return std::nullopt;
    }
}

- (void)finishRecovery:(const lomtree::PointerDownRecovery &)recovery
{
    @synchronized (self) {
        std::lock_guard<std::mutex> lock(_contactMutex);
        for (SessionRecorderPhysicalContact *contact in _contacts.objectEnumerator) {
            if (contact->recovery && contact->recovery->matches(recovery)) {
                contact->recovery.reset();
            }
        }
    }
}

- (void)abandonRoot:(SessionRecorderTouchRoot *)root
{
    if (!root || root.contacts == 0) return;
    {
        std::lock_guard<std::mutex> lock(_contactMutex);
        for (SessionRecorderPhysicalContact *contact in _contacts.objectEnumerator)
            if (contact.root == root) contact.terminalReceived = YES;
    }
    NSMutableArray<NSNumber *> *pointerIds = [NSMutableArray new];
    for (UITouch *touch in _contacts.keyEnumerator.allObjects) {
        SessionRecorderPhysicalContact *contact = [_contacts objectForKey:touch];
        if (contact.root != root) continue;
        if (contact.downForwarded) [pointerIds addObject:@(contact.pointerId)];
        [self forgetTouch:touch];
    }
    // Ownership loss is a private cleanup signal, never a physical Cancel/Up.
    if (pointerIds.count && [self isAuthorized:root.token]) TouchStep(^{
        self->_eventSink(@{@"activationToken": root.token, @"type": @"ownershipAbandoned", @"pointerIds": pointerIds});
    });
}

- (void)abandonTouch:(UITouch *)touch contact:(SessionRecorderPhysicalContact *)contact
{
    if (!contact) return;
    [self forgetTouch:touch];
    if (contact.downForwarded && [self isAuthorized:contact.root.token]) TouchStep(^{
        self->_eventSink(@{@"activationToken": contact.root.token, @"type": @"ownershipAbandoned",
            @"pointerIds": @[@(contact.pointerId)]});
    });
}

- (void)removeRoot:(SessionRecorderTouchRoot *)root
{
    if (!root || root.revoked) return;
    {
        std::lock_guard<std::mutex> lock(_contactMutex);
        root.revoked = YES;
    }
    if (root.view && [_roots objectForKey:root.view] == root) [_roots removeObjectForKey:root.view];
    TouchStep(^{ [self abandonRoot:root]; });
    TouchStep(^{ [root.view removeGestureRecognizer:root.recognizer]; });
    root.recognizer.owner = nil;
    root.recognizer.root = nil;
}

- (void)removeObservation
{
    for (id observer in _ownershipNotifications) TouchStep(^{
        [NSNotificationCenter.defaultCenter removeObserver:observer];
    });
    _ownershipNotifications = nil;
    if (_observingMounts) {
        TouchStep(^{ [self->_presenter removeObserver:self]; });
        _observingMounts = NO;
    }
    for (SessionRecorderTouchRoot *root in _roots.objectEnumerator.allObjects) [self removeRoot:root];
    [_roots removeAllObjects];
    {
        std::lock_guard<std::mutex> lock(_contactMutex);
        [_contacts removeAllObjects];
    }
    [_webHosts removeAllObjects];
    [_pendingPresentedRoots removeAllObjects];
    @synchronized (self) { _clockActivation = nil; }
    _installedToken = nil;
    _discoveryQueued = NO;
}

- (void)didMountComponentsWithRootTag:(NSInteger)rootTag
{
    TouchStep(^{
        if (![self isAuthorized:self->_installedToken] || self->_discoveryQueued) return;
        self->_discoveryQueued = YES;
        NSString *token = self->_installedToken;
        // A single mount-triggered main-queue turn also permits queued ScreenStack updates.
        // No per-Move subtree DFS; opaque hosts revalidate their ancestry.
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!TouchStep(^{
                if (![self->_installedToken isEqualToString:token] || ![self isAuthorized:token]) return;
                self->_discoveryQueued = NO;
                [self reconcileRoots];
            })) {
                [self->_pendingPresentedRoots removeAllObjects];
                for (SessionRecorderTouchRoot *root in self->_roots.objectEnumerator.allObjects)
                    TouchStep(^{ [self removeRoot:root]; });
            }
        });
    });
}

// Observe only pending presentation paths, not the whole tree. The bounded
// cadence observes state; elapsed time never accredits a root.
- (void)schedulePresentedAccreditation:(SessionRecorderPendingPresentedRoot *)pending
{
    __weak SessionRecorderTouchObserver *weakOwner = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        SessionRecorderTouchObserver *owner = weakOwner;
        if (!owner) return;
        __block BOOL again = NO;
        BOOL checked = TouchStep(^{
            UIView *view = pending.view;
            if (!view || [owner->_pendingPresentedRoots objectForKey:view] != pending) return;
            // A timeout stays deduplicated until a new identity or lifecycle.
            if (pending.remainingChecks == 0) return;
            pending.remainingChecks--;
            BOOL valid = [owner isAuthorized:pending.token] &&
                [owner->_installedToken isEqualToString:pending.token] &&
                view.window && view.window == pending->epoch->window &&
                pending.handler && pending.handler.view == view && NearestTouchRoot(view) == view;
            RootShadowNode::Shared mountedRoot;
            auto currentEpoch = valid ? ReadInputEpoch(owner->_presenter, owner->_inputManager, &mountedRoot) : nullptr;
            auto family = pending->family.lock();
            valid = valid && family && mountedRoot && SameInputEpoch(pending->epoch, currentEpoch);
            if (valid) {
                valid = MatchesPresentedAnchor(view, pending.anchor, pending.tag, family,
                    pending.modalHost, pending->modalFamily.lock(), *mountedRoot,
                    owner->_presenter.mountingManager.componentViewRegistry);
            }
            if (!valid || [owner->_roots objectForKey:view]) {
                [owner->_pendingPresentedRoots removeObjectForKey:view];
                return;
            }
            PresentedTouchProjection projection;
            if (!ReadPresentedTouchProjection(view, pending->epoch->viewport, projection)) {
                [owner->_pendingPresentedRoots removeObjectForKey:view];
                return;
            }
            const auto failure = CheckInputPath(view, pending->epoch->window);
            if (failure == InputPathFailure::None) {
                [owner->_pendingPresentedRoots removeObjectForKey:view];
                // Coalesce with mount-triggered work; only normal reconciliation installs.
                [owner didMountComponentsWithRootTag:pending->epoch->surfaceId];
            } else if (failure == InputPathFailure::Animation) {
                again = pending.remainingChecks > 0;
            } else {
                [owner->_pendingPresentedRoots removeObjectForKey:view];
            }
        });
        if (!checked) {
            TouchStep(^{
                UIView *view = pending.view;
                if (view && [owner->_pendingPresentedRoots objectForKey:view] == pending)
                    [owner->_pendingPresentedRoots removeObjectForKey:view];
            });
        } else if (again) {
            [owner schedulePresentedAccreditation:pending];
        }
    });
}

- (void)reconcileRoots
{
    if (![self isAuthorized:_installedToken]) return;
    [_webHosts removeAllObjects];
    RCTSurfacePresenter *presenter = _presenter;
    RCTScheduler *scheduler = presenter.scheduler;
    auto manager = scheduler ? scheduler.uiManager : nullptr;
    if (!manager) {
        [_pendingPresentedRoots removeAllObjects];
        for (SessionRecorderTouchRoot *root in _roots.objectEnumerator.allObjects) [self removeRoot:root];
        return;
    }

    RootShadowNode::Shared baselineRoot;
    auto baseline = ReadInputEpoch(presenter, _inputManager, &baselineRoot);
    if (!baseline) {
        [_pendingPresentedRoots removeAllObjects];
        for (SessionRecorderTouchRoot *root in _roots.objectEnumerator.allObjects) [self removeRoot:root];
        return;
    }
    std::vector<RootShadowNode::Shared> mountedRoots{baselineRoot};

    RCTComponentViewRegistry *registry = presenter.mountingManager.componentViewRegistry;
    NSMapTable<UIView *, SessionRecorderTouchRoot *> *candidates = [NSMapTable weakToStrongObjectsMapTable];
    NSHashTable<UIView *> *ambiguous = [NSHashTable weakObjectsHashTable];
    NSMapTable<UIView *, SessionRecorderPendingPresentedRoot *> *deferred = [NSMapTable weakToStrongObjectsMapTable];
    for (const auto &root : mountedRoots) {
        UIView *viewport = [registry findComponentViewWithTag:root->getTag()];
        if (!viewport) continue;
        auto size = root->getLayoutMetrics().frame.size;
        if (!std::isfinite(size.width) || !std::isfinite(size.height) || size.width <= 0 || size.height <= 0) continue;
        UIView *normalRoot = NearestTouchRoot(viewport);
        std::vector<std::shared_ptr<const ShadowNode>> nodes{root};
        while (!nodes.empty()) {
            auto node = nodes.back();
            nodes.pop_back();
            UIView *view = [registry findComponentViewWithTag:node->getTag()];
            const char *name = node->getComponentName();
            if (view && name && std::strcmp(name, "RNCWebView") == 0) {
                SessionRecorderWebTouchHost *identity = [SessionRecorderWebTouchHost new];
                identity->family = node->getFamilyShared();
                identity->rootFamily = root->getFamilyShared();
                identity.surfaceId = root->getSurfaceId();
                identity.tag = node->getTag();
                identity.view = view;
                [_webHosts setObject:identity forKey:view];
            }
            UIView *touchRoot = NearestTouchRoot(view);
            if (touchRoot && ![touchRoot isKindOfClass:[UIWindow class]]) {
                SessionRecorderTouchRoot *candidate = [candidates objectForKey:touchRoot];
                if (candidate && (candidate->family != root->getFamilyShared() ||
                    (candidate.presented && view == touchRoot &&
                     (candidate->presentedFamily != node->getFamilyShared() || candidate.presentedTag != node->getTag())) ||
                    (candidate->modalFamily && view.superview == touchRoot && candidate.presentedAnchor != view))) {
                    [ambiguous addObject:touchRoot];
                } else if (!candidate) {
                    candidate = [SessionRecorderTouchRoot new];
                    candidate->family = root->getFamilyShared();
                    candidate.surfaceId = root->getSurfaceId();
                    candidate.view = touchRoot;
                    candidate.handler = TouchHandlerOnView(touchRoot);
                    candidate.viewportView = viewport;
                    candidate.viewportSize = CGSizeMake(size.width, size.height);
                    // PRIMARY keeps the original direct Surface/viewport contract.
                    candidate.coordinatesKnown = touchRoot == normalRoot && viewport.superview == touchRoot &&
                        [touchRoot isKindOfClass:[RCTSurfaceView class]];
                    if (!candidate.coordinatesKnown) {
                        candidate.presentedAnchor = view;
                        candidate.presentedTag = node->getTag();
                        candidate->presentedFamily = node->getFamilyShared();
                        if (view != touchRoot) {
                            if (view.superview != touchRoot) continue;
                            auto ancestors = node->getFamily().getAncestors(*root);
                            if (ancestors.empty()) continue;
                            const auto &modal = ancestors.back().first.get();
                            const char *modalName = modal.getComponentName();
                            if (!modalName || std::strcmp(modalName, "ModalHostView") != 0) continue;
                            candidate->modalFamily = modal.getFamilyShared();
                            candidate.modalHost = [registry findComponentViewWithTag:modal.getTag()];
                        }
                        if (!MatchesPresentedAnchor(touchRoot, view, candidate.presentedTag,
                            candidate->presentedFamily, candidate.modalHost, candidate->modalFamily, *root, registry) ||
                            node->getSurfaceId() != root->getSurfaceId() || !candidate.handler ||
                            candidate.handler.view != touchRoot || touchRoot.window != baseline->window) continue;
                        InputPathFailure failure;
                        candidate.presentationPath = InputPath(touchRoot, baseline->window, &failure);
                        if (!candidate.presentationPath) {
                            PresentedTouchProjection projection;
                            if (failure == InputPathFailure::Animation &&
                                ReadPresentedTouchProjection(touchRoot, viewport, projection)) {
                                SessionRecorderPendingPresentedRoot *pending = [SessionRecorderPendingPresentedRoot new];
                                pending.view = touchRoot;
                                pending.anchor = candidate.presentedAnchor;
                                pending.modalHost = candidate.modalHost;
                                pending.handler = candidate.handler;
                                pending->family = node->getFamilyShared();
                                pending->modalFamily = candidate->modalFamily;
                                pending->epoch = baseline;
                                pending.tag = node->getTag();
                                pending.token = _installedToken;
                                pending.remainingChecks = 50;
                                [deferred setObject:pending forKey:touchRoot];
                            }
                            continue;
                        }
                        if (!ReadPresentedTouchProjection(touchRoot, viewport, candidate->projection)) continue;
                        candidate.presented = YES;
                        candidate.coordinatesKnown = YES;
                    }
                    candidate->epoch = baseline;
                    [candidates setObject:candidate forKey:touchRoot];
                }
            }
            for (const auto &child : node->getChildren()) nodes.push_back(child);
        }
    }

    for (SessionRecorderTouchRoot *root in _roots.objectEnumerator.allObjects) {
        SessionRecorderTouchRoot *candidate = [candidates objectForKey:root.view];
        if (!candidate || [ambiguous containsObject:root.view] || candidate->family != root->family ||
            candidate.handler != root.handler || candidate.viewportView != root.viewportView ||
            root.recognizer.view != root.view || candidate.presented != root.presented ||
            (root.presented && (candidate->presentedFamily != root->presentedFamily ||
                candidate.presentedTag != root.presentedTag ||
                candidate.presentedAnchor != root.presentedAnchor || candidate.modalHost != root.modalHost ||
                candidate->modalFamily != root->modalFamily ||
                !SamePresentedTouchProjection(candidate->projection, root->projection) ||
                !SameInputPath(root.presentationPath, root.view, baseline->window)))) {
            [self removeRoot:root];
        } else if (!SameInputEpoch(candidate->epoch, root->epoch)) {
            [self removeRoot:root];
        }
    }

    for (UIView *view in candidates.keyEnumerator.allObjects) {
        if ([ambiguous containsObject:view]) continue;
        SessionRecorderTouchRoot *candidate = [candidates objectForKey:view];
        SessionRecorderTouchRoot *installed = [_roots objectForKey:view];
        if (installed) {
            installed.viewportSize = candidate.viewportSize;
            installed.coordinatesKnown = candidate.coordinatesKnown;
            continue;
        }
        BOOL attached = TouchStep(^{
            candidate.token = self->_installedToken;
            candidate.generation = ++self->_nextRootGeneration;
            candidate.recognizer = [SessionRecorderPassiveTouchRecognizer new];
            candidate.recognizer.owner = self;
            candidate.recognizer.root = candidate;
            // Register before attachment so a partial install is still owned by cleanup.
            [self->_roots setObject:candidate forKey:view];
            [view addGestureRecognizer:candidate.recognizer];
        });
        if (!attached) [self removeRoot:candidate];
    }
    for (UIView *view in _pendingPresentedRoots.keyEnumerator.allObjects) {
        SessionRecorderPendingPresentedRoot *next = [deferred objectForKey:view];
        SessionRecorderPendingPresentedRoot *pending = [_pendingPresentedRoots objectForKey:view];
        if (!next || [ambiguous containsObject:view] || pending.tag != next.tag ||
            pending->family.lock() != next->family.lock() || pending.handler != next.handler ||
            pending.anchor != next.anchor || pending.modalHost != next.modalHost ||
            pending->modalFamily.lock() != next->modalFamily.lock() ||
            !SameInputEpoch(pending->epoch, next->epoch) || ![pending.token isEqualToString:next.token])
            [_pendingPresentedRoots removeObjectForKey:view];
    }
    for (UIView *view in deferred.keyEnumerator.allObjects) {
        if ([ambiguous containsObject:view] || [candidates objectForKey:view] ||
            [_pendingPresentedRoots objectForKey:view]) continue;
        SessionRecorderPendingPresentedRoot *pending = [deferred objectForKey:view];
        [_pendingPresentedRoots setObject:pending forKey:view];
        [self schedulePresentedAccreditation:pending];
    }
    for (UITouch *touch in _contacts.keyEnumerator.allObjects) {
        SessionRecorderPhysicalContact *contact = [_contacts objectForKey:touch];
        if (contact.webHost && (![self ownsWebHost:contact.webHost root:contact.root] ||
            !SameInputPath(contact.path, contact.webHost.view, contact.root.viewportView)))
            [self abandonTouch:touch contact:contact];
    }
}

- (BOOL)hasViewport:(SessionRecorderTouchRoot *)root
{
    if (root.presented) return [self ownsPresentedRoot:root];
    UIView *view = root.view;
    UIView *viewport = root.viewportView;
    return root != nil && view != nil && !root.revoked && [_roots objectForKey:view] == root &&
        SameInputEpoch(root->epoch, ReadInputEpoch(_presenter, _inputManager)) &&
        root.coordinatesKnown && view != nil && viewport != nil && view.window != nil &&
        view.window == viewport.window && root.handler.view == view && root.recognizer.view == view &&
        viewport.superview == view &&
        CGPointEqualToPoint(viewport.bounds.origin, CGPointZero) &&
        CGSizeEqualToSize(viewport.bounds.size, root.viewportSize) &&
        CGAffineTransformIsIdentity(viewport.transform) && CATransform3DIsIdentity(viewport.layer.transform);
}

- (BOOL)ownsPresentedRoot:(SessionRecorderTouchRoot *)root
{
    UIView *view = root.view;
    if (!root || !view) return NO;
    if (!root.presented) return NO;
    if (!root->presentedFamily) return NO;
    if (root.revoked) return NO;
    if (!root.coordinatesKnown) return NO;
    if ([_roots objectForKey:view] != root) return NO;
    if (![self isAuthorized:root.token]) return NO;
    if (![_installedToken isEqualToString:root.token]) return NO;
    if (root.handler.view != view) return NO;
    if (root.recognizer.view != view) return NO;
    if (NearestTouchRoot(view) != view) return NO;

    RootShadowNode::Shared mountedRoot;
    auto currentEpoch = ReadInputEpoch(_presenter, _inputManager, &mountedRoot);
    if (!mountedRoot) return NO;
    if (!SameInputEpoch(root->epoch, currentEpoch)) return NO;
    if (root.surfaceId != currentEpoch->surfaceId) return NO;
    if (root->family != mountedRoot->getFamilyShared()) return NO;
    if (root.viewportView != currentEpoch->viewport) return NO;
    if (!CGSizeEqualToSize(root.viewportSize, currentEpoch->viewportBounds.size))
        return NO;
    if (view.window != currentEpoch->window) return NO;
    if (!SameInputPath(root.presentationPath, view, currentEpoch->window)) return NO;
    if (!MatchesPresentedAnchor(view, root.presentedAnchor, (Tag)root.presentedTag,
        root->presentedFamily, root.modalHost, root->modalFamily, *mountedRoot,
        _presenter.mountingManager.componentViewRegistry)) return NO;

    PresentedTouchProjection projection;
    if (!ReadPresentedTouchProjection(view, root.viewportView, projection))
        return NO;
    if (!SamePresentedTouchProjection(root->projection, projection))
        return NO;
    return YES;
}

- (BOOL)acceptsTouch:(UITouch *)touch root:(SessionRecorderTouchRoot *)root
{
    return root != nil && root.view != nil && root.handler != nil &&
        [self isAuthorized:root.token] && root.handler.view == root.view &&
        [self hasViewport:root] && NearestTouchRoot(touch.view) == root.view &&
        InputPath(touch.view, root.presented ? root.view : root.viewportView) != nil;
}

- (SessionRecorderWebTouchHost *)webHostForTouch:(UITouch *)touch root:(SessionRecorderTouchRoot *)root
{
    SessionRecorderWebTouchHost *identity = nil;
    for (UIView *view = touch.view; view; view = view.superview) {
        SessionRecorderWebTouchHost *candidate = [_webHosts objectForKey:view];
        if (candidate) {
            if (identity) return nil; // Never choose between nested opaque owners.
            identity = candidate;
        }
        if (view == root.viewportView) break;
    }
    return identity && [self ownsWebHost:identity root:root] ? identity : nil;
}

- (BOOL)ownsWebHost:(SessionRecorderWebTouchHost *)identity root:(SessionRecorderTouchRoot *)root
{
    UIView *host = identity.view;
    if (!identity || !root || !host || !identity->family || !root.view || root.revoked ||
        ![self isAuthorized:root.token] || [_roots objectForKey:root.view] != root ||
        identity.surfaceId != root.surfaceId || identity->rootFamily != root->family ||
        host.tag != identity.tag || NearestTouchRoot(host) != root.view ||
        [_presenter.mountingManager.componentViewRegistry findComponentViewWithTag:(Tag)identity.tag] != host)
        return NO;

    RootShadowNode::Shared mountedRoot;
    auto currentEpoch = ReadInputEpoch(_presenter, _inputManager, &mountedRoot);
    if (!mountedRoot || !SameInputEpoch(root->epoch, currentEpoch)) return NO;
    // Check the concrete family against the current mounted revision, including
    // the interval before the deferred mounting reconciliation updates _webHosts.
    auto ancestors = identity->family->getAncestors(*mountedRoot);
    if (ancestors.empty()) return NO;
    const auto &parent = ancestors.back();
    const auto &children = parent.first.get().getChildren();
    if (parent.second < 0 || static_cast<size_t>(parent.second) >= children.size()) return NO;
    const auto &node = children[parent.second];
    if (!node || node->getFamilyShared() != identity->family || node->getTag() != identity.tag ||
        node->getSurfaceId() != identity.surfaceId) return NO;
    const char *name = node->getComponentName();
    return name && std::strcmp(name, "RNCWebView") == 0;
}

- (void)observeTouches:(NSSet<UITouch *> *)touches type:(NSString *)type
                  root:(SessionRecorderTouchRoot *)root
{
    BOOL terminal = [type isEqualToString:@"pointerUp"] || [type isEqualToString:@"pointerCancel"];
    if (terminal) {
        // Terminal closes recovery before any validation or asynchronous JS delivery.
        std::lock_guard<std::mutex> lock(_contactMutex);
        for (UITouch *touch in touches) {
            SessionRecorderPhysicalContact *contact = [_contacts objectForKey:touch];
            if (contact) {
                contact.terminalReceived = YES;
                contact->recovery.reset();
            }
        }
    }
    if (!root || root.revoked || ![self isAuthorized:root.token]) return;
    __block BOOL compatible = NO;
    TouchStep(^{ compatible = [self hasViewport:root]; });
    if (!compatible) {
        [self removeRoot:root];
        [self didMountComponentsWithRootTag:root.surfaceId];
        return;
    }
    BOOL down = [type isEqualToString:@"pointerDown"];
    for (UITouch *touch in touches) {
        SessionRecorderPhysicalContact *contact = [_contacts objectForKey:touch];
        @try {
            try {
                if (down) {
                    if (touch.phase != UITouchPhaseBegan || contact || ![self acceptsTouch:touch root:root]) continue;
                    contact = [SessionRecorderPhysicalContact new];
                    contact.pointerId = ++_nextPointerId;
                    contact.root = root;
                    contact.reportable = [self hasViewport:root];
                    contact->epoch = root->epoch;
                    contact.generation = root.generation;
                    contact.beginView = touch.view;
                    contact.path = InputPath(touch.view, root.presented ? root.view : root.viewportView);
                    contact.webHost = [self webHostForTouch:touch root:root];
                    if (contact.webHost) contact.path = InputPath(contact.webHost.view, root.viewportView);
                    {
                        std::lock_guard<std::mutex> lock(_contactMutex);
                        // A second native contact ends the individual-tap opportunity.
                        for (SessionRecorderPhysicalContact *existing in _contacts.objectEnumerator) {
                            existing->recovery.reset();
                        }
                        [_contacts setObject:contact forKey:touch];
                    }
                    root.contacts++;
                }
                if (!contact || contact.root != root) continue;
                // UIKit may clear touch.view after another recognizer recognizes
                // the touch. Keep validating the original, frozen input path.
                UIView *currentView = touch.view;
                UIView *continuityView = currentView ?: contact.beginView;
                if (contact.generation != root.generation || contact->epoch != root->epoch ||
                    (contact.webHost ?
                        (![self ownsWebHost:contact.webHost root:root] ||
                         !SameInputPath(contact.path, contact.webHost.view, root.viewportView)) :
                        (contact.beginView != continuityView ||
                         (!currentView && touch.window != contact->epoch->window) ||
                         NearestTouchRoot(continuityView) != root.view ||
                         !SameInputPath(contact.path, continuityView, root.presented ? root.view : root.viewportView)))) {
                    [self abandonTouch:touch contact:contact];
                    continue;
                }
                if (!contact.reportable) continue;
                CGPoint point = root.presented
                    ? [root.view convertPoint:[touch locationInView:root.view] toView:root.viewportView]
                    : [touch locationInView:root.viewportView];
                double timestamp = _epochAnchorMs + (touch.timestamp - _uptimeAnchor) * 1000.0;
                if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(timestamp)) {
                    [self abandonTouch:touch contact:contact];
                    continue;
                }
                // One changed contact per immutable primitive snapshot; no stationary samples invented.
                NSDictionary *payload = @{
                    @"activationToken": root.token, @"type": type, @"timestamp": @(timestamp),
                    @"actionPointerId": @(contact.pointerId),
                    @"pointers": @[@{@"pointerId": @(contact.pointerId), @"x": @(point.x),
                        @"y": @(point.y), @"isActive": @(!terminal)}],
                    @"viewport": @{@"left": @0, @"top": @0, @"width": @(root.viewportSize.width),
                        @"height": @(root.viewportSize.height)}
                };
                if ([type isEqualToString:@"pointerCancel"]) {
                    NSMutableDictionary *cancelPayload = [payload mutableCopy];
                    cancelPayload[@"cancelScope"] = @"pointer";
                    payload = [cancelPayload copy];
                }
                // UIKit callbacks are serial on main; the sink only enqueues RN delivery, never awaits JS.
                if (down) contact.downForwarded = YES; // Delivery can enqueue and then throw.
                if (terminal) [self forgetTouch:touch]; // Consume before delivery; never replay a terminal.
                if ([self isAuthorized:root.token]) _eventSink(payload);
            } catch (...) {
                TouchStep(^{ [self abandonTouch:touch contact:contact]; });
            }
        } @catch (NSException *exception) {
            TouchStep(^{ [self abandonTouch:touch contact:contact]; });
        }
    }
}
@end
