#import "SessionRecorderScrollObserver.h"
#import "SessionRecorderInputOwnership.h"

#import <UIKit/UIKit.h>
#import <WebKit/WKWebView.h>
#import <React/RCTComponentViewRegistry.h>
#import <React/RCTMountingManager.h>
#import <React/RCTScheduler.h>
#import <React/RCTScrollViewComponentView.h>
#import <React/RCTSurfacePresenter.h>
#import <React/RCTSurfaceView.h>
#import <QuartzCore/QuartzCore.h>

#include <react/renderer/components/scrollview/ScrollViewShadowNode.h>
#include <react/renderer/mounting/ShadowTree.h>
#include <react/renderer/mounting/ShadowTreeRegistry.h>
#include <react/renderer/uimanager/UIManager.h>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

using namespace facebook::react;
using namespace sessionrecorder;

static BOOL ScrollStep(dispatch_block_t step)
{
    @try { try { step(); return YES; } catch (...) {} }
    @catch (NSException *exception) {}
    return NO;
}

static BOOL FiniteRect(CGRect rect)
{
    return std::isfinite(rect.origin.x) && std::isfinite(rect.origin.y) &&
        std::isfinite(rect.size.width) && std::isfinite(rect.size.height) &&
        rect.size.width > 0 && rect.size.height > 0;
}

// Inspect only public UIKit ancestry, stopping at WKWebView's opaque boundary.
// Multiple WKWebViews under one Fabric host cannot be assigned by guesswork.
static WKWebView *UniqueWebView(UIView *host)
{
    if (!host) return nil;
    NSMutableArray<UIView *> *pending = [NSMutableArray arrayWithArray:host.subviews];
    WKWebView *found = nil;
    while (pending.count) {
        UIView *view = pending.lastObject;
        [pending removeLastObject];
        if ([view isKindOfClass:[WKWebView class]]) {
            if (found) return nil;
            found = (WKWebView *)view;
        } else {
            [pending addObjectsFromArray:view.subviews];
        }
    }
    return found;
}

@interface SessionRecorderScrollSession : NSObject
{
@public
    std::shared_ptr<const InputEpoch> epoch;
}
@property (nonatomic) NSUInteger token;
@property (nonatomic) NSUInteger generation;
@property (nonatomic) CGPoint startOffset;
@property (nonatomic) CGSize boundsSize;
@property (nonatomic) UIEdgeInsets contentInset;
@property (nonatomic) UIEdgeInsets adjustedInset;
@property (nonatomic) BOOL rtl;
@property (nonatomic) BOOL validated;
@property (nonatomic) BOOL decelerating;
@property (nonatomic) BOOL panCancelled;
@property (nonatomic, copy) NSDictionary *lastSnapshot;
@end
@implementation SessionRecorderScrollSession
@end

@class SessionRecorderScrollListener;
@interface SessionRecorderScrollRecord : NSObject
{
@public
    ShadowNodeFamily::Shared family;
    ShadowNodeFamily::Shared rootFamily;
    std::shared_ptr<const InputEpoch> epoch;
}
@property (nonatomic, weak) UIView *component;
@property (nonatomic, weak) WKWebView *webView;
@property (nonatomic) BOOL webViewRecord;
@property (nonatomic, weak) UIScrollView *scrollView;
@property (nonatomic, weak) UIView *viewport;
@property (nonatomic, strong) SessionRecorderScrollListener *listener;
@property (nonatomic, strong) SessionRecorderScrollSession *session;
@property (nonatomic, strong) CADisplayLink *displayLink;
@property (nonatomic, copy) NSString *activation;
@property (nonatomic) NSInteger tag;
@property (nonatomic) NSUInteger generation;
@property (nonatomic) NSUInteger viewId;
@property (nonatomic) CGSize viewportSize;
@property (nonatomic) BOOL rtl;
@property (nonatomic) BOOL revoked;
@property (nonatomic, strong) NSPointerArray *path;
@end
@implementation SessionRecorderScrollRecord
@end

struct ScrollCandidate {
    std::shared_ptr<const ShadowNode> node;
    ShadowNodeFamily::Shared rootFamily;
    SurfaceId surfaceId;
};

@interface SessionRecorderScrollObserver () <RCTSurfacePresenterObserver>
- (void)observe:(SessionRecorderScrollRecord *)record stage:(NSString *)stage decelerate:(BOOL)decelerate;
- (void)panCancelled:(SessionRecorderScrollRecord *)record;
- (void)webPanChanged:(SessionRecorderScrollRecord *)record recognizer:(UIPanGestureRecognizer *)recognizer;
- (void)pollWebScroll:(SessionRecorderScrollRecord *)record;
- (BOOL)hasScrollAssociation:(SessionRecorderScrollRecord *)record;
- (BOOL)isAuthorized:(NSString *)token;
- (BOOL)ownsRecord:(SessionRecorderScrollRecord *)record;
- (NSDictionary *)readSnapshot:(SessionRecorderScrollRecord *)record;
- (NSDictionary *)readSnapshot:(SessionRecorderScrollRecord *)record session:(SessionRecorderScrollSession *)session;
- (void)queueReconciliation;
- (void)reconcileSurface:(NSInteger)surfaceId beforeMount:(BOOL)beforeMount;
- (void)removeCandidates:(const std::shared_ptr<const ShadowNode> &)node;
- (void)indexNode:(const std::shared_ptr<const ShadowNode> &)node previous:(const std::shared_ptr<const ShadowNode> &)previous
             root:(const RootShadowNode::Shared &)root;
- (void)emit:(NSString *)name record:(SessionRecorderScrollRecord *)record
     session:(SessionRecorderScrollSession *)session snapshot:(NSDictionary *)snapshot;
- (void)validate:(SessionRecorderScrollSession *)session record:(SessionRecorderScrollRecord *)record
         snapshot:(NSDictionary *)snapshot;
- (void)finish:(SessionRecorderScrollRecord *)record finalSnapshot:(BOOL)finalSnapshot;
- (void)removeRecord:(SessionRecorderScrollRecord *)record finalSnapshot:(BOOL)finalSnapshot;
- (void)abandonSessions;
- (void)removeObservation;
@end

// The RN splitter holds this weakly; the record retains it until removal.
@interface SessionRecorderScrollListener : NSObject <UIScrollViewDelegate>
@property (nonatomic, weak) SessionRecorderScrollObserver *owner;
@property (nonatomic, weak) SessionRecorderScrollRecord *record;
@end
@implementation SessionRecorderScrollListener
- (void)scrollViewWillBeginDragging:(UIScrollView *)scrollView
{ [self.owner observe:self.record stage:@"begin" decelerate:NO]; }
- (void)scrollViewDidScroll:(UIScrollView *)scrollView
{ [self.owner observe:self.record stage:@"move" decelerate:NO]; }
- (void)scrollViewDidEndDragging:(UIScrollView *)scrollView willDecelerate:(BOOL)decelerate
{ [self.owner observe:self.record stage:@"end-drag" decelerate:decelerate]; }
- (void)scrollViewDidEndDecelerating:(UIScrollView *)scrollView
{ [self.owner observe:self.record stage:@"end-deceleration" decelerate:NO]; }
- (void)panChanged:(UIPanGestureRecognizer *)recognizer
{
    if (self.record.webViewRecord) {
        [self.owner webPanChanged:self.record recognizer:recognizer];
        return;
    }
    if (recognizer.state == UIGestureRecognizerStateCancelled) [self.owner panCancelled:self.record];
}
- (void)displayLinkFired:(CADisplayLink *)displayLink
{ [self.owner pollWebScroll:self.record]; }
@end

@implementation SessionRecorderScrollObserver
{
    __weak RCTSurfacePresenter *_presenter;
    std::weak_ptr<UIManager> _inputManager;
    SessionRecorderScrollEventSink _eventSink;
    NSString *_authorizedToken; // Only under @synchronized(self).
    BOOL _invalidated;

    // Main-thread state. IDs are not reset across activations of this owner.
    NSString *_installedToken;
    BOOL _observingMounts;
    BOOL _reconcileQueued;
    NSMutableArray<id> *_ownershipNotifications;
    NSUInteger _nextViewId;
    NSUInteger _nextSessionToken;
    double _epochAnchorMs;
    double _monotonicAnchor;
    NSMutableDictionary<NSNumber *, SessionRecorderScrollRecord *> *_records;
    std::unordered_map<SurfaceId, RootShadowNode::Shared> _roots;
    std::unordered_map<Tag, ScrollCandidate> _candidates;
    std::unordered_set<Tag> _indexedTags; // Scratch for moves between changed parents.
}

- (instancetype)initWithSurfacePresenter:(RCTSurfacePresenter *)presenter
                               eventSink:(SessionRecorderScrollEventSink)eventSink
{
    if ((self = [super init])) {
        _presenter = presenter;
        RCTScheduler *scheduler = presenter.scheduler;
        _inputManager = scheduler ? scheduler.uiManager : nullptr;
        _eventSink = [eventSink copy];
        _records = [NSMutableDictionary new];
    }
    return self;
}

- (BOOL)isAuthorized:(NSString *)token
{
    @synchronized (self) {
        return !_invalidated && token != nil && [_authorizedToken isEqualToString:token];
    }
}

- (void)startScrollObservation:(NSString *)activationToken
{
    ScrollStep(^{
        if (activationToken.length == 0) return;
        NSString *token = [activationToken copy];
        @synchronized (self) {
            if (self->_invalidated || [self->_authorizedToken isEqualToString:token]) return;
            self->_authorizedToken = token;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            ScrollStep(^{
                if (![self isAuthorized:token]) return;
                [self removeObservation];
                self->_installedToken = token;
                self->_epochAnchorMs = NSDate.date.timeIntervalSince1970 * 1000.0;
                self->_monotonicAnchor = CACurrentMediaTime();
                self->_observingMounts = YES;
                [self->_presenter addObserver:self];
                self->_ownershipNotifications = [NSMutableArray new];
                __weak SessionRecorderScrollObserver *weakSelf = self;
                for (NSNotificationName name in InputOwnershipNotifications()) {
                    id observer = [NSNotificationCenter.defaultCenter addObserverForName:name object:nil
                        queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
                            SessionRecorderScrollObserver *owner = weakSelf;
                            if (!owner) return;
                            ScrollStep(^{
                                if (![owner isAuthorized:owner->_installedToken]) return;
                                for (SessionRecorderScrollRecord *record in owner->_records.allValues) {
                                    if (([note.name isEqualToString:UISceneWillDeactivateNotification] ||
                                         [note.name isEqualToString:UISceneDidDisconnectNotification]) &&
                                        note.object == record->epoch->scene) [owner removeRecord:record finalSnapshot:YES];
                                    else if ([note.name isEqualToString:UIWindowDidBecomeHiddenNotification] &&
                                             note.object == record->epoch->window) [owner removeRecord:record finalSnapshot:YES];
                                }
                                [owner queueReconciliation];
                            });
                        }];
                    [self->_ownershipNotifications addObject:observer];
                }
                [self reconcileSurface:0 beforeMount:NO];
            });
        });
    });
}

- (void)stopScrollObservation:(NSString *)activationToken
{
    ScrollStep(^{
        @synchronized (self) {
            if (![self->_authorizedToken isEqualToString:activationToken]) return;
            self->_authorizedToken = nil;
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            ScrollStep(^{
                @synchronized (self) { if (self->_authorizedToken != nil) return; }
                [self removeObservation];
            });
        });
    });
}

- (void)invalidate
{
    @synchronized (self) {
        if (_invalidated) return;
        _invalidated = YES;
        _authorizedToken = nil;
    }
    dispatch_async(dispatch_get_main_queue(), ^{ ScrollStep(^{ [self removeObservation]; }); });
}

// Skip shared branches: scroll state updates must not cause a full-tree DFS.
- (void)removeCandidates:(const std::shared_ptr<const ShadowNode> &)node
{
    if (!node) return;
    if (_indexedTags.count(node->getTag()) == 0) _candidates.erase(node->getTag());
    for (const auto &child : node->getChildren()) [self removeCandidates:child];
}

- (void)indexNode:(const std::shared_ptr<const ShadowNode> &)node previous:(const std::shared_ptr<const ShadowNode> &)previous
             root:(const RootShadowNode::Shared &)root
{
    if (node == previous) return;
    if (!node) { [self removeCandidates:previous]; return; }
    if (previous && previous->getFamilyShared() != node->getFamilyShared()) {
        [self removeCandidates:previous];
        [self indexNode:node previous:nullptr root:root];
        return;
    }
    const char *name = node->getComponentName();
    if (node->getComponentHandle() == ScrollViewShadowNode::Handle() ||
        (name && std::strcmp(name, "RNCWebView") == 0)) {
        _indexedTags.insert(node->getTag());
        _candidates.insert_or_assign(node->getTag(), ScrollCandidate{node, root->getFamilyShared(), root->getSurfaceId()});
    } else {
        _candidates.erase(node->getTag());
    }
    std::unordered_map<Tag, std::shared_ptr<const ShadowNode>> oldChildren;
    if (previous) for (const auto &child : previous->getChildren()) oldChildren.emplace(child->getTag(), child);
    for (const auto &child : node->getChildren()) {
        auto old = oldChildren.find(child->getTag());
        [self indexNode:child previous:(old == oldChildren.end() ? nullptr : old->second) root:root];
        if (old != oldChildren.end()) oldChildren.erase(old);
    }
    for (const auto &entry : oldChildren) [self removeCandidates:entry.second];
}

- (void)willMountComponentsWithRootTag:(NSInteger)rootTag
{
    // The mounting coordinator has pulled the new base revision, but UIKit is
    // still the old tree: retire deleted/replaced records before recycle resets.
    if (!ScrollStep(^{ [self reconcileSurface:rootTag beforeMount:YES]; })) [self abandonSessions];
}
- (void)didMountComponentsWithRootTag:(NSInteger)rootTag
{
    if (!ScrollStep(^{ [self reconcileSurface:rootTag beforeMount:NO]; })) [self abandonSessions];
    [self queueReconciliation]; // One mount-triggered turn for queued UIKit container updates.
}

- (void)queueReconciliation
{
    ScrollStep(^{
        if (![self isAuthorized:self->_installedToken] || self->_reconcileQueued) return;
        self->_reconcileQueued = YES;
        NSString *activation = self->_installedToken;
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!ScrollStep(^{
                if (![self->_installedToken isEqualToString:activation] || ![self isAuthorized:activation]) return;
                self->_reconcileQueued = NO;
                [self reconcileSurface:0 beforeMount:NO];
            })) [self abandonSessions];
        });
    });
}

- (void)reconcileSurface:(NSInteger)surfaceId beforeMount:(BOOL)beforeMount
{
    if (![self isAuthorized:_installedToken]) return;
    _indexedTags.clear();
    RCTSurfacePresenter *presenter = _presenter;
    RCTScheduler *scheduler = presenter.scheduler;
    auto manager = scheduler ? scheduler.uiManager : nullptr;
    if (!manager || manager != _inputManager.lock()) {
        for (SessionRecorderScrollRecord *record in _records.allValues) [self removeRecord:record finalSnapshot:NO];
        _roots.clear();
        _candidates.clear();
        return;
    }
    std::unordered_map<SurfaceId, RootShadowNode::Shared> current;
    if (manager) manager->getShadowTreeRegistry().enumerate([&](const ShadowTree &tree, bool &) {
        auto root = tree.getMountingCoordinator()->getBaseRevision().rootShadowNode;
        if (root && (surfaceId == 0 || root->getSurfaceId() == surfaceId)) current.emplace(root->getSurfaceId(), root);
    }); // Release the Fabric registry lock before UIKit work.
    for (auto it = _roots.begin(); it != _roots.end();) {
        if ((surfaceId == 0 || it->first == surfaceId) && current.count(it->first) == 0) {
            [self removeCandidates:it->second];
            it = _roots.erase(it);
        } else ++it;
    }
    for (const auto &entry : current) {
        auto old = _roots.find(entry.first);
        [self indexNode:entry.second previous:(old == _roots.end() ? nullptr : old->second) root:entry.second];
        _roots.insert_or_assign(entry.first, entry.second);
    }
    RCTComponentViewRegistry *registry = presenter.mountingManager.componentViewRegistry;
    for (SessionRecorderScrollRecord *record in _records.allValues) {
        auto candidate = _candidates.find((Tag)record.tag);
        if (candidate == _candidates.end() || candidate->second.node->getFamilyShared() != record->family ||
            candidate->second.rootFamily != record->rootFamily) {
            [self removeRecord:record finalSnapshot:YES];
        }
    }
    if (beforeMount) return;
    auto baseline = ReadInputEpoch(presenter, _inputManager);
    if (!baseline) {
        for (SessionRecorderScrollRecord *record in _records.allValues) [self removeRecord:record finalSnapshot:NO];
        return; // LOM remains independent of UIKit input eligibility.
    }
    for (SessionRecorderScrollRecord *record in _records.allValues) {
        if (!SameInputEpoch(record->epoch, baseline) ||
            !SameInputPath(record.path, record.scrollView, record.viewport)) [self removeRecord:record finalSnapshot:NO];
    }
    for (const auto &entry : _candidates) {
        const auto &candidate = entry.second;
        if (surfaceId != 0 && candidate.surfaceId != surfaceId) continue;
        UIView *component = [registry findComponentViewWithTag:entry.first];
        BOOL standard = candidate.node->getComponentHandle() == ScrollViewShadowNode::Handle() &&
            [component isKindOfClass:[RCTScrollViewComponentView class]];
        const char *name = candidate.node->getComponentName();
        BOOL webViewRecord = name && std::strcmp(name, "RNCWebView") == 0;
        WKWebView *webView = webViewRecord ? UniqueWebView(component) : nil;
        UIScrollView *scroll = standard ? ((RCTScrollViewComponentView *)component).scrollView : webView.scrollView;
        auto root = _roots.find(candidate.surfaceId);
        UIView *viewport = root == _roots.end() ? nil : [registry findComponentViewWithTag:root->second->getTag()];
        SessionRecorderScrollRecord *record = _records[@(entry.first)];
        if (record && (record.component != component || record.scrollView != scroll || record.webView != webView ||
            record.viewport != viewport)) {
            [self removeRecord:record finalSnapshot:NO];
            record = nil;
        }
        NSPointerArray *path = InputPath(scroll, viewport);
        if ((!standard && !webView) || !scroll || component.tag != entry.first ||
            !viewport || candidate.surfaceId != baseline->surfaceId ||
            candidate.rootFamily != baseline->family || viewport != baseline->viewport || !path) {
            if (record) [self removeRecord:record finalSnapshot:NO];
            continue; // No invented Modal/native presentation coordinate space.
        }
        if (!record) {
            record = [SessionRecorderScrollRecord new];
            record->family = candidate.node->getFamilyShared();
            record->rootFamily = candidate.rootFamily;
            record->epoch = baseline;
            record.component = component;
            record.webViewRecord = webViewRecord;
            record.webView = webView;
            record.scrollView = scroll;
            record.viewport = viewport;
            record.path = path;
            record.tag = entry.first;
            record.viewId = ++_nextViewId;
            record.generation = record.viewId;
            record.activation = _installedToken;
            record.listener = [SessionRecorderScrollListener new];
            record.listener.owner = self;
            record.listener.record = record;
            _records[@(entry.first)] = record; // Own partial installs before invoking RN.
            BOOL installed = ScrollStep(^{
                if (standard) [(RCTScrollViewComponentView *)component addScrollListener:record.listener];
                [scroll.panGestureRecognizer addTarget:record.listener action:@selector(panChanged:)];
            });
            if (!installed) { [self removeRecord:record finalSnapshot:NO]; continue; }
        }
        auto size = root->second->getLayoutMetrics().frame.size;
        record.viewportSize = CGSizeMake(size.width, size.height);
        // WebKit offsets remain in its native local scroll coordinate system.
        record.rtl = standard && static_cast<const ScrollViewShadowNode *>(candidate.node.get())->getLayoutMetrics().layoutDirection ==
            LayoutDirection::RightToLeft;
        if (record.session && ![self readSnapshot:record]) {
            [self removeRecord:record finalSnapshot:NO];
            [self queueReconciliation];
        } else if (!record.session) [self readSnapshot:record];
    }
}

- (BOOL)hasScrollAssociation:(SessionRecorderScrollRecord *)record
{
    if (!record || !record.component || !record.scrollView) return NO;
    if (!record.webViewRecord)
        return ((RCTScrollViewComponentView *)record.component).scrollView == record.scrollView;
    auto candidate = _candidates.find((Tag)record.tag);
    if (candidate == _candidates.end() || candidate->second.node->getFamilyShared() != record->family ||
        candidate->second.rootFamily != record->rootFamily) return NO;
    return record.webView != nil && UniqueWebView(record.component) == record.webView &&
        record.webView.scrollView == record.scrollView;
}

- (BOOL)ownsRecord:(SessionRecorderScrollRecord *)record
{
    if (!record || record.revoked || ![self isAuthorized:record.activation] || ![_installedToken isEqualToString:record.activation] ||
        _records[@(record.tag)] != record || record.component.tag != record.tag ||
        ![self hasScrollAssociation:record] ||
        !SameInputEpoch(record->epoch, ReadInputEpoch(_presenter, _inputManager)) ||
        !SameInputPath(record.path, record.scrollView, record.viewport)) return NO;
    return [_presenter.mountingManager.componentViewRegistry findComponentViewWithTag:(Tag)record.tag] == record.component;
}

- (NSDictionary *)readSnapshot:(SessionRecorderScrollRecord *)record
{
    return [self readSnapshot:record session:record.session];
}

- (NSDictionary *)readSnapshot:(SessionRecorderScrollRecord *)record session:(SessionRecorderScrollSession *)session
{
    if (!record || ![self hasScrollAssociation:record] ||
        [_presenter.mountingManager.componentViewRegistry findComponentViewWithTag:(Tag)record.tag] != record.component ||
        !SameInputEpoch(record->epoch, ReadInputEpoch(_presenter, _inputManager)) ||
        !SameInputPath(record.path, record.scrollView, record.viewport)) return nil;
    UIScrollView *scroll = record.scrollView;
    UIView *viewport = record.viewport;
    if (!scroll || !viewport || !scroll.window || scroll.window != viewport.window ||
        ![viewport.superview isKindOfClass:[RCTSurfaceView class]] || ![scroll isDescendantOfView:viewport] ||
        !CGPointEqualToPoint(viewport.bounds.origin, CGPointZero) ||
        !CGSizeEqualToSize(viewport.bounds.size, record.viewportSize) ||
        !CGAffineTransformIsIdentity(viewport.transform) || !CATransform3DIsIdentity(viewport.layer.transform)) return nil;
    CGPoint raw = scroll.contentOffset;
    CGSize content = scroll.contentSize, bounds = scroll.bounds.size;
    UIEdgeInsets inset = scroll.adjustedContentInset;
    UIEdgeInsets contentInset = scroll.contentInset;
    if (session && (session->epoch != record->epoch || session.generation != record.generation ||
        !CGSizeEqualToSize(session.boundsSize, bounds) ||
        !UIEdgeInsetsEqualToEdgeInsets(session.adjustedInset, inset) ||
        !UIEdgeInsetsEqualToEdgeInsets(session.contentInset, contentInset) || session.rtl != record.rtl)) return nil;
    if (!std::isfinite(raw.x) || !std::isfinite(raw.y) || !std::isfinite(content.width) ||
        !std::isfinite(content.height) || !std::isfinite(bounds.width) || !std::isfinite(bounds.height) ||
        !std::isfinite(inset.left) || !std::isfinite(inset.right) ||
        !std::isfinite(inset.top) || !std::isfinite(inset.bottom) ||
        !std::isfinite(contentInset.left) || !std::isfinite(contentInset.right) ||
        !std::isfinite(contentInset.top) || !std::isfinite(contentInset.bottom) ||
        content.width < 0 || content.height < 0 || bounds.width <= 0 || bounds.height <= 0) return nil;
    CGFloat minX = -inset.left, minY = -inset.top;
    CGFloat maxX = fmax(minX, content.width - bounds.width + inset.right);
    CGFloat maxY = fmax(minY, content.height - bounds.height + inset.bottom);
    CGPoint offset = CGPointMake(fmin(maxX, fmax(minX, raw.x)), fmin(maxY, fmax(minY, raw.y)));
    if (record.rtl) offset.x = content.width - bounds.width - offset.x;
    CGRect rect = [scroll convertRect:scroll.bounds toView:viewport];
    if (record.webViewRecord)
        rect = CGRectIntersection(rect, [record.component convertRect:record.component.bounds toView:viewport]);
    for (UIView *ancestor = scroll; ancestor; ancestor = ancestor.superview) {
        if (ancestor.hidden || ancestor.alpha <= 0) return nil;
        if (ancestor.clipsToBounds) rect = CGRectIntersection(rect, [ancestor convertRect:ancestor.bounds toView:viewport]);
        if (ancestor == viewport) break;
    }
    rect = CGRectIntersection(rect, CGRectMake(0, 0, record.viewportSize.width, record.viewportSize.height));
    if (!FiniteRect(rect) || !std::isfinite(offset.x) || !std::isfinite(offset.y)) return nil;
    return @{@"left": @(rect.origin.x), @"top": @(rect.origin.y),
        @"width": @(rect.size.width), @"height": @(rect.size.height),
        @"scrollX": @(offset.x), @"scrollY": @(offset.y)};
}

- (void)emit:(NSString *)name record:(SessionRecorderScrollRecord *)record
     session:(SessionRecorderScrollSession *)session snapshot:(NSDictionary *)snapshot
{
    if (!snapshot || ![self isAuthorized:record.activation]) return;
    NSMutableDictionary *event = [snapshot mutableCopy];
    event[@"activationToken"] = record.activation;
    event[@"viewId"] = @(record.viewId);
    event[@"token"] = @(session.token);
    event[@"timestamp"] = @(_epochAnchorMs + (CACurrentMediaTime() - _monotonicAnchor) * 1000.0);
    if (_eventSink) _eventSink(name, event);
}

- (void)validate:(SessionRecorderScrollSession *)session record:(SessionRecorderScrollRecord *)record
         snapshot:(NSDictionary *)snapshot
{
    session.lastSnapshot = snapshot;
    CGFloat dx = [snapshot[@"scrollX"] doubleValue] - session.startOffset.x;
    CGFloat dy = [snapshot[@"scrollY"] doubleValue] - session.startOffset.y;
    if (!session.validated && dx * dx + dy * dy > 1.0) {
        session.validated = YES;
        [self emit:@"SessionRecorderScrollValidated" record:record session:session snapshot:snapshot];
    }
}

- (void)finish:(SessionRecorderScrollRecord *)record finalSnapshot:(BOOL)finalSnapshot
{
    [record.displayLink invalidate];
    record.displayLink = nil;
    SessionRecorderScrollSession *session = record.session;
    if (!session) return;
    record.session = nil; // Consume before any delivery; never replay after an error.
    __block NSDictionary *snapshot = nil;
    if (finalSnapshot) ScrollStep(^{ snapshot = [self readSnapshot:record session:session]; });
    if (snapshot) {
        if (!ScrollStep(^{ [self validate:session record:record snapshot:snapshot]; })) snapshot = nil;
    }
    BOOL delivered = ScrollStep(^{
        [self emit:snapshot && session.validated ? @"SessionRecorderScrollEnd" : @"SessionRecorderScrollCancel"
            record:record session:session snapshot:snapshot ?: session.lastSnapshot];
    });
    if (!delivered) ScrollStep(^{
        [self emit:@"SessionRecorderScrollCancel" record:record session:session snapshot:session.lastSnapshot];
    });
}

- (void)observe:(SessionRecorderScrollRecord *)record stage:(NSString *)stage decelerate:(BOOL)decelerate
{
    if (!record || record.revoked || ![self isAuthorized:record.activation]) return;
    BOOL completed = ScrollStep(^{
        NSDictionary *sessionSnapshot = nil;
        if (![self ownsRecord:record] ||
            (record.session && !(sessionSnapshot = [self readSnapshot:record]))) {
            [self removeRecord:record finalSnapshot:NO];
            [self queueReconciliation];
            return;
        }
        if ([stage isEqualToString:@"begin"]) {
            [self finish:record finalSnapshot:YES];
            NSDictionary *snapshot = [self readSnapshot:record];
            if (!snapshot) return;
            SessionRecorderScrollSession *session = [SessionRecorderScrollSession new];
            session.token = ++self->_nextSessionToken;
            session->epoch = record->epoch;
            session.generation = record.generation;
            session.boundsSize = record.scrollView.bounds.size;
            session.contentInset = record.scrollView.contentInset;
            session.adjustedInset = record.scrollView.adjustedContentInset;
            session.rtl = record.rtl;
            session.startOffset = CGPointMake([snapshot[@"scrollX"] doubleValue], [snapshot[@"scrollY"] doubleValue]);
            session.lastSnapshot = snapshot;
            record.session = session;
            if (record.webViewRecord) {
                record.displayLink = [CADisplayLink displayLinkWithTarget:record.listener selector:@selector(displayLinkFired:)];
                [record.displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
            }
            [self emit:@"SessionRecorderScrollStart" record:record session:session snapshot:snapshot];
        } else if ([stage isEqualToString:@"move"]) {
            NSDictionary *snapshot = record.session ? sessionSnapshot : [self readSnapshot:record];
            if (!snapshot) { [self removeRecord:record finalSnapshot:NO]; [self queueReconciliation]; return; }
            if (record.session) [self validate:record.session record:record snapshot:snapshot];
            // Programmatic/layout snapshots only validate; no inferred user session.
        } else if ([stage isEqualToString:@"end-drag"]) {
            if (!record.session || record.session.decelerating) return;
            if (decelerate) record.session.decelerating = YES;
            else [self finish:record finalSnapshot:YES];
        } else if ([stage isEqualToString:@"end-deceleration"]) {
            if (record.session.decelerating && !record.scrollView.dragging && !record.scrollView.decelerating)
                [self finish:record finalSnapshot:YES];
        }
    });
    if (!completed) ScrollStep(^{ [self removeRecord:record finalSnapshot:NO]; [self queueReconciliation]; });
}

- (void)webPanChanged:(SessionRecorderScrollRecord *)record recognizer:(UIPanGestureRecognizer *)recognizer
{
    if (!record || record.revoked || ![self isAuthorized:record.activation]) return;
    if (!ScrollStep(^{
        if (recognizer.state == UIGestureRecognizerStateBegan) {
            [self observe:record stage:@"begin" decelerate:NO];
        } else if (record.session) {
            [self observe:record stage:@"move" decelerate:NO];
            if (!record.session) return;
            if (recognizer.state == UIGestureRecognizerStateCancelled ||
                recognizer.state == UIGestureRecognizerStateFailed)
                record.session.panCancelled = YES;
            if (recognizer.state == UIGestureRecognizerStateEnded ||
                recognizer.state == UIGestureRecognizerStateCancelled ||
                recognizer.state == UIGestureRecognizerStateFailed) {
                NSUInteger token = record.session.token, generation = record.generation;
                // Let UIScrollView settle its dragging/decelerating flags first.
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (record.generation == generation && record.session.token == token)
                        [self pollWebScroll:record];
                });
            }
        }
    })) {
        ScrollStep(^{ [self removeRecord:record finalSnapshot:NO]; [self queueReconciliation]; });
    }
}

- (void)pollWebScroll:(SessionRecorderScrollRecord *)record
{
    if (!record || record.revoked || !record.session) return;
    if (!ScrollStep(^{
        if (![self isAuthorized:record.activation]) { [self removeRecord:record finalSnapshot:NO]; return; }
        [self observe:record stage:@"move" decelerate:NO];
        if (!record.session || record.revoked) return;
        UIScrollView *scroll = record.scrollView;
        UIGestureRecognizerState state = scroll.panGestureRecognizer.state;
        if (scroll.dragging || scroll.tracking || state == UIGestureRecognizerStateBegan ||
            state == UIGestureRecognizerStateChanged) return;
        if (scroll.decelerating) { record.session.decelerating = YES; return; }
        // Remember cancellation even after UIKit resets the pan to Possible.
        // As on the standard path, actual deceleration may still finish normally.
        [self finish:record finalSnapshot:!record.session.panCancelled || record.session.decelerating];
    })) {
        ScrollStep(^{ [self removeRecord:record finalSnapshot:NO]; [self queueReconciliation]; });
    }
}

- (void)panCancelled:(SessionRecorderScrollRecord *)record
{
    ScrollStep(^{
        if (!record || record.revoked || ![self isAuthorized:record.activation]) return;
        if (![self ownsRecord:record]) { [self removeRecord:record finalSnapshot:NO]; [self queueReconciliation]; return; }
        if (!record.session) return;
        NSUInteger token = record.session.token, generation = record.generation;
        // A single main turn reconciles callback ordering, not an idle timeout.
        dispatch_async(dispatch_get_main_queue(), ^{
            ScrollStep(^{
                if (record.revoked || record.generation != generation || record.session.token != token) return;
                if (![self ownsRecord:record] || ![self readSnapshot:record]) {
                    [self removeRecord:record finalSnapshot:NO]; [self queueReconciliation]; return;
                }
                UIScrollView *scroll = record.scrollView;
                if (scroll.decelerating) { record.session.decelerating = YES; return; }
                if (scroll.dragging || scroll.tracking) return;
                // Cancel releases ownership; it does NOT invent a wire End.
                [self finish:record finalSnapshot:NO];
            });
        });
    });
}

- (void)removeRecord:(SessionRecorderScrollRecord *)record finalSnapshot:(BOOL)finalSnapshot
{
    if (!record || record.revoked) return;
    record.revoked = YES; // Retire this registration before invoking delivery or RN cleanup.
    if (_records[@(record.tag)] == record) [_records removeObjectForKey:@(record.tag)];
    ScrollStep(^{ [self finish:record finalSnapshot:finalSnapshot]; });
    if (!record.webViewRecord)
        ScrollStep(^{ [(RCTScrollViewComponentView *)record.component removeScrollListener:record.listener]; });
    ScrollStep(^{ [record.scrollView.panGestureRecognizer removeTarget:record.listener action:@selector(panChanged:)]; });
    record.listener.owner = nil;
    record.listener.record = nil;
}

- (void)abandonSessions
{
    ScrollStep(^{
        for (SessionRecorderScrollRecord *record in self->_records.allValues)
            ScrollStep(^{ [self finish:record finalSnapshot:NO]; });
    });
}

- (void)removeObservation
{
    for (id observer in _ownershipNotifications) ScrollStep(^{
        [NSNotificationCenter.defaultCenter removeObserver:observer];
    });
    _ownershipNotifications = nil;
    if (_observingMounts) {
        ScrollStep(^{ [self->_presenter removeObserver:self]; });
        _observingMounts = NO;
    }
    for (SessionRecorderScrollRecord *record in _records.allValues) [self removeRecord:record finalSnapshot:NO];
    [_records removeAllObjects];
    _roots.clear();
    _candidates.clear();
    _indexedTags.clear();
    _installedToken = nil;
    _reconcileQueued = NO;
}

- (void)snapshotScrollSessions:(NSArray *)requests completion:(void (^)(NSArray *))completion
{
    // The JS drain reserves tokens before clearing its maps. Native records stay
    // alive until stop's main cleanup; requests queued before it can still read.
    NSArray *batch = [requests copy];
    dispatch_async(dispatch_get_main_queue(), ^{
        NSMutableArray *results = [NSMutableArray new];
        ScrollStep(^{
            @synchronized (self) { if (self->_invalidated) return; }
            for (id value in batch) {
                ScrollStep(^{
                    if (![value isKindOfClass:[NSDictionary class]]) return;
                    NSNumber *viewId = value[@"viewId"], *token = value[@"token"];
                    NSString *activation = value[@"activationToken"];
                    if (![viewId isKindOfClass:[NSNumber class]] || ![token isKindOfClass:[NSNumber class]] ||
                        ![activation isKindOfClass:[NSString class]] ||
                        ![self->_installedToken isEqualToString:activation]) return;
                    for (SessionRecorderScrollRecord *record in self->_records.allValues) {
                        if (!record.session || ![viewId isEqualToNumber:@(record.viewId)] ||
                            record.generation != record.viewId || ![token isEqualToNumber:@(record.session.token)] ||
                            ![record.activation isEqualToString:activation] ||
                            record.component.tag != record.tag || ![self hasScrollAssociation:record] ||
                            [self->_presenter.mountingManager.componentViewRegistry findComponentViewWithTag:(Tag)record.tag]
                                != record.component) continue;
                        NSDictionary *snapshot = [self readSnapshot:record];
                        if (!snapshot) continue;
                        [results addObject:@{@"viewId": viewId, @"token": token,
                            @"offsetX": snapshot[@"scrollX"], @"offsetY": snapshot[@"scrollY"],
                            @"viewport": @{@"left": snapshot[@"left"], @"top": snapshot[@"top"],
                                @"width": snapshot[@"width"], @"height": snapshot[@"height"]}}];
                        break;
                    }
                });
            }
        });
        ScrollStep(^{ completion(results); });
    });
}
@end
