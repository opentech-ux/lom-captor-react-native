#include "SessionRecorderObserver.h"
#if defined(__APPLE__)
#include "SessionRecorderScreenVisibilityRegistry.h"
#endif
#include "SessionRecorderPlatformLog.h"

#include <algorithm>
#include <stdexcept>

namespace lomtree
{
    namespace
    {
        constexpr int64_t ACTION_CAUSAL_WINDOW = 300;
        constexpr int64_t CONSEQUENCE_DELAY = 300;
    }

    int64_t SessionRecorderObserver::now()
    {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    // Constructor
    SessionRecorderObserver::SessionRecorderObserver(ObserverConfig config) : config_(config)
    {
        // Even zero-debounce work must leave the registry-held commit hook.
        debounceThread_ = std::thread([this] { run(); });
    }

    // Destructor
    SessionRecorderObserver::~SessionRecorderObserver()
    {
        shutdown();
    }

    void SessionRecorderObserver::shutdown()
    {
        {
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            isShuttingDown_.store(true);
#if defined(__APPLE__)
            pendingTouchRecovery_.reset();
#endif
            for (auto &entry : freshnessBySurface_)
                entry.second.interactionConsequence.reset();
        }
        pendingSignal_.notify_all();

        // Verify the thread before "pause" it
        if (debounceThread_.joinable())
        {
            debounceThread_.join();
        }
    }

    // * BUILD SCHEDULE

    void SessionRecorderObserver::removeSurfaceStateLocked(SurfaceId surfaceId)
    {
#if defined(__APPLE__)
        if (uiManager_) {
            if (auto visibility = screenVisibilityRegistry(*uiManager_)) visibility->removeSurface(surfaceId);
        }
        if (pendingTouchRecovery_ && pendingTouchRecovery_->surface == surfaceId)
            pendingTouchRecovery_.reset();
#endif
        pagers_->removeSurface(surfaceId);
        freshnessBySurface_.erase(surfaceId);
        captureDebounceAnchorBySurface_.erase(surfaceId);
        pendingCaptureSources_.erase(surfaceId);
        pendingMutationCaptures_.erase(surfaceId);
        inFlightMutationCaptures_.erase(surfaceId);
        pendingScreensForDebounce_.erase(
            std::remove(pendingScreensForDebounce_.begin(), pendingScreensForDebounce_.end(), surfaceId),
            pendingScreensForDebounce_.end());
        std::lock_guard<std::mutex> hashLock(hashCacheLock_);
        lastSnapshotsBySurface_.erase(surfaceId);
        stagedDirtySnapshotsBySurface_.erase(surfaceId);
        pendingSignal_.notify_one();
    }

    std::optional<std::vector<ActiveSurface>> SessionRecorderObserver::getActiveSurfaces()
    {
        try
        {
            std::vector<ActiveSurface> surfaces;
            {
                std::lock_guard<std::mutex> uimLock(uiManagerLock_);
                if (uiManager_ == nullptr || uiManagerUnregistering_ || isShuttingDown_.load())
                    return std::nullopt;

                std::unordered_map<SurfaceId, ShadowNodeFamily::Shared> registered;
                uiManager_->getShadowTreeRegistry().enumerate(
                    [&](const ShadowTree &tree, bool &)
                    {
                        const auto root = tree.getCurrentRevision().rootShadowNode;
                        if (!root) throw std::runtime_error("Surface root unavailable");
                        registered.emplace(tree.getSurfaceId(), root->getFamilyShared());
                        surfaces.push_back({tree.getSurfaceId(), {}});
                    });

                // No SDK membership writer can race this snapshot: they all use
                // uiManagerLock_. RN may retire it later; guarded writes below use
                // registry.visit() so removal cannot interleave check and update.
                std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                for (auto it = surfaceInstances_.begin(); it != surfaceInstances_.end();)
                {
                    const auto found = registered.find(it->first);
                    if (found == registered.end() || found->second != it->second.family)
                    {
                        removeSurfaceStateLocked(it->first);
                        it = surfaceInstances_.erase(it);
                    }
                    else ++it;
                }
                for (auto &surface : surfaces)
                {
                    const auto id = surface.surfaceId;
                    const auto &family = registered.at(id);
                    auto instance = surfaceInstances_.find(id);
                    if (instance == surfaceInstances_.end())
                        instance = surfaceInstances_.emplace(id, SurfaceInstance{family, std::to_string(++nextSurfaceInstance_)}).first;
                    surface.surfaceInstance = instance->second.token;
                }
            }
            return surfaces;
        }
        catch (...)
        {
            // Unavailable is not an authoritative empty registry.
            return std::nullopt;
        }
    }

    bool SessionRecorderObserver::visitCurrentSurface(
        SurfaceId surfaceId, const std::string &instance, const std::function<void()> &action)
    {
        std::lock_guard<std::mutex> uimLock(uiManagerLock_);
        if (!uiManager_ || uiManagerUnregistering_ || isShuttingDown_.load()) return false;
        bool current = false;
        uiManager_->getShadowTreeRegistry().visit(surfaceId, [&](const ShadowTree &tree)
        {
            const auto revision = tree.getCurrentRevision();
            const auto &root = revision.rootShadowNode;
            const auto known = surfaceInstances_.find(surfaceId);
            if (!root || known == surfaceInstances_.end() || known->second.token != instance ||
                known->second.family != root->getFamilyShared()) return;
            current = true;
            {
                std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                const auto state = freshnessBySurface_.find(surfaceId);
                if (state != freshnessBySurface_.end() && state->second.interactionConsequence &&
                    state->second.interactionConsequence->waitingForSignal)
                {
                    state->second.interactionConsequence->waitingForSignal = false;
                    pendingSignal_.notify_one();
                }
                observeInteractionRevisionLocked(surfaceId, root, revision.number);
            }
            action(); // Registry membership remains locked throughout the update.
        });
        return current;
    }

    bool SessionRecorderObserver::isSurfaceInstanceCurrent(SurfaceId surfaceId, const std::string &instance)
    {
        return visitCurrentSurface(surfaceId, instance, [] {});
    }

    bool SessionRecorderObserver::armInteractionConsequence(
        SurfaceId surfaceId, const std::string &instance, uint64_t captureGeneration)
    {
        try
        {
            bool armed = false;
            visitCurrentSurface(surfaceId, instance, [&]
            {
                std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                if (paused_ || isShuttingDown_.load() || captureGeneration_ != captureGeneration ||
                    postScrollDebtGeneration_ != 0) return;
                auto &opportunity = freshnessBySurface_[surfaceId].interactionConsequence;
                const auto time = now();
                if (!opportunity || (opportunity->deadline < 0 && time - opportunity->armedAt > ACTION_CAUSAL_WINDOW))
                {
                    opportunity = InteractionConsequence{surfaceInstances_.at(surfaceId).family, captureGeneration, time};
                }
                armed = true;
            });
            // An arm without a mutation schedules no scan or expiry wakeup.
            return armed;
        }
        catch (...) { return false; }
    }

    void SessionRecorderObserver::notifyCaptureResult(
        uint64_t requestId,
        uint64_t debtGeneration,
        CaptureResult result,
        uint64_t captureGeneration,
        std::vector<ActiveSurface> surfaces, uint64_t pagerVersion)
    {
        CaptureResultCallback resultCallback;
        {
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            if (paused_ || captureGeneration_ != captureGeneration)
                return;
            resultCallback = captureResultCallbackJS_;
        }
        if (isShuttingDown_.load() || !resultCallback)
        {
            return;
        }
        try
        {
            resultCallback(requestId, debtGeneration, result, captureGeneration, std::move(surfaces), pagerVersion);
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Capture result callback failed: %s", e.what());
        }
        catch (...)
        {
            LOG_ERROR("Capture result callback failed with unknown exception");
        }
    }

    // * SCAN SURFACE

    RootShadowNode::Unshared SessionRecorderObserver::shadowTreeWillCommit(
        ShadowTree const &shadowTree,
        RootShadowNode::Shared const &oldRootShadowNode,
        RootShadowNode::Unshared const &newRootShadowNode) noexcept
    {

        // RN can hold registry.visit() here. Only retain provenance; the worker
        // or a JSI entry verifies membership before mutating active state.
        try
        {
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            if (paused_ || isShuttingDown_.load()) return newRootShadowNode;
            const auto surfaceId = shadowTree.getSurfaceId();
            const auto family = newRootShadowNode->getFamilyShared();
            const auto time = now();
            const auto state = freshnessBySurface_.find(surfaceId);
            if (state != freshnessBySurface_.end())
            {
                auto &opportunity = state->second.interactionConsequence;
                if (opportunity && opportunity->family == family &&
                    opportunity->captureGeneration == captureGeneration_ && !scrollSuppressed_)
                {
                    // Reuse the commit signal's monotone timestamp. A late commit
                    // extends stabilization without moving the base deadline.
                    opportunity->lastMutationAt = time;
                    opportunity->waitingForSignal = false;
                    if (opportunity->deadline < 0)
                    {
                        if (time - opportunity->armedAt <= ACTION_CAUSAL_WINDOW)
                        {
                            opportunity->deadline = time + CONSEQUENCE_DELAY;
                        }
                        else opportunity.reset(); // Lazy expiry, without a wakeup of its own.
                    }
                    if (opportunity && opportunity->deadline >= 0 && opportunity->requiredRevision < 0)
                    {
                        // Retain only weak provenance until a published revision
                        // can be identified outside this pre-publication hook.
                        if (newRootShadowNode.get() != oldRootShadowNode.get())
                            opportunity->mutationRoot = newRootShadowNode;
                        else opportunity->mutationRoot.reset();
                    }
                }
            }
            const auto signal = std::find_if(pendingCommitSignals_.begin(), pendingCommitSignals_.end(),
                [&](const CommitSignal &entry) { return entry.surfaceId == surfaceId && entry.family == family && entry.suppressed == scrollSuppressed_; });
            if (signal == pendingCommitSignals_.end())
                pendingCommitSignals_.push_back({surfaceId, family, 1, time, scrollSuppressed_});
            else
            {
                ++signal->count;
                signal->timestamp = time;
            }
        }
        catch (...) { return newRootShadowNode; }
        pendingSignal_.notify_one();

        return newRootShadowNode;
    }

    // * Lock Guards

#if defined(__APPLE__)
    void SessionRecorderObserver::requestTouchCapture(PointerDownRecovery recovery)
    {
        TouchRecoveryClose close;
        {
            std::lock_guard<std::mutex> lock(debounceQueueLock_);
            if (isShuttingDown_.load() || paused_ || recovery.captureGeneration != captureGeneration_) {
                close = touchRecoveryClose_;
            } else if (!scrollSuppressed_) {
                pendingTouchRecovery_ = std::move(recovery);
                pendingSignal_.notify_one();
                return;
            }
        }
        if (close) close(recovery);
    }
#endif

    void SessionRecorderObserver::requestInitialCapture()
    {
        {
            std::lock_guard<std::mutex> lock(debounceQueueLock_);
            if (paused_)
            {
                return;
            }
            if (scrollSuppressed_)
            {
                commitPendingWhileSuppressed_ = true;
                return;
            }
            initialCaptureRequested_ = true;
        }
        pendingSignal_.notify_one();
    }

    bool SessionRecorderObserver::requestExplicitCapture(uint64_t requestId, uint64_t debtGeneration)
    {
        {
            std::lock_guard<std::mutex> lock(debounceQueueLock_);
            if (isShuttingDown_.load() || paused_ ||
                scrollSuppressed_ ||
                debtGeneration == 0 ||
                postScrollDebtGeneration_ != debtGeneration ||
                explicitCaptureRequested_ ||
                explicitCaptureInFlight_)
            {
                return false;
            }
            explicitCaptureRequestId_ = requestId;
            explicitCaptureDebtGeneration_ = debtGeneration;
            explicitCaptureRequested_ = true;
        }
        pendingSignal_.notify_one();
        return true;
    }

    uint64_t SessionRecorderObserver::setPaused(bool paused)
    {
        processPendingCommits();
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(debounceQueueLock_);
            if (paused_ != paused)
            {
                paused_ = paused;
                ++captureGeneration_;
            }
            if (paused)
            {
#if defined(__APPLE__)
                pendingTouchRecovery_.reset();
#endif
                inFlightMutationCaptures_.clear();
                for (auto &entry : freshnessBySurface_)
                    entry.second.interactionConsequence.reset();
                pendingCommitSignals_.clear();
                pendingScreensForDebounce_.clear();
                pendingCaptureSources_.clear();
                pendingMutationCaptures_.clear();
                initialCaptureRequested_ = false;
                explicitCaptureRequested_ = false;
                explicitCaptureInFlight_ = false;
                explicitCaptureRequestId_ = 0;
                explicitCaptureDebtGeneration_ = 0;

                // A pending JS delivery from the previous generation may be
                // discarded, so its exact snapshot must not remain a C2 baseline.
                // Keep the lock order queue -> hash shared with snapshot publication.
                std::lock_guard<std::mutex> hashLock(hashCacheLock_);
                lastSnapshotsBySurface_.clear();
                stagedDirtySnapshotsBySurface_.clear();
            }
            generation = captureGeneration_;
        }
        pendingSignal_.notify_all();
        return generation;
    }

    bool SessionRecorderObserver::isCaptureGenerationCurrent(uint64_t captureGeneration) const
    {
        std::lock_guard<std::mutex> lock(debounceQueueLock_);
        return !paused_ && captureGeneration_ == captureGeneration;
    }

    void SessionRecorderObserver::setPostScrollDebtGeneration(uint64_t generation)
    {
        std::lock_guard<std::mutex> lock(debounceQueueLock_);
        postScrollDebtGeneration_ = generation;
        if (generation != 0)
        {
            // Validated scroll owns the existing post-scroll capture pipeline.
            for (auto &entry : freshnessBySurface_)
                entry.second.interactionConsequence.reset();
            pendingSignal_.notify_one();
        }
    }

    void SessionRecorderObserver::setScrollSuppressed(bool suppressed, bool restorePendingAutomaticCapture)
    {
        processPendingCommits();
        bool notify = false;
        {
            std::lock_guard<std::mutex> lock(debounceQueueLock_);
            if (suppressed)
            {
                if (!pendingScreensForDebounce_.empty() || initialCaptureRequested_)
                {
                    commitPendingWhileSuppressed_ = true;
                }
                pendingScreensForDebounce_.clear();
                pendingCaptureSources_.clear();
                pendingMutationCaptures_.clear();
                initialCaptureRequested_ = false;
#if defined(__APPLE__)
                pendingTouchRecovery_.reset();
#endif
                scrollSuppressed_ = true;
                pendingSignal_.notify_one();
                return;
            }

            const bool wasSuppressed = scrollSuppressed_;
            scrollSuppressed_ = false;
            if (wasSuppressed && !paused_ && postScrollDebtGeneration_ == 0)
            {
                for (auto &entry : freshnessBySurface_)
                {
                    auto &opportunity = entry.second.interactionConsequence;
                    if (opportunity && opportunity->deadline >= 0)
                    {
                        // A provisional scroll is a barrier, not permission to
                        // inspect while moving. Resume its obligation after 300 ms.
                        opportunity->deadline = now() + CONSEQUENCE_DELAY;
                        opportunity->waitingForSignal = false;
                        notify = true;
                    }
                }
            }
            if (!paused_ && restorePendingAutomaticCapture && commitPendingWhileSuppressed_)
            {
                initialCaptureRequested_ = true;
                notify = true;
            }
        }
        if (notify)
        {
            pendingSignal_.notify_one();
        }
    }

    void SessionRecorderObserver::setSnapshotCallback(SnapshotCallback sc)
    {
        std::lock_guard<std::mutex> lockGuard(debounceQueueLock_);
        callbackJS_ = std::move(sc);
    }

    void SessionRecorderObserver::clearSnapshotCallback()
    {
        std::lock_guard<std::mutex> lockGuard(debounceQueueLock_);
        callbackJS_ = nullptr;
    }

    void SessionRecorderObserver::setCaptureResultCallback(CaptureResultCallback callback)
    {
        std::lock_guard<std::mutex> lockGuard(debounceQueueLock_);
        captureResultCallbackJS_ = std::move(callback);
    }

    void SessionRecorderObserver::clearCaptureResultCallback()
    {
        std::lock_guard<std::mutex> lockGuard(debounceQueueLock_);
        captureResultCallbackJS_ = nullptr;
    }

    void SessionRecorderObserver::unregisterFromUIManager()
    {
        UIManager const *registeredUIManager = nullptr;
        {
            std::lock_guard<std::mutex> lock(uiManagerLock_);
            if (uiManager_ == nullptr || uiManagerUnregistering_)
            {
                return;
            }
            registeredUIManager = uiManager_;
            uiManagerUnregistering_ = true;
        }

        // unregisterCommitHook synchronously calls commitHookWasUnregistered;
        // never hold uiManagerLock_ across this re-entrant callback.
        const_cast<facebook::react::UIManager *>(registeredUIManager)->unregisterCommitHook(*this);

        {
            std::lock_guard<std::mutex> lock(uiManagerLock_);
            if (uiManager_ == registeredUIManager)
            {
                uiManager_ = nullptr;
            }
            uiManagerUnregistering_ = false;
        }
    }

    void SessionRecorderObserver::resetUIManager()
    {
        pagers_->stop();
        // Invalidate queued/background work before dropping UIManager ownership.
        {
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            pendingScreensForDebounce_.clear();
            pendingCaptureSources_.clear();
            pendingMutationCaptures_.clear();
            pendingCommitSignals_.clear();
#if defined(__APPLE__)
            pendingTouchRecovery_.reset();
#endif
            initialCaptureRequested_ = false;
            paused_ = true;
            inFlightMutationCaptures_.clear();
            ++captureGeneration_;
            scrollSuppressed_ = false;
            commitPendingWhileSuppressed_ = false;
            explicitCaptureRequested_ = false;
            explicitCaptureInFlight_ = false;
            explicitCaptureRequestId_ = 0;
            explicitCaptureDebtGeneration_ = 0;
            postScrollDebtGeneration_ = 0;
            captureDebounceAnchorBySurface_.clear();
            freshnessBySurface_.clear();
        }

        pendingSignal_.notify_all();

        // Preserve the owner locally while unregistering. The callback may run
        // synchronously, so unregisterFromUIManager releases uiManagerLock_.
        unregisterFromUIManager();

        {
            std::lock_guard<std::mutex> uimLock(uiManagerLock_);
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            surfaceInstances_.clear();
            std::lock_guard<std::mutex> cacheLock(hashCacheLock_);
            lastSnapshotsBySurface_.clear();
            stagedDirtySnapshotsBySurface_.clear();
        }
    }

    void SessionRecorderObserver::commitHookWasRegistered(UIManager const &uiManager) noexcept
    {
        std::lock_guard<std::mutex> lock(uiManagerLock_);
        uiManager_ = &uiManager;
    }

    void SessionRecorderObserver::commitHookWasUnregistered(UIManager const &uiManager) noexcept
    {
        std::lock_guard<std::mutex> lock(uiManagerLock_);
        if (uiManager_ == &uiManager)
        {
            uiManager_ = nullptr;
            uiManagerUnregistering_ = false;
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            surfaceInstances_.clear();
            pendingCommitSignals_.clear();
            freshnessBySurface_.clear();
            inFlightMutationCaptures_.clear();
            pendingScreensForDebounce_.clear();
            pendingCaptureSources_.clear();
            pendingMutationCaptures_.clear();
            captureDebounceAnchorBySurface_.clear();
            std::lock_guard<std::mutex> cacheLock(hashCacheLock_);
            lastSnapshotsBySurface_.clear();
            stagedDirtySnapshotsBySurface_.clear();
            pendingSignal_.notify_all();
        }
    }
}
