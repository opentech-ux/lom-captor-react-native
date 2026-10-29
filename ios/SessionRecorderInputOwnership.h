#pragma once

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <React/RCTComponentViewRegistry.h>
#import <React/RCTMountingManager.h>
#import <React/RCTScheduler.h>
#import <React/RCTSurfacePresenter.h>
#import <React/RCTSurfaceView.h>

#include <react/renderer/mounting/ShadowTree.h>
#include <react/renderer/mounting/ShadowTreeRegistry.h>
#include <react/renderer/uimanager/UIManager.h>
#include <cmath>
#include <initializer_list>
#include <memory>

namespace sessionrecorder {

// Main-thread snapshots only. Root family is the native Surface-instance
// identity, not a replacement for the JS surfaceInstance or captureGeneration.
struct InputEpoch {
    facebook::react::ShadowNodeFamily::Shared family;
    facebook::react::SurfaceId surfaceId;
    facebook::react::Tag rootTag;
    __weak UIView *viewport;
    __weak UIView *hostingView;
    __weak UIWindow *window;
    __weak UIWindowScene *scene;
    CGRect viewportBounds;
    CGRect windowBounds;
    CGPoint originInWindow;
    CGPoint xInWindow;
    CGPoint yInWindow;
    UIInterfaceOrientation orientation;
};

inline std::shared_ptr<const InputEpoch> ReadInputEpoch(
    RCTSurfacePresenter *presenter, const std::weak_ptr<facebook::react::UIManager> &managerOwner,
    facebook::react::RootShadowNode::Shared *mountedRoot = nullptr)
{
    if (!NSThread.isMainThread || !presenter) return nullptr;
    auto scheduler = presenter.scheduler;
    auto manager = scheduler ? scheduler.uiManager : nullptr;
    if (!manager || manager != managerOwner.lock()) return nullptr; // Never register Runtime B through owner A.
    facebook::react::RootShadowNode::Shared root;
    NSUInteger surfaces = 0;
    manager->getShadowTreeRegistry().enumerate([&](const facebook::react::ShadowTree &tree, bool &) {
        surfaces++;
        root = tree.getMountingCoordinator()->getBaseRevision().rootShadowNode;
    }); // Release the registry lock before UIKit work; never choose latest/first.
    if (surfaces != 1 || !root) return nullptr;
    UIView *viewport = [presenter.mountingManager.componentViewRegistry findComponentViewWithTag:root->getTag()];
    UIWindow *window = viewport.window;
    UIWindowScene *scene = window.windowScene;
    auto size = root->getLayoutMetrics().frame.size;
    if (!viewport || !window || !scene || window.hidden || window.alpha <= 0 ||
        scene.activationState != UISceneActivationStateForegroundActive || scene.interfaceOrientation == UIInterfaceOrientationUnknown ||
        ![viewport.superview isKindOfClass:[RCTSurfaceView class]] ||
        !std::isfinite(size.width) || !std::isfinite(size.height) || size.width <= 0 || size.height <= 0 ||
        !CGPointEqualToPoint(viewport.bounds.origin, CGPointZero) ||
        !CGSizeEqualToSize(viewport.bounds.size, CGSizeMake(size.width, size.height)) ||
        !CGAffineTransformIsIdentity(viewport.transform) || !CATransform3DIsIdentity(viewport.layer.transform))
        return nullptr;
    NSUInteger activeScenes = 0;
    for (UIScene *candidate in UIApplication.sharedApplication.connectedScenes) {
        if ([candidate isKindOfClass:[UIWindowScene class]] &&
            candidate.activationState == UISceneActivationStateForegroundActive) activeScenes++;
    }
    if (activeScenes != 1) return nullptr;
    for (UIView *ancestor = viewport; ancestor; ancestor = ancestor.superview) {
        if (ancestor.hidden || ancestor.alpha <= 0 || ancestor.layer.animationKeys.count != 0) return nullptr;
        if (ancestor == window) break;
    }
    auto epoch = std::make_shared<InputEpoch>();
    epoch->family = root->getFamilyShared();
    epoch->surfaceId = root->getSurfaceId();
    epoch->rootTag = root->getTag();
    epoch->viewport = viewport;
    epoch->hostingView = viewport.superview;
    epoch->window = window;
    epoch->scene = scene;
    epoch->viewportBounds = viewport.bounds;
    epoch->windowBounds = window.bounds;
    epoch->originInWindow = [viewport convertPoint:CGPointZero toView:window];
    epoch->xInWindow = [viewport convertPoint:CGPointMake(1, 0) toView:window];
    epoch->yInWindow = [viewport convertPoint:CGPointMake(0, 1) toView:window];
    epoch->orientation = scene.interfaceOrientation;
    for (CGPoint point : {epoch->originInWindow, epoch->xInWindow, epoch->yInWindow}) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return nullptr;
    }
    if (mountedRoot) *mountedRoot = root;
    return epoch;
}

inline bool SameInputEpoch(const std::shared_ptr<const InputEpoch> &a,
                           const std::shared_ptr<const InputEpoch> &b)
{
    return a && b && a->family == b->family && a->surfaceId == b->surfaceId && a->rootTag == b->rootTag &&
        a->viewport != nil && a->viewport == b->viewport && a->hostingView == b->hostingView &&
        a->window != nil && a->window == b->window && a->scene != nil && a->scene == b->scene &&
        CGRectEqualToRect(a->viewportBounds, b->viewportBounds) && CGRectEqualToRect(a->windowBounds, b->windowBounds) &&
        CGPointEqualToPoint(a->originInWindow, b->originInWindow) &&
        CGPointEqualToPoint(a->xInWindow, b->xInWindow) && CGPointEqualToPoint(a->yInWindow, b->yInWindow) &&
        a->orientation == b->orientation;
}

// A UIWindow endpoint is already the owning window; ordinary views use .window.
static inline UIView *InputPathEndpointWindow(UIView *endpoint)
{
    return [endpoint isKindOfClass:[UIWindow class]] ? endpoint : endpoint.window;
}

// Remember actual ancestry, not tags. Reparenting cannot migrate an interaction.
enum class InputPathFailure { None, Animation, Other };

// Pending accreditation only needs validity; callers that freeze ancestry also
// request the weak snapshot. Both paths execute the same guards once.
static inline InputPathFailure CheckInputPath(UIView *view, UIView *viewport,
                                              NSPointerArray *__autoreleasing *snapshot = nullptr)
{
    if (!view || !viewport || !view.window || view.window != InputPathEndpointWindow(viewport))
        return InputPathFailure::Other;
    NSPointerArray *path = snapshot ? [NSPointerArray weakObjectsPointerArray] : nil;
    for (UIView *ancestor = view; ancestor; ancestor = ancestor.superview) {
        if (ancestor.hidden || ancestor.alpha <= 0) return InputPathFailure::Other;
        if (ancestor.layer.animationKeys.count != 0) return InputPathFailure::Animation;
        if (snapshot) [path addPointer:(__bridge void *)ancestor];
        if (ancestor == viewport) {
            if (snapshot) *snapshot = path;
            return InputPathFailure::None;
        }
    }
    return InputPathFailure::Other; // The endpoint must belong to the actual superview chain.
}

inline NSPointerArray *InputPath(UIView *view, UIView *viewport, InputPathFailure *failure = nullptr)
{
    if (failure) *failure = InputPathFailure::Other;
    NSPointerArray *path = nil;
    auto result = CheckInputPath(view, viewport, &path);
    if (failure) *failure = result;
    return path;
}

inline bool SameInputPath(NSPointerArray *path, UIView *view, UIView *viewport)
{
    if (!path || !view || !viewport || !view.window || view.window != InputPathEndpointWindow(viewport)) return false;
    NSUInteger index = 0;
    for (UIView *ancestor = view; ancestor; ancestor = ancestor.superview) {
        if (index >= path.count || [path pointerAtIndex:index++] != (__bridge void *)ancestor ||
            ancestor.hidden || ancestor.alpha <= 0 || ancestor.layer.animationKeys.count != 0) return false;
        if (ancestor == viewport) return index == path.count;
    }
    return false;
}

inline NSArray<NSNotificationName> *InputOwnershipNotifications()
{
    return @[UISceneDidActivateNotification, UISceneWillDeactivateNotification, UISceneDidDisconnectNotification,
        UIWindowDidBecomeVisibleNotification, UIWindowDidBecomeHiddenNotification];
}

} // namespace sessionrecorder
