#include "SessionRecorderTreeScanner.h"

#include <react/renderer/components/root/RootShadowNode.h>
#include <react/renderer/core/ComponentDescriptor.h>
#include <react/renderer/core/PropsParserContext.h>
#include <react/renderer/components/view/ViewProps.h>
#include <unordered_map>
#include <react/debug/react_native_assert.h>
#if defined(__APPLE__)
#include "SessionRecorderScreenVisibilityRegistry.h"
#include <react/renderer/components/rnscreens/Props.h>
#endif
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace lomtree
{
    namespace
    {
        // Read-only ancestor adapter: no Yoga cloning, layout or commit. Fabric
        // queries only its metrics, transform, content origin and original children.
        class ScreenContentClipNode final : public LayoutableShadowNode
        {
        public:
            explicit ScreenContentClipNode(const LayoutableShadowNode &source)
                : LayoutableShadowNode(source, ShadowNodeFragment{.props = source.getProps(), .state = source.getState()}),
                  source_(source) {}
facebook::react::Transform getTransform() const override
{
    return source_.getTransform();
}            facebook::react::Point getContentOriginOffset(bool includeTransform) const override
{
    return source_.getContentOriginOffset(includeTransform);
}
            void layoutTree(LayoutContext, LayoutConstraints) override { react_native_assert(false); }
            void layout(LayoutContext) override { react_native_assert(false); }
            void dirtyLayout() override { react_native_assert(false); }
            bool getIsLayoutClean() const override { return true; }
        private:
            const LayoutableShadowNode &source_;
        };

        using ScreenClipCache = std::unordered_map<const ShadowNode *, std::unique_ptr<ScreenContentClipNode>>;

        LayoutMetrics screenContentRelativeMetrics(const ShadowNode &node,
                                                  const LayoutableShadowNode &surfaceRoot,
                                                  ScreenClipCache &screenCopies)
        {
            auto ancestors = node.getFamily().getAncestors(surfaceRoot);
            for (auto &ancestor : ancestors)
            {
                const auto &parent = ancestor.first.get();
                const char *name = parent.getComponentName();
                if (!name || std::strcmp(name, "RNSScreen") != 0) continue;
                const auto *screen = dynamic_cast<const LayoutableShadowNode *>(&parent);
                if (!screen) continue;
                const auto &branch = parent.getChildren().at(ancestor.second);
                const char *branchName = branch->getComponentName();
                // The header lives outside the screen's shifted content box.
                if (branchName && std::strcmp(branchName, "RNSScreenStackHeaderConfig") == 0) continue;
                const auto offset = screen->getContentOriginOffset(true);
                if (offset.x == 0 && offset.y == 0) continue;

                auto cached = screenCopies.find(&parent);
                if (cached == screenCopies.end())
                {
                    auto copy = std::make_unique<ScreenContentClipNode>(*screen);
                    auto metrics = screen->getLayoutMetrics();
                    // Fabric adds contentOriginOffset before intersecting with its
                    // overflow rect. Translate that rect, preserving its size and
                    // existing overflow insets; do not translate the frame twice.
                    metrics.overflowInset.left += offset.x;
                    metrics.overflowInset.right -= offset.x;
                    metrics.overflowInset.top += offset.y;
                    metrics.overflowInset.bottom -= offset.y;
                    copy->setLayoutMetrics(metrics);
                    cached = screenCopies.emplace(&parent, std::move(copy)).first;
                }
                ancestor.first = std::cref(static_cast<const ShadowNode &>(*cached->second));
            }
            return LayoutableShadowNode::computeRelativeLayoutMetrics(
                ancestors,
                {.includeTransform = true, .includeViewportOffset = false, .enableOverflowClipping = true});
        }
    }

    struct SessionRecorderTreeScanner::CaptureGeometryContext
    {
        SessionRecorderPagerRegistry::Snapshot pagers;
        ShadowNodeFamily::Shared rootFamily;
#if defined(ANDROID)
        std::shared_ptr<const SessionRecorderScreenVisibilityRegistry::Snapshot> androidVisibility;
        int64_t androidRevision = -1;
        bool androidVisibilityInvalid = false;
#endif
#if defined(__APPLE__)
        std::shared_ptr<const SessionRecorderScreenVisibilityRegistry::Snapshot> visibility;
        std::vector<SessionRecorderScreenVisibilityRegistry::Identity> missingScreens;
        int64_t revision = -1;
        bool visibilityInvalid = false;
#endif
        // Original-node identity and adapters are valid only for this snapshot.
        ScreenClipCache screenCopies;
#if defined(ANDROID)
        // Private flat-tree IDs only; never persisted as Fabric tags or wire IDs.
        Tag nextNativeTag = -2;
#endif
    };

    TreeCapture SessionRecorderTreeScanner::captureTree(
        SurfaceId surfaceId,
        UIManager const *uiManager,
        size_t maxNodes,
        [[maybe_unused]] bool allowVisibilityRetry) const
    {
        if (!uiManager)
        {
            return {};
        }

#if defined(__APPLE__)
        std::shared_ptr<SessionRecorderScreenVisibilityRegistry> visibilityRegistry;
        std::shared_ptr<const SessionRecorderScreenVisibilityRegistry::Snapshot> visibility;
        std::vector<SessionRecorderScreenVisibilityRegistry::Identity> missingScreens;
        try {
            visibilityRegistry = screenVisibilityRegistry(*uiManager);
            if (visibilityRegistry) visibility = visibilityRegistry->snapshot();
        } catch (...) { visibility.reset(); }
#endif
        const auto pagerSnapshot = pagers_ ? pagers_->snapshot() : SessionRecorderPagerRegistry::Snapshot{};
        TreeCapture result;
        result.pagerVersion = pagerSnapshot.version;
#if defined(__APPLE__)
        result.visibilityGeneration = visibility ? visibility->generation : 0;
#endif
        result.roots.reserve(64);

        std::unordered_set<Tag> tags;
        bool stopTraversal = false;

        uiManager->getShadowTreeRegistry().visit(surfaceId, [&](const ShadowTree &tree)
                                                 {
            auto revision = tree.getCurrentRevision();
            result.revisionNumber = revision.number;

            if (!revision.rootShadowNode)
            {
                return;
            }

            const auto &rootShadow = *revision.rootShadowNode;
            result.surfaceFamily = rootShadow.getFamilyShared();
            const auto &rootMetrics = rootShadow.getLayoutMetrics();
            const bool rootMetricsEmpty = rootMetrics == EmptyLayoutMetrics;
            const bool rootDisplayNone = rootMetrics.displayType == DisplayType::None;
            const bool rootHidden = rootShadow.getTraits().check(ShadowNodeTraits::Trait::Hidden);
            const bool rootNonPositiveSize = rootMetrics.frame.size.width <= 0 || rootMetrics.frame.size.height <= 0;
            if (rootMetricsEmpty || rootDisplayNone || rootHidden || rootNonPositiveSize)
            {
                return;
            }

            const facebook::react::Rect surfaceViewport{
                .origin = {.x = 0, .y = 0},
                .size = rootMetrics.frame.size,
            };
            result.viewportWidth = surfaceViewport.size.width;
            result.viewportHeight = surfaceViewport.size.height;

            const auto *rootViewProps = getViewProps(rootShadow);
            const bool rootOpacityZero =
                rootViewProps != nullptr && rootViewProps->opacity == 0;

            // Query geometry in application viewport coordinates without moving
            // the root's overflow clip into the external viewport-offset space.
            const auto &sourceProps = rootShadow.getConcreteProps();
            const auto &descriptor = rootShadow.getFamily().getComponentDescriptor();
            const PropsParserContext propsContext{
                surfaceId, *descriptor.getContextContainer()};
            // A non-null source makes cloneProps allocate fresh, parsed props.
            auto geometryProps = std::const_pointer_cast<RootProps>(
                std::static_pointer_cast<const RootProps>(descriptor.cloneProps(
                    propsContext, rootShadow.getProps(), RawProps{})));
            geometryProps->layoutConstraints = sourceProps.layoutConstraints;
            geometryProps->layoutContext = sourceProps.layoutContext;
            geometryProps->layoutContext.viewportOffset = {.x = 0, .y = 0};
            const RootShadowNode geometryRoot(
                rootShadow,
                ShadowNodeFragment{
                    .props = geometryProps,
                    .state = rootShadow.getState(),
                });

            CaptureGeometryContext geometryContext;
            geometryContext.pagers = pagerSnapshot;
            geometryContext.rootFamily = rootShadow.getFamilyShared();
#if defined(ANDROID)
            geometryContext.androidVisibility = androidVisibility ? androidVisibility->snapshot() : nullptr;
            geometryContext.androidRevision = revision.number;
            result.visibilityGeneration = geometryContext.androidVisibility ? geometryContext.androidVisibility->generation : 0;
#endif
#if defined(__APPLE__)
            geometryContext.visibility = visibility;
            geometryContext.revision = revision.number;
#endif
            for (const auto &child : rootShadow.getChildren())
            {
                if (stopTraversal) break;
                if (child) {
                    recursiveNode(
                        *child,
                        geometryRoot,
                        surfaceViewport,
                        geometryContext,
                        -1,
                        0,
                        rootShadow.getTraits().check(
                            ShadowNodeTraits::Trait::ChildrenFormStackingContext),
                        rootOpacityZero,
                        maxNodes,
                        stopTraversal,
                        result.roots,
                        tags);
                }
            }
#if defined(ANDROID)
            const auto &native = geometryContext.androidVisibility;
            if (!stopTraversal && native && native->alertPresent) {
                // Append supported native dialog evidence after the normal Fabric traversal.
                const auto found = native->alerts.find(surfaceId);
                if (found != native->alerts.end()) {
                    const auto &alert = found->second;
                    if (alert.rootFamily == rootShadow.getFamilyShared() && alert.dialog.instance != 0 &&
                        alert.viewportWidth == surfaceViewport.size.width &&
                        alert.viewportHeight == surfaceViewport.size.height) {
                        auto append = [&](const auto &element, Tag tag, Tag parent, const char *name, int depth) {
                            if (!element.instance || stopTraversal) return false;
                            const auto rect = facebook::react::Rect::intersect(
                                {{element.x, element.y}, {element.width, element.height}}, surfaceViewport);
                            if (std::trunc(rect.size.width) <= 0 || std::trunc(rect.size.height) <= 0) return false;
                            result.roots.push_back(Root{tag, parent, name, depth,
                                rect.origin.x, rect.origin.y, rect.size.width, rect.size.height});
                            if (result.roots.size() > maxNodes) stopTraversal = true;
                            return true;
                        };
                        // Parent IDs must be non-negative: serialization maps negative
                        // parentTag values to null. Reserve an unused capture-local ID;
                        // native leaves keep the existing Back/title tag allocator.
                        Tag dialogTag = std::numeric_limits<Tag>::max();
                        while (tags.find(dialogTag) != tags.end()) --dialogTag;
                        tags.insert(dialogTag);
                        if (append(alert.dialog, dialogTag, -1,
                            alert.datePicker ? "DatePickerDialog" : alert.timePicker ? "TimePickerDialog" : "AlertDialog", 0)) {
                            if (alert.datePicker && !stopTraversal)
                                append(alert.picker, geometryContext.nextNativeTag--, dialogTag, "DatePicker", 1);
                            const char *names[] = {"AlertPositiveButton", "AlertNegativeButton", "AlertNeutralButton"};
                            const char *dateNames[] = {"DatePickerPositiveButton", "DatePickerNegativeButton", "DatePickerNeutralButton"};
                            const char *timeNames[] = {"TimePickerPositiveButton", "TimePickerNegativeButton", "TimePickerNeutralButton"};
                            for (size_t i = 0; i < alert.buttons.size() && !stopTraversal; ++i)
                                append(alert.buttons[i], geometryContext.nextNativeTag--, dialogTag,
                                    alert.datePicker ? dateNames[i] : alert.timePicker ? timeNames[i] : names[i], 1);
                        }
                    }
                }
            }
            if (geometryContext.androidVisibilityInvalid || (geometryContext.androidVisibility &&
                !androidVisibility->current(*geometryContext.androidVisibility))) {
                result.roots.clear(); result.revisionNumber = -1;
            }
#endif
#if defined(__APPLE__)
            if (geometryContext.visibilityInvalid) {
                result.visibilityUnavailable = true;
                result.roots.clear();
                result.revisionNumber = -1;
            }
            missingScreens = std::move(geometryContext.missingScreens);
#endif
        });

#if defined(__APPLE__)
        if (visibilityRegistry && !missingScreens.empty()) {
            try { visibilityRegistry->request(std::move(missingScreens)); } catch (...) {}
        }
        if (visibility && !visibilityRegistry->current(*visibility)) {
            // Native RNSScreen visibility evidence changed during this capture. Never
            // publish a partial result or replay without native visibility.
            result.visibilityUnavailable = true;
            result.roots.clear();
            result.revisionNumber = -1;
        }
        if (allowVisibilityRetry && result.visibilityUnavailable && visibilityRegistry && visibility) {
            std::shared_ptr<const SessionRecorderScreenVisibilityRegistry::Snapshot> published;
            try { published = visibilityRegistry->snapshot(); } catch (...) {}
            // A reserved atomic generation is not proof of a published snapshot.
            // Retry only within this activation, and never retry the second scan.
            if (published && published != visibility &&
                !published->activation.empty() && published->activation == visibility->activation &&
                visibilityRegistry->current(*published)) {
                if (pagers_ && pagers_->active()) return result;
                auto retried = captureTree(surfaceId, uiManager, maxNodes, false);
                return retried;
            }
        }
#endif
        return result;
    }

    void SessionRecorderTreeScanner::recursiveNode(
        const ShadowNode &node,
        const LayoutableShadowNode &surfaceRoot,
        const facebook::react::Rect &surfaceViewport,
        CaptureGeometryContext &geometryContext,
        Tag emittedParentTag,
        int emittedDepth,
        bool parentChildrenFormStackingContext,
        bool ancestorOpacityZero,
        size_t maxNodes,
        bool &stopTraversal,
        std::vector<Root> &roots,
        std::unordered_set<Tag> &tags) const
    {
        if (stopTraversal)
            return;

        Tag currentTag = node.getTag();

        if (tags.find(currentTag) != tags.end())
        {
            return;
        }

        tags.insert(currentTag);

        if (node.getTraits().check(ShadowNodeTraits::Trait::Hidden))
        {
            return;
        }

        const char *nameChar = node.getComponentName();
        if (nameChar != nullptr && std::strcmp(nameChar, "DebuggingOverlay") == 0)
        {
            // React Native development infrastructure is not product UI.
            return;
        }
#if defined(__APPLE__)
        if (nameChar != nullptr && std::strcmp(nameChar, "RNSScreen") == 0)
        {
            const SessionRecorderScreenVisibilityRegistry::Identity identity{
                node.getSurfaceId(), node.getTag(), geometryContext.rootFamily, node.getFamilyShared()};
            const auto &visibility = geometryContext.visibility;
            const auto *observation = visibility ? visibility->find(identity) : nullptr;
            if (!observation) geometryContext.missingScreens.push_back(identity);
            if (!observation || !observation->known || observation->mountedRevision != geometryContext.revision) {
                // Keep discovering candidates in this traversal, but discard
                // the entire capture if any encountered screen is unaccredited.
                geometryContext.visibilityInvalid = true;
            }
            const auto *screenProps =
                dynamic_cast<const facebook::react::RNSScreenProps *>(node.getProps().get());
            if (screenProps != nullptr && screenProps->activityState == 0)
            {
                return;
            }
            if (visibility && visibility->detached(observation, geometryContext.revision)) {
                return;
            }
        }
#elif defined(RN_SERIALIZABLE_STATE)
        if (nameChar != nullptr && std::strcmp(nameChar, "RNSScreen") == 0)
        {
            try
            {
                const auto &props = node.getProps();
                if (props && props->rawProps.isObject())
                {
                    const auto &rawProps = props->rawProps;
                    const auto activityState = rawProps.find("activityState");
                    if (activityState != rawProps.items().end() &&
                        activityState->second.isNumber() &&
                        activityState->second.asDouble() == 0.0)
                    {
                        return;
                    }
                }
            }
            catch (...)
            {
                // An unreadable state cannot establish that the screen is inactive.
            }
        }
#endif

        const auto *viewProps = getViewProps(node);
#if defined(ANDROID)
        if (nameChar && std::strcmp(nameChar, "RNSScreen") == 0) {
            const auto &snapshot = geometryContext.androidVisibility;
            const auto *observation = snapshot ? snapshot->find({node.getSurfaceId(), node.getTag(),
                geometryContext.rootFamily, node.getFamilyShared()}) : nullptr;
            if (!observation || !observation->known || observation->mountedRevision != geometryContext.androidRevision)
                geometryContext.androidVisibilityInvalid = true;
            else if (snapshot->detached(observation, geometryContext.androidRevision)) return;
        }
#endif
        const bool ownOpacityZero =
            viewProps != nullptr && viewProps->opacity == 0;
        if (ancestorOpacityZero || ownOpacityZero)
        {
            return;
        }

        const bool clipsDescendants =
            viewProps != nullptr && viewProps->getClipsContentToBounds();

        auto layoutNode = dynamic_cast<const LayoutableShadowNode *>(&node);

        if (!layoutNode)
        {
            for (const auto &child : node.getChildren())
            {
                if (stopTraversal)
                    break;
                if (child && geometryContext.pagers.allows(node, *child, geometryContext.rootFamily))
                    recursiveNode(
                        *child, surfaceRoot, surfaceViewport, geometryContext,
                        emittedParentTag, emittedDepth,
                        node.getTraits().check(
                            ShadowNodeTraits::Trait::ChildrenFormStackingContext),
                        false, maxNodes, stopTraversal, roots, tags);
            }
            return;
        }

        const LayoutMetrics &localMetrics = layoutNode->getLayoutMetrics();
        if (localMetrics.displayType == DisplayType::None)
        {
            return;
        }

        if (localMetrics == EmptyLayoutMetrics)
        {
            if (clipsDescendants)
            {
                return;
            }

            for (const auto &child : node.getChildren())
            {
                if (stopTraversal)
                    break;
                if (child && geometryContext.pagers.allows(node, *child, geometryContext.rootFamily))
                    recursiveNode(
                        *child, surfaceRoot, surfaceViewport, geometryContext,
                        emittedParentTag, emittedDepth,
                        node.getTraits().check(
                            ShadowNodeTraits::Trait::ChildrenFormStackingContext),
                        false, maxNodes, stopTraversal, roots, tags);
            }
            return;
        }

        const auto relativeMetrics = screenContentRelativeMetrics(node, surfaceRoot, geometryContext.screenCopies);


        if (relativeMetrics == EmptyLayoutMetrics)
        {
            if (clipsDescendants)
            {
                return;
            }

            for (const auto &child : node.getChildren())
            {
                if (stopTraversal)
                    break;
                if (child && geometryContext.pagers.allows(node, *child, geometryContext.rootFamily))
                    recursiveNode(
                        *child, surfaceRoot, surfaceViewport, geometryContext,
                        emittedParentTag, emittedDepth,
                        node.getTraits().check(
                            ShadowNodeTraits::Trait::ChildrenFormStackingContext),
                        false, maxNodes, stopTraversal, roots, tags);
            }
            return;
        }

        const facebook::react::Rect visibleRect =
            facebook::react::Rect::intersect(relativeMetrics.frame, surfaceViewport);
        const bool hasVisibleArea =
            visibleRect.size.width > 0 && visibleRect.size.height > 0;

        if (!hasVisibleArea)
        {
            if (clipsDescendants)
            {
                return;
            }

            for (const auto &child : node.getChildren())
            {
                if (stopTraversal)
                    break;
                if (child && geometryContext.pagers.allows(node, *child, geometryContext.rootFamily))
                    recursiveNode(
                        *child, surfaceRoot, surfaceViewport, geometryContext,
                        emittedParentTag, emittedDepth,
                        node.getTraits().check(
                            ShadowNodeTraits::Trait::ChildrenFormStackingContext),
                        false, maxNodes, stopTraversal, roots, tags);
            }
            return;
        }

        bool flattenNode =
            nameChar != nullptr &&
            (std::strcmp(nameChar, "RNSScreenContentWrapper") == 0 ||
             std::strcmp(nameChar, "RNGestureHandlerRootView") == 0 ||
             std::strcmp(nameChar, "RNCSafeAreaProvider") == 0 ||
             std::strcmp(nameChar, "RNSScreenStack") == 0 ||
             std::strcmp(nameChar, "RNSScreenContainer") == 0 ||
             std::strcmp(nameChar, "AndroidHorizontalScrollContentView") == 0);
        const bool isView = nameChar != nullptr && std::strcmp(nameChar, "View") == 0;
        ShadowNodeTraits viewTraits;
        bool hostView = false;
        if (!flattenNode && isView)
        {
            viewTraits = node.getTraits();
            if (viewTraits.check(ShadowNodeTraits::Trait::ViewKind))
            {
                hostView =
                    (viewTraits.check(ShadowNodeTraits::Trait::FormsView) ||
                     parentChildrenFormStackingContext) &&
                    !viewTraits.check(ShadowNodeTraits::Trait::ForceFlattenView);
                flattenNode = !hostView;
            }
        }
        const bool isDebuggingOverlayWrapper =
            isView && viewProps != nullptr &&
            viewProps->pointerEvents == PointerEventsMode::None &&
            node.getChildren().size() == 1 && node.getChildren().front() &&
            node.getChildren().front()->getComponentName() != nullptr &&
            std::strcmp(node.getChildren().front()->getComponentName(), "DebuggingOverlay") == 0;
        if (!flattenNode && isView && viewProps != nullptr &&
            (viewProps->pointerEvents == PointerEventsMode::BoxNone ||
             isDebuggingOverlayWrapper))
        {
            const auto *props = dynamic_cast<const ViewProps *>(node.getProps().get());
            if (props != nullptr)
            {
                bool hasBorder = false;
                for (const auto edge : facebook::yoga::ordinals<facebook::yoga::Edge>())
                {
                    if (props->yogaStyle.border(edge).isDefined())
                    {
                        hasBorder = true;
                        break;
                    }
                }

                bool hasPlatformSemantics = false;
#if defined(__ANDROID__)
                hasPlatformSemantics =
                    props->focusable || props->hasTVPreferredFocus ||
                    props->screenReaderFocusable || props->nextFocusDown.has_value() ||
                    props->nextFocusForward.has_value() || props->nextFocusLeft.has_value() ||
                    props->nextFocusRight.has_value() || props->nextFocusUp.has_value() ||
                    props->nativeBackground.has_value() || props->nativeForeground.has_value() ||
                    props->elevation != 0;
#endif

                const bool hasOwnSemantics = hasPlatformSemantics ||
                    props->events.bits.any() || props->hitSlop != EdgeInsets{} ||
                    props->accessible || props->accessibilityState.has_value() ||
                    !props->accessibilityLabel.empty() || !props->accessibilityRole.empty() ||
                    props->role != Role::None || !props->accessibilityHint.empty() ||
                    !props->accessibilityLanguage.empty() ||
                    !props->accessibilityLargeContentTitle.empty() ||
                    props->accessibilityValue != AccessibilityValue{} ||
                    !props->accessibilityActions.empty() ||
                    !props->accessibilityOrder.empty() ||
                    !props->accessibilityLabelledBy.value.empty() ||
                    props->accessibilityTraits != AccessibilityTraits::None ||
                    props->accessibilityLiveRegion != AccessibilityLiveRegion::None ||
                    props->accessibilityShowsLargeContentViewer ||
                    props->accessibilityViewIsModal || props->accessibilityElementsHidden ||
                    props->accessibilityIgnoresInvertColors ||
                    !props->accessibilityRespondsToUserInteraction ||
                    props->importantForAccessibility != ImportantForAccessibility::Auto ||
                    props->onAccessibilityTap || props->onAccessibilityMagicTap ||
                    props->onAccessibilityEscape || props->onAccessibilityAction ||
                    !props->testId.empty() || !props->nativeId.empty() ||
                    isColorMeaningful(props->backgroundColor) || hasBorder ||
                    isColorMeaningful(props->shadowColor) || !props->boxShadow.empty() ||
                    !props->backgroundImage.empty() || props->outlineWidth > 0 ||
                    !props->filter.empty() || props->mixBlendMode != BlendMode::Normal ||
                    props->isolation == Isolation::Isolate ||
                    props->opacity != 1 || props->transform != Transform{} ||
                    props->cursor != Cursor::Auto;

                // Keep Fabric layout/clipping in the traversal, but omit a box
                // that cannot itself receive pointers and has no own semantics.
                flattenNode = !hasOwnSemantics &&
                    (!isDebuggingOverlayWrapper ||
                     (!clipsDescendants && !props->removeClippedSubviews &&
                      !props->zIndex.has_value()));
            }
        }
        flattenNode = flattenNode ||
            std::trunc(visibleRect.size.width) == 0 ||
            std::trunc(visibleRect.size.height) == 0;
        if (!flattenNode)
        {
            std::string name = nameChar ? nameChar : "Unknown";
            roots.push_back(Root{
                currentTag,
                emittedParentTag,
                std::move(name),
                emittedDepth,
                static_cast<float>(visibleRect.origin.x),
                static_cast<float>(visibleRect.origin.y),
                static_cast<float>(visibleRect.size.width),
                static_cast<float>(visibleRect.size.height),
            });

            // Keep the first maxNodes nodes and one explicit overflow sentinel node.
            // The resulting partial tree is valid and is hashed/delivered normally.
            if (roots.size() > maxNodes)
            {
                stopTraversal = true;
                return;
            }
        }

        for (const auto &child : node.getChildren())
        {
            if (stopTraversal)
                break;
            if (child && geometryContext.pagers.allows(node, *child, geometryContext.rootFamily))
                recursiveNode(
                    *child, surfaceRoot, surfaceViewport, geometryContext,
                    flattenNode ? emittedParentTag : currentTag,
                    flattenNode ? emittedDepth : emittedDepth + 1,
                    node.getTraits().check(
                        ShadowNodeTraits::Trait::ChildrenFormStackingContext),
                    false, maxNodes, stopTraversal, roots, tags);
        }
#if defined(ANDROID)
        if (!stopTraversal && !flattenNode && nameChar && std::strcmp(nameChar, "RNSScreen") == 0) {
            const auto &snapshot = geometryContext.androidVisibility;
            const auto *observation = snapshot ? snapshot->find({node.getSurfaceId(), currentTag,
                geometryContext.rootFamily, node.getFamilyShared()}) : nullptr;
            if (observation && observation->known && observation->inWindow &&
                observation->mountedRevision == geometryContext.androidRevision && observation->backButton.instance != 0) {
                const auto &button = observation->backButton;
                const auto rect = facebook::react::Rect::intersect(
                    {{button.x, button.y}, {button.width, button.height}}, surfaceViewport);
                if (std::trunc(rect.size.width) > 0 && std::trunc(rect.size.height) > 0) {
                    // This is a real native ImageButton, outside HeaderConfig's
                    // Fabric content clip. Its own UI snapshot already includes
                    // native ancestor clipping in application-viewport space.
                    const Tag tag = geometryContext.nextNativeTag--;
                    roots.push_back(Root{tag, currentTag, "ImageButton", emittedDepth + 1,
                        rect.origin.x, rect.origin.y, rect.size.width, rect.size.height});
                    if (roots.size() > maxNodes) stopTraversal = true;
                }
            }
            if (!stopTraversal && observation && observation->known && observation->inWindow &&
                observation->mountedRevision == geometryContext.androidRevision && observation->title.instance != 0) {
                const auto &title = observation->title;
                const auto rect = facebook::react::Rect::intersect(
                    {{title.x, title.y}, {title.width, title.height}}, surfaceViewport);
                if (std::trunc(rect.size.width) > 0 && std::trunc(rect.size.height) > 0) {
                    // Only the untagged standard native title, not a Fabric TextView.
                    // Like Back, its native box bypasses the Fabric HeaderConfig clip.
                    const Tag tag = geometryContext.nextNativeTag--;
                    roots.push_back(Root{tag, currentTag, "TextView", emittedDepth + 1,
                        rect.origin.x, rect.origin.y, rect.size.width, rect.size.height});
                    if (roots.size() > maxNodes) stopTraversal = true;
                }
            }
        }
#endif
    }

    const BaseViewProps *SessionRecorderTreeScanner::getViewProps(
        const ShadowNode &node)
    {
        return dynamic_cast<const BaseViewProps *>(node.getProps().get());
    }

}
