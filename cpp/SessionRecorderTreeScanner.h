#pragma once

#include "SessionRecorderPagerRegistry.h"
#include "SessionRecorderScreenVisibilityRegistry.h"
#include <react/renderer/uimanager/UIManagerBinding.h>
#include <react/renderer/uimanager/UIManager.h>
#include <react/renderer/core/ShadowNode.h>
#include <react/renderer/core/LayoutMetrics.h>
#include <react/renderer/components/view/BaseViewProps.h>
#include <react/renderer/mounting/ShadowTreeDelegate.h>
#include <react/renderer/mounting/MountingCoordinator.h>
#include <react/renderer/core/LayoutableShadowNode.h>
#include <jsi/jsi.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <unordered_set>

namespace lomtree
{
    using namespace facebook::react;
    using namespace facebook::jsi;

    struct Root
    {
        Tag tag;
        Tag parentTag;
        std::string name;
        int depth;
        float x;
        float y;
        float width;
        float height;
    };

    struct TreeCapture
    {
        std::vector<Root> roots;
        ShadowNodeFamily::Shared surfaceFamily;
        float viewportWidth = 0;
        float viewportHeight = 0;
        int64_t revisionNumber = -1;
        uint64_t pagerVersion = 0;
#if defined(ANDROID) || defined(__APPLE__)
        // Exact native visibility evidence used by this capture attempt; never wire metadata.
        uint64_t visibilityGeneration = 0;
#endif
#if defined(__APPLE__)
        bool visibilityUnavailable = false;
#endif
    };

    class SessionRecorderTreeScanner final
    {
    public:
        explicit SessionRecorderTreeScanner(std::shared_ptr<SessionRecorderPagerRegistry> pagers = nullptr)
            : pagers_(std::move(pagers)) {}

        // Only captureIfDirty opts into the single local Apple visibility retry.
        TreeCapture captureTree(
            SurfaceId surfaceId,
            UIManager const *uiManager,
            size_t maxNodes,
            bool allowVisibilityRetry = false) const;

    private:
#if defined(ANDROID)
    public:
        std::shared_ptr<SessionRecorderScreenVisibilityRegistry> androidVisibility;
    private:
#endif
        std::shared_ptr<SessionRecorderPagerRegistry> pagers_;
        struct CaptureGeometryContext;

        void recursiveNode(const ShadowNode &node,
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
                           std::unordered_set<Tag> &tags) const;

        static const BaseViewProps *getViewProps(const ShadowNode &node);
    };

}
