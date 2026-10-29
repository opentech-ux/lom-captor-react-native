#include "SessionRecorderObserver.h"
#include "SessionRecorderPlatformLog.h"

#include <algorithm>
#include <cmath>
#include <exception>

namespace lomtree
{
#if defined(__APPLE__)
    bool SessionRecorderObserver::isIOSVisibilityGenerationCurrentLocked(uint64_t generation) const
    {
        if (!uiManager_ || uiManagerUnregistering_ || isShuttingDown_.load()) return false;
        const auto visibility = screenVisibilityRegistry(*uiManager_);
        return visibility && visibility->current(generation);
    }

    bool SessionRecorderObserver::isIOSVisibilityGenerationCurrent(uint64_t generation) const
    {
        std::lock_guard<std::mutex> uimLock(uiManagerLock_);
        return isIOSVisibilityGenerationCurrentLocked(generation);
    }
#endif

    namespace
    {
        bool sameNativeRoots(const std::vector<Root> &left, const std::vector<Root> &right)
        {
            return left.size() == right.size() &&
                   std::equal(left.begin(), left.end(), right.begin(),
                              [](const Root &a, const Root &b)
                              {
                                  return a.tag == b.tag &&
                                         a.parentTag == b.parentTag &&
                                         a.name == b.name &&
                                         a.depth == b.depth &&
                                         a.x == b.x && a.y == b.y &&
                                         a.width == b.width && a.height == b.height;
                              });
        }
    }

    CaptureOutcome SessionRecorderObserver::captureSurface(SurfaceId surfaceId, uint64_t captureGeneration, uint64_t explicitRequestId, const char *source, const std::string &instance, int64_t consequenceArmedAt
#if defined(__APPLE__)
                                                                    , const PointerDownRecovery *recovery
#endif
                                                                    )
    {
        if (pagers_->active() || !isSurfaceInstanceCurrent(surfaceId, instance))
            return {CaptureResult::Unavailable, 0, captureGeneration};
        uint64_t captureDebtGeneration = 0;
        ScanClock scanClock;
        ScanTiming scanTiming;
        {
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            captureDebtGeneration = postScrollDebtGeneration_;
            scanClock = scanClock_;
            if (paused_ || captureGeneration_ != captureGeneration)
            {
                return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
            }
            if (scrollSuppressed_)
            {
                commitPendingWhileSuppressed_ = true;
                return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
            }
        }

        TreeCapture treeCapture;
        {
            std::lock_guard<std::mutex> uimLock(uiManagerLock_);
            if (uiManager_ == nullptr || uiManagerUnregistering_)
            {
                return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
            }
            SessionRecorderTreeScanner scanner(pagers_);
#if defined(ANDROID)
            scanner.androidVisibility = androidVisibility;
#endif
            if (consequenceArmedAt >= 0)
            {
                bool current = false;
                uiManager_->getShadowTreeRegistry().visit(surfaceId, [&](const ShadowTree &tree)
                {
                    const auto root = tree.getCurrentRevision().rootShadowNode;
                    const auto known = surfaceInstances_.find(surfaceId);
                    current = root && known != surfaceInstances_.end() && known->second.token == instance &&
                        known->second.family == root->getFamilyShared();
                });
                std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                const auto state = freshnessBySurface_.find(surfaceId);
                if (!current || paused_ || scrollSuppressed_ || isShuttingDown_.load() ||
                    captureGeneration_ != captureGeneration || postScrollDebtGeneration_ != 0 ||
                    state == freshnessBySurface_.end() || !state->second.interactionConsequence)
                    return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
                const auto &opportunity = *state->second.interactionConsequence;
                if (opportunity.captureGeneration != captureGeneration ||
                    opportunity.family != surfaceInstances_.at(surfaceId).family ||
                    opportunity.armedAt != consequenceArmedAt || opportunity.deadline < 0 ||
                    std::max(opportunity.deadline, opportunity.lastMutationAt + config_.debounceMs) > now())
                    return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
                // Consume only at the scanner boundary, not when selected by
                // the worker or when blocked by a pre-inspection gate.
                state->second.interactionConsequence.reset();
            }
#if defined(__APPLE__)
            if (recovery) {
                TouchRecoveryCheck check;
                {
                    std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                    if (paused_ || scrollSuppressed_ || isShuttingDown_.load() ||
                        captureGeneration_ != recovery->captureGeneration)
                        return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
                    check = touchRecoveryCheck_;
                }
                // Recheck after acquiring UIManager, without holding a contact lock
                // during scanning. Physical timing still bounds adoption at terminal.
                if (!check || !check(*recovery) || pagers_->active())
                    return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
            }
#endif
            const auto start = scanClock ? scanClock() : ScanClockSample{};
            treeCapture = scanner.captureTree(
                surfaceId,
                uiManager_,
                static_cast<size_t>(config_.maxNodes));
            const auto end = scanClock ? scanClock() : ScanClockSample{};
            // This interval encloses root acquisition and all strict scan guards.
            // The touch observer supplies both samples in its activation's physical clock.
            if (!start.activation.empty() && start.activation == end.activation &&
                std::isfinite(start.time) && std::isfinite(end.time) &&
                start.time >= 0 && end.time >= start.time) {
                scanTiming = {start.activation, start.time, end.time};
            }
        }
        auto &roots = treeCapture.roots;

        {
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            if (paused_ || captureGeneration_ != captureGeneration)
            {
                return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
            }
        }

        if (!pagers_->current(treeCapture.pagerVersion) || roots.empty() || treeCapture.revisionNumber < 0)
        {
            CaptureOutcome unavailable{CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
#if defined(__APPLE__)
            unavailable.visibilityUnavailable = treeCapture.visibilityUnavailable &&
                pagers_->current(treeCapture.pagerVersion);
#endif
            return unavailable;
        }

        SnapshotCallback snapshotCallback;
        {
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            if (paused_ || captureGeneration_ != captureGeneration)
            {
                return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
            }
            if (scrollSuppressed_)
            {
                commitPendingWhileSuppressed_ = true;
                return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
            }
            snapshotCallback = callbackJS_;
        }
        if (!snapshotCallback || isShuttingDown_.load())
        {
            return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
        }
#if defined(ANDROID)
        if (!androidVisibility->current(treeCapture.visibilityGeneration))
            return {CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
#elif defined(__APPLE__)
        if (!isIOSVisibilityGenerationCurrent(treeCapture.visibilityGeneration))
        {
            CaptureOutcome unavailable{CaptureResult::Unavailable, captureDebtGeneration, captureGeneration};
            unavailable.visibilityUnavailable = true;
            return unavailable;
        }
#endif

        const uint64_t signature = hashRoots(roots);
        CaptureResult result = CaptureResult::Unavailable;
#if defined(__APPLE__)
        bool visibilityUnavailable = false;
#endif
        bool copyingSnapshot = false;
        try
        {
            visitCurrentSurface(surfaceId, instance, [&]
            {
                if (!pagers_->current(treeCapture.pagerVersion) ||
                    surfaceInstances_.at(surfaceId).family != treeCapture.surfaceFamily) return;
                {
                    std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                    if (paused_ || scrollSuppressed_ || captureGeneration_ != captureGeneration) return;
#if defined(ANDROID)
                    if (!androidVisibility->current(treeCapture.visibilityGeneration)) return;
#elif defined(__APPLE__)
                    if (!isIOSVisibilityGenerationCurrentLocked(treeCapture.visibilityGeneration))
                    {
                        visibilityUnavailable = true;
                        return;
                    }
#endif
                    std::lock_guard<std::mutex> hashLock(hashCacheLock_);
                    const auto previous = lastSnapshotsBySurface_.find(surfaceId);
                    const auto freshness = freshnessBySurface_.find(surfaceId);
                    commitPendingWhileSuppressed_ = false;
                    if (previous != lastSnapshotsBySurface_.end() &&
                        freshness != freshnessBySurface_.end() &&
                        freshness->second.materializedRevision >= 0 &&
                        previous->second.revisionNumber == freshness->second.materializedRevision &&
                        previous->second.pagerVersion == freshness->second.materializedPagerVersion &&
#if defined(__APPLE__)
                        previous->second.visibilityGeneration == freshness->second.materializedVisibilityGeneration &&
#endif
                        freshness->second.materializedCaptureGeneration == captureGeneration &&
                        treeCapture.revisionNumber >= freshness->second.materializedRevision &&
                        previous->second.viewportWidth == treeCapture.viewportWidth &&
                        previous->second.viewportHeight == treeCapture.viewportHeight &&
                        previous->second.signature == signature && sameNativeRoots(previous->second.roots, roots))
                    {
                        // Derive acceptance here so either ACK/snapshot order is valid.
#if defined(ANDROID)
                        if (!androidVisibility->current(treeCapture.visibilityGeneration)) return;
#elif defined(__APPLE__)
                        if (!isIOSVisibilityGenerationCurrentLocked(treeCapture.visibilityGeneration))
                        {
                            visibilityUnavailable = true;
                            return;
                        }
#endif
                        freshness->second.materializedRevision =
                            std::max(freshness->second.materializedRevision, treeCapture.revisionNumber);
                        freshness->second.materializedCaptureGeneration = captureGeneration;
                        freshness->second.materializedPagerVersion = treeCapture.pagerVersion;
#if defined(ANDROID) || defined(__APPLE__)
                        freshness->second.materializedVisibilityGeneration = treeCapture.visibilityGeneration;
#endif
                        previous->second.revisionNumber = freshness->second.materializedRevision;
                        previous->second.pagerVersion = treeCapture.pagerVersion;
#if defined(ANDROID) || defined(__APPLE__)
                        previous->second.visibilityGeneration = treeCapture.visibilityGeneration;
#endif
                        satisfyInteractionConsequenceLocked(surfaceId);
                        result = CaptureResult::Deduplicated;
                        return;
                    }
                }
                // Copy only for publication, before the callback takes the roots.
                // Copy failures still propagate; callback failures remain contained.
                copyingSnapshot = true;
                NativeSnapshot processedSnapshot{signature, roots, treeCapture.revisionNumber,
                                                 treeCapture.viewportWidth, treeCapture.viewportHeight, treeCapture.pagerVersion};
#if defined(ANDROID) || defined(__APPLE__)
                processedSnapshot.visibilityGeneration = treeCapture.visibilityGeneration;
#endif
                copyingSnapshot = false;
                snapshotCallback(surfaceId, std::move(roots), treeCapture.viewportWidth, treeCapture.viewportHeight,
                                 signature, explicitRequestId, captureDebtGeneration, captureGeneration,
                                 treeCapture.revisionNumber, source, instance, treeCapture.pagerVersion, std::move(scanTiming)
#if defined(ANDROID) || defined(__APPLE__)
                                 , treeCapture.visibilityGeneration
#endif
                );
                std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                if (paused_ || captureGeneration_ != captureGeneration) return;
#if defined(ANDROID)
                if (!androidVisibility->current(treeCapture.visibilityGeneration)) return;
#elif defined(__APPLE__)
                if (!isIOSVisibilityGenerationCurrentLocked(treeCapture.visibilityGeneration))
                {
                    visibilityUnavailable = true;
                    return;
                }
#endif
                std::lock_guard<std::mutex> hashLock(hashCacheLock_);
                const auto previous = lastSnapshotsBySurface_.find(surfaceId);
                if (previous == lastSnapshotsBySurface_.end() ||
                    previous->second.revisionNumber <= processedSnapshot.revisionNumber)
                {
                    lastSnapshotsBySurface_.insert_or_assign(surfaceId, std::move(processedSnapshot));
                }
                result = CaptureResult::Captured;
            });
        }
        catch (const std::exception &e)
        {
            if (copyingSnapshot) throw;
            LOG_ERROR("Callback failed: %s", e.what());
        }
        catch (...)
        {
            if (copyingSnapshot) throw;
            LOG_ERROR("Callback failed with unknown exception");
        }
        CaptureOutcome outcome{result, captureDebtGeneration, captureGeneration, treeCapture.pagerVersion};
#if defined(__APPLE__)
        outcome.visibilityUnavailable = visibilityUnavailable;
        outcome.visibilityGeneration = treeCapture.visibilityGeneration;
#endif
        return outcome;
    }

    DirtyCaptureOutcome SessionRecorderObserver::captureIfDirty(
        SurfaceId surfaceId, const std::string &instance, uint64_t priorityDebtGeneration)
    {
        processPendingCommits();
        DirtyCaptureOutcome outcome;
#if defined(__APPLE__)
        std::shared_ptr<SessionRecorderScreenVisibilityRegistry> iosVisibility;
#endif
        if (pagers_->active()) return outcome;
        outcome.surfaceId = surfaceId;
        outcome.surfaceInstance = instance;
        const bool priority = priorityDebtGeneration != 0;
        try
        {
            {
                std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                const auto state = freshnessBySurface_.find(surfaceId);
                if (state != freshnessBySurface_.end())
                {
                    outcome.observedGeneration = state->second.observedGeneration;
                    outcome.materializedRevision = state->second.materializedRevision;
                }
                outcome.captureGeneration = captureGeneration_;
                outcome.debtGeneration = postScrollDebtGeneration_;
                if (paused_ || (!priority && scrollSuppressed_) || isShuttingDown_.load() ||
                    (priority && (postScrollDebtGeneration_ != priorityDebtGeneration ||
                                  explicitCaptureRequested_ || explicitCaptureInFlight_))) return outcome;
            }

            {
                // The same lifetime guard used by the background scanner also protects
                // the synchronous JSI read. Registry/revision reads have their own RN locks.
                std::lock_guard<std::mutex> uimLock(uiManagerLock_);
                if (uiManager_ == nullptr || uiManagerUnregistering_) return outcome;
#if defined(__APPLE__)
                iosVisibility = screenVisibilityRegistry(*uiManager_);
#endif

                const auto &registry = uiManager_->getShadowTreeRegistry();
                int64_t publishedRevision = -1;
                if (!registry.visit(surfaceId, [&](const ShadowTree &tree)
                                    {
                                        const auto revision = tree.getCurrentRevision();
                                        const auto known = surfaceInstances_.find(surfaceId);
                                        if (revision.rootShadowNode && known != surfaceInstances_.end() &&
                                            known->second.token == outcome.surfaceInstance &&
                                            known->second.family == revision.rootShadowNode->getFamilyShared())
                                            publishedRevision = revision.number;
                                    }) || publishedRevision < 0)
                    return outcome;
                outcome.latestRevision = publishedRevision;

                bool pagerCovered = false;
                {
                    std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                    const auto state = freshnessBySurface_.find(surfaceId);
                    pagerCovered = state != freshnessBySurface_.end() &&
                        pagers_->current(state->second.materializedPagerVersion)
#if defined(ANDROID)
                        && androidVisibility->current(state->second.materializedVisibilityGeneration)
#elif defined(__APPLE__)
                        && isIOSVisibilityGenerationCurrentLocked(state->second.materializedVisibilityGeneration)
#endif
                        ;
                }
                const bool dirty = !pagerCovered || publishedRevision > outcome.materializedRevision;
                if (!priority && pagerCovered && publishedRevision <= outcome.materializedRevision)
                {
                    outcome.status = DirtyCaptureStatus::Clean;
                    return outcome;
                }

                size_t surfaceCount = 0;
                registry.enumerate([&](const ShadowTree &, bool &stop)
                                   { if (++surfaceCount > 1) stop = true; });
                if (surfaceCount != 1) return outcome;

                SessionRecorderTreeScanner scanner(pagers_);
#if defined(ANDROID)
                scanner.androidVisibility = androidVisibility;
#endif
                if (priority)
                {
                    std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                    if (paused_ || isShuttingDown_.load() || captureGeneration_ != outcome.captureGeneration ||
                        postScrollDebtGeneration_ != priorityDebtGeneration ||
                        explicitCaptureRequested_ || explicitCaptureInFlight_) return outcome;
                    // Only an actual scanner call consumes the PointerDown opportunity.
                    outcome.priorityAttempted = true;
                }
                outcome.dirty = dirty;
#if defined(__APPLE__)
                outcome.tree = scanner.captureTree(surfaceId, uiManager_, static_cast<size_t>(config_.maxNodes), true);
#else
                outcome.tree = scanner.captureTree(surfaceId, uiManager_, static_cast<size_t>(config_.maxNodes));
#endif
                if (outcome.tree.surfaceFamily != surfaceInstances_.at(surfaceId).family) return outcome;
            }

            if (outcome.tree.roots.empty() || outcome.tree.revisionNumber < 0)
            {
                return outcome;
            }
            outcome.latestRevision = std::max(outcome.latestRevision, outcome.tree.revisionNumber);
            {
                std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                if (paused_ || (!priority && scrollSuppressed_) || captureGeneration_ != outcome.captureGeneration ||
                    isShuttingDown_.load() || (priority && postScrollDebtGeneration_ != priorityDebtGeneration))
                {
                    return outcome;
                }
                const auto state = freshnessBySurface_.find(surfaceId);
                if (state != freshnessBySurface_.end())
                {
                    outcome.observedGeneration = state->second.observedGeneration;
                    outcome.materializedRevision = state->second.materializedRevision;
                }
                if (!priority && outcome.tree.revisionNumber <= outcome.materializedRevision
#if defined(ANDROID)
                    && state != freshnessBySurface_.end() &&
                    state->second.materializedPagerVersion == outcome.tree.pagerVersion
                    && state->second.materializedVisibilityGeneration == outcome.tree.visibilityGeneration
#elif defined(__APPLE__)
                    && state != freshnessBySurface_.end() &&
                    state->second.materializedPagerVersion == outcome.tree.pagerVersion &&
                    state->second.materializedVisibilityGeneration == outcome.tree.visibilityGeneration &&
                    iosVisibility && iosVisibility->current(outcome.tree.visibilityGeneration)
#endif
                )
                {
                    outcome.status = DirtyCaptureStatus::Clean;
                    outcome.tree.roots.clear();
                    return outcome;
                }
            }

            outcome.signature = hashRoots(outcome.tree.roots);
            visitCurrentSurface(surfaceId, outcome.surfaceInstance, [&]
            {
                if (outcome.tree.surfaceFamily != surfaceInstances_.at(surfaceId).family) return;
                std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                if (paused_ || (!priority && scrollSuppressed_) || captureGeneration_ != outcome.captureGeneration ||
                    (priority && postScrollDebtGeneration_ != priorityDebtGeneration)) return;
                if (!pagers_->current(outcome.tree.pagerVersion)) return;
#if defined(ANDROID)
                if (!androidVisibility->current(outcome.tree.visibilityGeneration)) return;
#elif defined(__APPLE__)
                if (!isIOSVisibilityGenerationCurrentLocked(outcome.tree.visibilityGeneration)) return;
#endif
                outcome.status = DirtyCaptureStatus::Captured;
            });
            return outcome;
        }
        catch (...)
        {
            if (!priority) throw;
            outcome.status = DirtyCaptureStatus::Unavailable;
            return outcome;
        }
    }

    void SessionRecorderObserver::stageDirtySnapshot(
        SurfaceId surfaceId, const std::string &instance, uint64_t captureGeneration, uint64_t signature, TreeCapture &&tree)
    {
        visitCurrentSurface(surfaceId, instance, [&]
        {
            if (surfaceInstances_.at(surfaceId).family != tree.surfaceFamily) return;
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            if (paused_ || captureGeneration_ != captureGeneration || tree.revisionNumber < 0 || !pagers_->current(tree.pagerVersion)) return;
#if defined(ANDROID)
            if (!androidVisibility->current(tree.visibilityGeneration)) return;
#elif defined(__APPLE__)
            if (!isIOSVisibilityGenerationCurrentLocked(tree.visibilityGeneration)) return;
#endif
            const auto state = freshnessBySurface_.find(surfaceId);
            if (state != freshnessBySurface_.end() &&
                tree.revisionNumber <= state->second.materializedRevision &&
                state->second.materializedPagerVersion == tree.pagerVersion
#if defined(ANDROID) || defined(__APPLE__)
                && state->second.materializedVisibilityGeneration == tree.visibilityGeneration
#endif
                ) return;
            std::lock_guard<std::mutex> hashLock(hashCacheLock_);
            stagedDirtySnapshotsBySurface_.insert_or_assign(
                surfaceId, NativeSnapshot{signature, std::move(tree.roots), tree.revisionNumber,
                                          tree.viewportWidth, tree.viewportHeight, tree.pagerVersion
#if defined(ANDROID) || defined(__APPLE__)
                                          , tree.visibilityGeneration
#endif
                });
        });
    }

    bool SessionRecorderObserver::acceptMaterializedRevision(
        SurfaceId surfaceId, int64_t revisionNumber, uint64_t captureGeneration, const std::string &instance, uint64_t pagerVersion
#if defined(ANDROID) || defined(__APPLE__)
        , uint64_t visibilityGeneration
#endif
    )
    {
        bool accepted = false;
        visitCurrentSurface(surfaceId, instance, [&]
        {
            std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
            if (paused_ || captureGeneration_ != captureGeneration || revisionNumber < 0 || !pagers_->current(pagerVersion)) return;
#if defined(ANDROID)
            // Compare this attempt's evidence, not a baseline shared by revision/key.
            if (!androidVisibility->current(visibilityGeneration)) return;
#elif defined(__APPLE__)
            if (!isIOSVisibilityGenerationCurrentLocked(visibilityGeneration)) return;
#endif
            auto &state = freshnessBySurface_[surfaceId];
            if (revisionNumber < state.materializedRevision) return;
            state.materializedRevision = std::max(state.materializedRevision, revisionNumber);
            state.materializedCaptureGeneration = captureGeneration;
            state.materializedPagerVersion = pagerVersion;
#if defined(ANDROID) || defined(__APPLE__)
            state.materializedVisibilityGeneration = visibilityGeneration;
#endif
            satisfyInteractionConsequenceLocked(surfaceId);

            std::lock_guard<std::mutex> hashLock(hashCacheLock_);
            const auto staged = stagedDirtySnapshotsBySurface_.find(surfaceId);
            if (staged != stagedDirtySnapshotsBySurface_.end() &&
                staged->second.revisionNumber <= revisionNumber)
            {
                if (staged->second.revisionNumber == revisionNumber && staged->second.pagerVersion == pagerVersion
#if defined(ANDROID) || defined(__APPLE__)
                    && staged->second.visibilityGeneration == visibilityGeneration
#endif
                )
                {
                    const auto previous = lastSnapshotsBySurface_.find(surfaceId);
                    if (previous == lastSnapshotsBySurface_.end() ||
                        previous->second.revisionNumber <= revisionNumber)
                    {
                        lastSnapshotsBySurface_.insert_or_assign(surfaceId, std::move(staged->second));
                    }
                }
                stagedDirtySnapshotsBySurface_.erase(staged);
            }
            accepted = true;
        });
        return accepted;
    }

    uint64_t SessionRecorderObserver::hashRoot(const Root &root)
    {
        uint64_t hash = 14695981039346656037ULL;
        auto mix = [&](uint64_t componentValue)
        { hash ^= componentValue; hash *= 1099511628211ULL; };

        const float TOLERANCE = 4.0f;
        auto bucket = [&](float value) -> uint64_t
        {
            return static_cast<uint64_t>(std::round(value / TOLERANCE));
        };

        mix((uint64_t)root.depth);

        mix(bucket(root.x));
        mix(bucket(root.y));
        mix(bucket(root.width));
        mix(bucket(root.height));

        for (char c : root.name)
        {
            mix((uint64_t)c);
        }

        return hash;
    }

    uint64_t SessionRecorderObserver::hashRoots(const std::vector<Root> &roots)
    {
        uint64_t seed = roots.size();
        for (const auto &n : roots)
        {
            uint64_t value = hashRoot(n);
            seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
}
