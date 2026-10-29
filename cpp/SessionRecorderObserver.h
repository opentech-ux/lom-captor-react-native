#pragma once

#include "SessionRecorderTreeScanner.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstring>

#include <react/renderer/uimanager/UIManagerCommitHook.h>
#include <react/renderer/components/root/RootShadowNode.h>

namespace lomtree
{
    using namespace facebook::react;
    using namespace facebook::jsi;

    enum class CaptureResult
    {
        Captured,
        Deduplicated,
        Unavailable
    };

    struct CaptureOutcome
    {
        CaptureResult result;
        uint64_t debtGeneration;
        uint64_t captureGeneration;
        uint64_t pagerVersion = 0;
#if defined(__APPLE__)
        bool visibilityUnavailable = false;
        uint64_t visibilityGeneration = 0;
#endif
    };

    struct ActiveSurface
    {
        SurfaceId surfaceId;
        std::string surfaceInstance;
#if defined(__APPLE__)
        // Used only by capture-result delivery; membership snapshots keep the default.
        uint64_t visibilityGeneration = 0;
#endif
    };

    enum class DirtyCaptureStatus
    {
        Clean,
        Captured,
        Unavailable
    };

    struct DirtyCaptureOutcome
    {
        DirtyCaptureStatus status = DirtyCaptureStatus::Unavailable;
        SurfaceId surfaceId = -1;
        std::string surfaceInstance;
        TreeCapture tree;
        uint64_t signature = 0;
        bool priorityAttempted = false;
        bool dirty = false;
        uint64_t observedGeneration = 0;
        uint64_t captureGeneration = 0;
        uint64_t debtGeneration = 0;
        int64_t latestRevision = -1;
        int64_t materializedRevision = -1;
    };

    struct ScanClockSample
    {
        std::string activation;
        double time = -1;
    };
    struct ScanTiming
    {
        std::string activation;
        double scanStart = -1;
        double scanEnd = -1;
    };
#if defined(__APPLE__)
    struct PointerDownRecovery
    {
        std::string activation;
        uint64_t pointerId = 0;
        SurfaceId surface = -1;
        std::string instance;
        uint64_t captureGeneration = 0;
        bool matches(const PointerDownRecovery &other) const
        {
            return activation == other.activation && pointerId == other.pointerId &&
                surface == other.surface && instance == other.instance &&
                captureGeneration == other.captureGeneration;
        }
    };
    using TouchRecoveryCheck = std::function<bool(const PointerDownRecovery &)>;
    using TouchRecoveryClose = std::function<void(const PointerDownRecovery &)>;
#endif
    using ScanClock = std::function<ScanClockSample()>;

    using SnapshotCallback = std::function<void(SurfaceId, std::vector<Root>, float, float, uint64_t, uint64_t, uint64_t, uint64_t, int64_t, const char *, std::string, uint64_t, ScanTiming
#if defined(ANDROID) || defined(__APPLE__)
        , uint64_t
#endif
    )>;
    using CaptureResultCallback = std::function<void(uint64_t, uint64_t, CaptureResult, uint64_t, std::vector<ActiveSurface>, uint64_t)>;

    enum class DebouncePhase
    {
        Frame,
        IDLE,
        Immediate
    };

    struct ObserverConfig
    {
        DebouncePhase phase = DebouncePhase::IDLE;
        int64_t debounceMs = 150;
        int maxNodes = 10000;
    };

    class SessionRecorderObserver final : public UIManagerCommitHook
    {
    private:
        // Background thread processing view changes without freezing the UI
        void run();
        CaptureOutcome captureSurface(SurfaceId surfaceId, uint64_t captureGeneration, uint64_t explicitRequestId, const char *source, const std::string &instance, int64_t consequenceArmedAt = -1
#if defined(__APPLE__)
                                      , const PointerDownRecovery *recovery = nullptr
#endif
                                      );
        void notifyCaptureResult(uint64_t requestId, uint64_t debtGeneration, CaptureResult result, uint64_t captureGeneration, std::vector<ActiveSurface> surfaces, uint64_t pagerVersion);
        bool visitCurrentSurface(SurfaceId surfaceId, const std::string &instance, const std::function<void()> &action);
        void removeSurfaceStateLocked(SurfaceId surfaceId);
#if defined(__APPLE__)
        // Caller holds uiManagerLock_; no UIKit work or synchronous dispatch.
        bool isIOSVisibilityGenerationCurrentLocked(uint64_t generation) const;
#endif
        void processPendingCommits();
        bool hasInteractionDeadlineLocked() const;
        void observeInteractionRevisionLocked(SurfaceId surfaceId, const RootShadowNode::Shared &root, int64_t revisionNumber);
        void satisfyInteractionConsequenceLocked(SurfaceId surfaceId);

        static uint64_t hashRoots(const std::vector<Root> &roots);
        static uint64_t hashRoot(const Root &root);
        static int64_t now();

        // Dependencies
        std::shared_ptr<SessionRecorderPagerRegistry> pagers_ = std::make_shared<SessionRecorderPagerRegistry>();
        ObserverConfig config_;
        UIManager const *uiManager_ = nullptr;

        struct NativeSnapshot
        {
            uint64_t signature;
            std::vector<Root> roots;
            int64_t revisionNumber;
            float viewportWidth;
            float viewportHeight;
            uint64_t pagerVersion;
#if defined(ANDROID) || defined(__APPLE__)
            uint64_t visibilityGeneration = 0;
#endif
        };

        struct InteractionConsequence
        {
            ShadowNodeFamily::Shared family;
            uint64_t captureGeneration;
            int64_t armedAt;
            int64_t deadline = -1;
            int64_t lastMutationAt = -1;
            bool waitingForSignal = false;
            // Weak provenance only, not a second snapshot cache. A hook runs
            // before publication and can be retried/cancelled by Fabric.
            std::weak_ptr<const RootShadowNode> mutationRoot;
            int64_t requiredRevision = -1;
        };

        struct MutationCapture
        {
            std::string instance;
            uint64_t captureGeneration;
            int64_t revisionNumber;
            uint64_t deliveryId = 0;
        };

        bool mutationCanCoverConsequenceLocked(SurfaceId surfaceId, const InteractionConsequence &opportunity) const;

        struct SurfaceFreshness
        {
            uint64_t observedGeneration = 0;
            int64_t materializedRevision = -1;
            uint64_t materializedCaptureGeneration = 0;
            uint64_t materializedPagerVersion = 0;
#if defined(ANDROID) || defined(__APPLE__)
            uint64_t materializedVisibilityGeneration = 0;
#endif
            std::optional<InteractionConsequence> interactionConsequence;
        };

        struct SurfaceInstance
        {
            ShadowNodeFamily::Shared family;
            std::string token;
        };
        struct CommitSignal
        {
            SurfaceId surfaceId;
            ShadowNodeFamily::Shared family;
            uint64_t count;
            int64_t timestamp;
            bool suppressed;
        };
        // Membership is reconciled outside commit hooks. Families keep instance
        // identity alive; opaque tokens are never reused, including after reset.
        std::unordered_map<SurfaceId, SurfaceInstance> surfaceInstances_;
        uint64_t nextSurfaceInstance_ = 0;
        std::vector<CommitSignal> pendingCommitSignals_;

        // Keep the last processed snapshot per surface. Suppression requires
        // accepted coverage in the current generation, exact vector equality
        // and equal viewport dimensions; the hash is only a prefilter.
        mutable std::unordered_map<SurfaceId, NativeSnapshot> lastSnapshotsBySurface_;
        mutable std::unordered_map<SurfaceId, NativeSnapshot> stagedDirtySnapshotsBySurface_;

        // This causes one thread to wait for the other to finish
        // between the main thread and the debounce's thread
        mutable std::mutex hashCacheLock_;

        // Queue of screens waiting to be processed (Debounce)
        mutable std::vector<SurfaceId> pendingScreensForDebounce_;
        // Publication provenance only; it never changes debounce eligibility.
        std::unordered_map<SurfaceId, const char *> pendingCaptureSources_;
        // Revision provenance only; no additional tree is retained.
        std::unordered_map<SurfaceId, MutationCapture> pendingMutationCaptures_;
        std::unordered_map<SurfaceId, std::vector<MutationCapture>> inFlightMutationCaptures_;
        uint64_t nextMutationDeliveryId_ = 0;
        bool initialCaptureRequested_ = false;
        bool paused_ = false;
        uint64_t captureGeneration_ = 0;
        bool scrollSuppressed_ = false;
        bool commitPendingWhileSuppressed_ = false;
        bool explicitCaptureRequested_ = false;
        bool explicitCaptureInFlight_ = false;
        uint64_t explicitCaptureRequestId_ = 0;
        uint64_t explicitCaptureDebtGeneration_ = 0;
        uint64_t postScrollDebtGeneration_ = 0;
        mutable std::unordered_map<SurfaceId, int64_t> captureDebounceAnchorBySurface_;
        std::unordered_map<SurfaceId, SurfaceFreshness> freshnessBySurface_;

        mutable std::condition_variable pendingSignal_;

        // Shield to prevent our thread from using UIManager if React Native is destroying it
        mutable std::mutex uiManagerLock_;
        bool uiManagerUnregistering_ = false;

        std::thread debounceThread_;

        // Atomic flag to safely stop the background thread
        std::atomic<bool> isShuttingDown_{false};

        // JavaScript callback function where we send the captured view tree
        mutable SnapshotCallback callbackJS_;
        ScanClock scanClock_;
#if defined(__APPLE__)
        // Coalesced work in the existing worker; contact eligibility stays native.
        std::optional<PointerDownRecovery> pendingTouchRecovery_;
        TouchRecoveryCheck touchRecoveryCheck_;
        TouchRecoveryClose touchRecoveryClose_;
#endif
        mutable CaptureResultCallback captureResultCallbackJS_;
        mutable std::mutex debounceQueueLock_;

    public:
#if defined(ANDROID)
        std::shared_ptr<SessionRecorderScreenVisibilityRegistry> androidVisibility =
            std::make_shared<SessionRecorderScreenVisibilityRegistry>();
        bool refreshAndroidAlerts(const std::string &activation, bool present,
            const std::function<SessionRecorderScreenVisibilityRegistry::Snapshot::Alert(Tag)> &probe)
        {
            using Alert = SessionRecorderScreenVisibilityRegistry::Snapshot::Alert;
            if (androidVisibility->snapshot()->activation != activation) return false;
            std::vector<RootShadowNode::Shared> roots;
            {
                std::lock_guard<std::mutex> lock(uiManagerLock_);
                if (uiManager_ && !uiManagerUnregistering_)
                    uiManager_->getShadowTreeRegistry().enumerate([&](const ShadowTree &tree, bool &) {
                        const auto revision = tree.getMountingCoordinator()->getBaseRevision();
                        if (revision.rootShadowNode) roots.push_back(revision.rootShadowNode);
                    });
            }
            std::vector<Alert> alerts;
            // Existing Down routing requires a unique Surface. Never pick one by size/order.
            if (present && roots.size() == 1) {
                auto alert = probe(roots.front()->getTag());
                alert.surface = roots.front()->getSurfaceId();
                alert.rootFamily = roots.front()->getFamilyShared();
                alerts.push_back(std::move(alert));
            }
            const bool accredited = alerts.size() == 1 && alerts.front().dialog.instance != 0;
            const auto generation = androidVisibility->snapshot()->generation;
            androidVisibility->publishAlerts(activation, present, std::move(alerts));
            if (generation != androidVisibility->snapshot()->generation) requestInitialCapture();
            // Do not let an unavailable Down resolution fall back onto the covered Fabric LOM.
            // Keep the native window observer installed; the next real input can re-accredit it.
            std::lock_guard<std::mutex> lock(debounceQueueLock_);
            return accredited && !paused_ && !scrollSuppressed_ && !isShuttingDown_.load() &&
                postScrollDebtGeneration_ == 0 && !pagers_->active();
        }
        void refreshAndroidScreens(const std::string &activation, const std::function<long long(Tag, Tag)> &probe,
            const std::function<SessionRecorderScreenVisibilityRegistry::Observation::BackButton(Tag)> &probeBack,
            const std::function<SessionRecorderScreenVisibilityRegistry::Observation::Title(Tag)> &probeTitle)
        {
            std::vector<std::pair<RootShadowNode::Shared, int64_t>> roots;
            {
                std::lock_guard<std::mutex> lock(uiManagerLock_);
                if (!uiManager_ || uiManagerUnregistering_) return;
                uiManager_->getShadowTreeRegistry().enumerate([&](const ShadowTree &tree, bool &) {
                    auto revision = tree.getMountingCoordinator()->getBaseRevision();
                    if (revision.rootShadowNode) roots.emplace_back(revision.rootShadowNode, revision.number);
                });
            }
            const auto generation = androidVisibility->snapshot()->generation;
            bool coverageAdvanced = false;
            for (const auto &entry : androidVisibility->snapshot()->screens) {
                bool present = false;
                for (const auto &root : roots) if (root.first->getSurfaceId() == entry.second.identity.surface) present = true;
                if (!present) androidVisibility->removeSurface(entry.second.identity.surface);
            }
            for (const auto &[root, revision] : roots) {
                std::vector<SessionRecorderScreenVisibilityRegistry::Observation> observations;
                std::vector<std::shared_ptr<const ShadowNode>> nodes{root};
                while (!nodes.empty()) {
                    auto node = nodes.back(); nodes.pop_back();
                    const auto *name = node->getComponentName();
                    if (name && std::strcmp(name, "RNSScreen") == 0) {
                        auto evidence = probe(node->getTag(), root->getTag());
                        const auto previous = androidVisibility->snapshot();
                        const auto *old = previous->find({root->getSurfaceId(), node->getTag(), root->getFamilyShared(), node->getFamilyShared()});
                        coverageAdvanced = coverageAdvanced || !old || old->mountedRevision != revision;
                        observations.push_back({{root->getSurfaceId(), node->getTag(), root->getFamilyShared(), node->getFamilyShared()},
                            static_cast<uint64_t>(evidence >> 2), revision, (evidence & 3) != 0, (evidence & 3) != 2});
                        if ((evidence & 3) == 1) observations.back().backButton = probeBack(node->getTag());
                        if ((evidence & 3) == 1) observations.back().title = probeTitle(node->getTag());
                    }
                    for (const auto &child : node->getChildren()) nodes.push_back(child);
                }
                androidVisibility->publishSurface(activation, root->getSurfaceId(), std::move(observations));
            }
            if (coverageAdvanced || generation != androidVisibility->snapshot()->generation) requestInitialCapture();
        }
#endif
        explicit SessionRecorderObserver(ObserverConfig config = {});
        // Destructor
        ~SessionRecorderObserver();
        // Native iOS invalidation can stop the worker before JS releases its holders.
        void shutdown();

        std::shared_ptr<SessionRecorderPagerRegistry> pagerRegistry() const { return pagers_; }
        std::vector<SessionRecorderPagerRegistry::Pager> reconcilePagers(const std::string &activation)
        {
            std::lock_guard<std::mutex> lock(uiManagerLock_);
            return uiManager_ && !uiManagerUnregistering_ ? pagers_->reconcile(activation, *uiManager_) :
                std::vector<SessionRecorderPagerRegistry::Pager>{};
        }
#if defined(__APPLE__)
        void setTouchRecoveryCallbacks(TouchRecoveryCheck check, TouchRecoveryClose close)
        {
            std::lock_guard<std::mutex> lock(debounceQueueLock_);
            touchRecoveryCheck_ = std::move(check);
            touchRecoveryClose_ = std::move(close);
        }
        void requestTouchCapture(PointerDownRecovery recovery);
#endif
        void setScanClock(ScanClock clock)
        {
            std::lock_guard<std::mutex> lock(debounceQueueLock_);
            scanClock_ = std::move(clock);
        }
        void setSnapshotCallback(SnapshotCallback sc);
        void clearSnapshotCallback();
        void setCaptureResultCallback(CaptureResultCallback callback);
        void clearCaptureResultCallback();
        void requestInitialCapture();
        bool requestExplicitCapture(uint64_t requestId, uint64_t debtGeneration);
        DirtyCaptureOutcome captureIfDirty(SurfaceId surfaceId, const std::string &instance, uint64_t priorityDebtGeneration = 0);
        std::optional<std::vector<ActiveSurface>> getActiveSurfaces();
        bool isSurfaceInstanceCurrent(SurfaceId surfaceId, const std::string &instance);
        void stageDirtySnapshot(SurfaceId surfaceId, const std::string &instance, uint64_t captureGeneration, uint64_t signature, TreeCapture &&tree);
        bool acceptMaterializedRevision(SurfaceId surfaceId, int64_t revisionNumber, uint64_t captureGeneration, const std::string &instance, uint64_t pagerVersion
#if defined(ANDROID) || defined(__APPLE__)
            , uint64_t visibilityGeneration
#endif
        );
        bool armInteractionConsequence(SurfaceId surfaceId, const std::string &instance, uint64_t captureGeneration);
        uint64_t beginMutationDelivery(SurfaceId surfaceId, const std::string &instance, uint64_t captureGeneration, int64_t revisionNumber);
        void completeMutationDelivery(SurfaceId surfaceId, uint64_t deliveryId);
        uint64_t setPaused(bool paused);
        bool isCaptureGenerationCurrent(uint64_t captureGeneration) const;
#if defined(__APPLE__)
        bool isIOSVisibilityGenerationCurrent(uint64_t generation) const;
#endif
        void setPostScrollDebtGeneration(uint64_t generation);
        void setScrollSuppressed(bool suppressed, bool restorePendingAutomaticCapture);

        void unregisterFromUIManager();
        void resetUIManager();

        bool isRegistered() const
        {
            std::lock_guard<std::mutex> lock(uiManagerLock_);
            return uiManager_ != nullptr && !uiManagerUnregistering_;
        }

        void commitHookWasRegistered(UIManager const &uiManager) noexcept override;
        void commitHookWasUnregistered(UIManager const &uiManager) noexcept override;

        RootShadowNode::Unshared shadowTreeWillCommit(
            ShadowTree const &shadowTree,
            RootShadowNode::Shared const &oldRootShadowNode,
            RootShadowNode::Unshared const &newRootShadowNode) noexcept override;
    };

}
