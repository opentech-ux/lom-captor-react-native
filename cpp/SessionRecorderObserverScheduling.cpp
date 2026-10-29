#include "SessionRecorderObserver.h"

#include <algorithm>
#include <limits>

namespace lomtree
{
    void SessionRecorderObserver::run()
    {
        // While the app is running
        while (!isShuttingDown_.load())
        {
            std::unique_lock<std::mutex> uniqueLock(debounceQueueLock_);

            // The thread goes to sleep
            pendingSignal_.wait(uniqueLock,
                                [this]
                                {
                                    // If we're shutting down or if there are screens in the queue, the thread wakes up
                                    return isShuttingDown_.load() || (!paused_ && (
#if defined(__APPLE__)
                                        pendingTouchRecovery_.has_value() ||
#endif
                                        !pendingCommitSignals_.empty() || explicitCaptureRequested_ || initialCaptureRequested_ || !pendingScreensForDebounce_.empty() || hasInteractionDeadlineLocked()));
                                });

            if (isShuttingDown_.load())
                break;

            uniqueLock.unlock();
            const auto active = getActiveSurfaces();
            if (active) processPendingCommits();
            uniqueLock.lock();
            if (paused_) continue;
            if (!active)
            {
                if (explicitCaptureRequested_)
                {
                    const auto requestId = explicitCaptureRequestId_;
                    const auto debtGeneration = explicitCaptureDebtGeneration_;
                    const auto generation = captureGeneration_;
                    explicitCaptureRequested_ = false;
                    uniqueLock.unlock();
                    notifyCaptureResult(requestId, debtGeneration, CaptureResult::Unavailable, generation, {}, pagers_->status().version);
                    continue;
                }
                // Keep pending work until an existing entry wakes the worker;
                // unavailable membership is neither removal nor a polling loop.
                if (!isShuttingDown_.load()) pendingSignal_.wait(uniqueLock);
                continue;
            }

#if defined(__APPLE__)
            if (pendingTouchRecovery_) {
                const auto recovery = std::move(*pendingTouchRecovery_);
                pendingTouchRecovery_.reset();
                const auto check = touchRecoveryCheck_;
                const auto close = touchRecoveryClose_;
                const bool current = recovery.captureGeneration == captureGeneration_ &&
                    std::any_of(active->begin(), active->end(), [&](const ActiveSurface &surface) {
                        return surface.surfaceId == recovery.surface && surface.surfaceInstance == recovery.instance;
                    });
                uniqueLock.unlock();
                if (!current || !check || !check(recovery)) {
                    if (close) close(recovery);
                    continue;
                }
                const auto outcome = captureSurface(recovery.surface, recovery.captureGeneration, 0,
                    "pointerDownRecovery", recovery.instance, -1, &recovery);
                // A later real publication may already have queued a coalesced request
                // during this scan. Keep it only for a visibility-only failure.
                if (!outcome.visibilityUnavailable) {
                    if (close) close(recovery);
                    uniqueLock.lock();
                    if (pendingTouchRecovery_ && pendingTouchRecovery_->matches(recovery))
                        pendingTouchRecovery_.reset();
                }
                continue;
            }
#endif

            if (explicitCaptureRequested_)
            {
                const uint64_t requestId = explicitCaptureRequestId_;
                const uint64_t requestDebtGeneration = explicitCaptureDebtGeneration_;
                const uint64_t requestCaptureGeneration = captureGeneration_;
                explicitCaptureRequested_ = false;
                explicitCaptureInFlight_ = true;
                uniqueLock.unlock();

#if defined(__APPLE__)
                auto surfaces = *active;
#else
                const auto &surfaces = *active;
#endif
                const auto requestPagerVersion = pagers_->status().version;

                CaptureResult aggregate = CaptureResult::Deduplicated;
                bool inspectedSurface = false;
                bool unavailableSurface = surfaces.empty();
                for (auto &surface : surfaces)
                {
                    const CaptureOutcome outcome = captureSurface(surface.surfaceId, requestCaptureGeneration, requestId, "scroll", surface.surfaceInstance);
#if defined(__APPLE__)
                    surface.visibilityGeneration = outcome.visibilityGeneration;
#endif
                    if (outcome.debtGeneration != requestDebtGeneration)
                    {
                        unavailableSurface = true;
                        continue;
                    }
                    if (outcome.result == CaptureResult::Captured)
                    {
                        inspectedSurface = true;
                        aggregate = CaptureResult::Captured;
                    }
                    else if (outcome.result == CaptureResult::Deduplicated)
                    {
                        inspectedSurface = true;
                    }
                    else
                    {
                        unavailableSurface = true;
                    }
                }
                if (!inspectedSurface || unavailableSurface || !pagers_->current(requestPagerVersion))
                {
                    aggregate = CaptureResult::Unavailable;
                }

                {
                    std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                    explicitCaptureInFlight_ = false;
                }
                notifyCaptureResult(requestId, requestDebtGeneration, aggregate, requestCaptureGeneration, surfaces, requestPagerVersion);
                continue;
            }

            if (initialCaptureRequested_)
            {
                const uint64_t requestCaptureGeneration = captureGeneration_;
                initialCaptureRequested_ = false;
                if (scrollSuppressed_)
                {
                    commitPendingWhileSuppressed_ = true;
                    continue;
                }
                for (const auto &surface : *active)
                {
                    const auto id = surface.surfaceId;
                    const auto instance = surfaceInstances_.find(id);
                    if (instance == surfaceInstances_.end() || instance->second.token != surface.surfaceInstance) continue;
                    if (paused_ || captureGeneration_ != requestCaptureGeneration)
                        break;
                    if (std::find(pendingScreensForDebounce_.begin(), pendingScreensForDebounce_.end(), id) == pendingScreensForDebounce_.end())
                    {
                        pendingScreensForDebounce_.push_back(id);
                        captureDebounceAnchorBySurface_[id] = now() - config_.debounceMs;
                        pendingCaptureSources_[id] = "initial";
                    }
                }
            }
            const int64_t thresholdMs = config_.phase == DebouncePhase::Immediate ? 0 : config_.debounceMs;
            SurfaceId selectedSurfaceId = -1;
            int64_t nextDue = std::numeric_limits<int64_t>::max();
            int64_t consequenceArmedAt = -1;
            const auto time = now();

            for (const SurfaceId surfaceId : pendingScreensForDebounce_)
            {
                const auto due = captureDebounceAnchorBySurface_[surfaceId] + thresholdMs;
                if (due < nextDue)
                {
                    selectedSurfaceId = surfaceId;
                    nextDue = due;
                }
            }
            if (!scrollSuppressed_)
            {
                for (const auto &entry : freshnessBySurface_)
                {
                    const auto &opportunity = entry.second.interactionConsequence;
                    if (opportunity && opportunity->deadline >= 0 && !opportunity->waitingForSignal &&
                        !mutationCanCoverConsequenceLocked(entry.first, *opportunity))
                    {
                        const auto due = std::max(opportunity->deadline, opportunity->lastMutationAt + config_.debounceMs);
                        if (due < nextDue)
                        {
                            selectedSurfaceId = entry.first;
                            nextDue = due;
                            consequenceArmedAt = opportunity->armedAt;
                        }
                    }
                }
            }
            if (selectedSurfaceId == -1) continue;
            if (nextDue > time)
            {
                // Commits, lifecycle and suppression can interrupt this wait.
                pendingSignal_.wait_for(uniqueLock, std::chrono::milliseconds(nextDue - time));
                continue;
            }

            const auto provenance = pendingCaptureSources_.find(selectedSurfaceId);
            const char *source = consequenceArmedAt >= 0 ? "interactionConsequence" :
                (provenance == pendingCaptureSources_.end() ? "automatic" : provenance->second);
            pendingCaptureSources_.erase(selectedSurfaceId);
            pendingMutationCaptures_.erase(selectedSurfaceId);
            pendingScreensForDebounce_.erase(std::remove(pendingScreensForDebounce_.begin(), pendingScreensForDebounce_.end(), selectedSurfaceId), pendingScreensForDebounce_.end());
            const uint64_t requestCaptureGeneration = captureGeneration_;
            const std::string instance = surfaceInstances_.at(selectedSurfaceId).token;
            uniqueLock.unlock();
            CaptureOutcome outcome{CaptureResult::Unavailable, 0, requestCaptureGeneration};
            if (consequenceArmedAt >= 0)
            {
                try
                {
                    outcome = captureSurface(selectedSurfaceId, requestCaptureGeneration, 0, source, instance, consequenceArmedAt);
                }
                catch (...) {} // An attempted consequence is never replayed.
            }
            else
            {
                outcome = captureSurface(selectedSurfaceId, requestCaptureGeneration, 0, source, instance);
            }
            if (outcome.result == CaptureResult::Deduplicated && outcome.debtGeneration != 0)
            {
                notifyCaptureResult(0, outcome.debtGeneration, outcome.result, outcome.captureGeneration,
                    {{selectedSurfaceId, instance
#if defined(__APPLE__)
                        , outcome.visibilityGeneration
#endif
                    }}, outcome.pagerVersion);
            }
            if (consequenceArmedAt >= 0 && outcome.result == CaptureResult::Unavailable)
            {
                uniqueLock.lock();
                const auto state = freshnessBySurface_.find(selectedSurfaceId);
                // Leave other ordinary work runnable, but park this deadline
                // until a commit or guarded entry can verify ownership again.
                if (!paused_ && !scrollSuppressed_ &&
                    state != freshnessBySurface_.end() && state->second.interactionConsequence &&
                    state->second.interactionConsequence->armedAt == consequenceArmedAt &&
                    std::max(state->second.interactionConsequence->deadline,
                             state->second.interactionConsequence->lastMutationAt + config_.debounceMs) <= now() &&
                    pendingCommitSignals_.empty())
                    state->second.interactionConsequence->waitingForSignal = true;
            }
        }
    }

    void SessionRecorderObserver::processPendingCommits()
    {
        try
        {
            std::lock_guard<std::mutex> uimLock(uiManagerLock_);
            if (!uiManager_ || uiManagerUnregistering_ || isShuttingDown_.load()) return;
            std::vector<CommitSignal> signals;
            {
                std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                signals.swap(pendingCommitSignals_);
            }
            for (const auto &signal : signals)
            {
                uiManager_->getShadowTreeRegistry().visit(signal.surfaceId, [&](const ShadowTree &tree)
                {
                    const auto revision = tree.getCurrentRevision();
                    const auto &root = revision.rootShadowNode;
                    if (!root || root->getFamilyShared() != signal.family) return;
                    std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
                    if (paused_) return;
                    auto instance = surfaceInstances_.find(signal.surfaceId);
                    if (instance != surfaceInstances_.end() && instance->second.family != signal.family)
                    {
                        removeSurfaceStateLocked(signal.surfaceId);
                        surfaceInstances_.erase(instance);
                        instance = surfaceInstances_.end();
                    }
                    if (instance == surfaceInstances_.end())
                        surfaceInstances_.emplace(signal.surfaceId,
                            SurfaceInstance{signal.family, std::to_string(++nextSurfaceInstance_)});
                    freshnessBySurface_[signal.surfaceId].observedGeneration += signal.count;
                    observeInteractionRevisionLocked(signal.surfaceId, root, revision.number);
                    if (scrollSuppressed_ || signal.suppressed)
                    {
                        commitPendingWhileSuppressed_ = true;
                        return;
                    }
                    auto &lastChanged = captureDebounceAnchorBySurface_[signal.surfaceId];
                    lastChanged = std::max(lastChanged, signal.timestamp);
                    pendingCaptureSources_[signal.surfaceId] = "mutation";
                    pendingMutationCaptures_.insert_or_assign(signal.surfaceId,
                        MutationCapture{surfaceInstances_.at(signal.surfaceId).token, captureGeneration_, revision.number});
                    if (std::find(pendingScreensForDebounce_.begin(), pendingScreensForDebounce_.end(), signal.surfaceId) == pendingScreensForDebounce_.end())
                        pendingScreensForDebounce_.push_back(signal.surfaceId);
                });
            }
            if (!signals.empty()) pendingSignal_.notify_one();
        }
        catch (...) {}
    }

    bool SessionRecorderObserver::hasInteractionDeadlineLocked() const
    {
        if (scrollSuppressed_) return false;
        return std::any_of(freshnessBySurface_.begin(), freshnessBySurface_.end(),
            [this](const auto &entry)
            {
                const auto &opportunity = entry.second.interactionConsequence;
                return opportunity && opportunity->deadline >= 0 && !opportunity->waitingForSignal &&
                       !mutationCanCoverConsequenceLocked(entry.first, *opportunity);
            });
    }

    bool SessionRecorderObserver::mutationCanCoverConsequenceLocked(
        SurfaceId surfaceId, const InteractionConsequence &opportunity) const
    {
        const auto instance = surfaceInstances_.find(surfaceId);
        if (opportunity.requiredRevision < 0 || opportunity.captureGeneration != captureGeneration_ ||
            instance == surfaceInstances_.end() || instance->second.family != opportunity.family)
            return false;
        const auto covers = [&](const MutationCapture &capture)
        {
            return capture.instance == instance->second.token &&
                   capture.captureGeneration == opportunity.captureGeneration &&
                   capture.revisionNumber >= opportunity.requiredRevision;
        };
        const auto pending = pendingMutationCaptures_.find(surfaceId);
        if (pending != pendingMutationCaptures_.end() && covers(pending->second)) return true;
        const auto inFlight = inFlightMutationCaptures_.find(surfaceId);
        return inFlight != inFlightMutationCaptures_.end() &&
               std::any_of(inFlight->second.begin(), inFlight->second.end(), covers);
    }

    uint64_t SessionRecorderObserver::beginMutationDelivery(
        SurfaceId surfaceId, const std::string &instance, uint64_t captureGeneration, int64_t revisionNumber)
    {
        std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
        if (paused_ || isShuttingDown_.load() || captureGeneration_ != captureGeneration || revisionNumber < 0)
            return 0;
        const uint64_t deliveryId = ++nextMutationDeliveryId_;
        inFlightMutationCaptures_[surfaceId].push_back({instance, captureGeneration, revisionNumber, deliveryId});
        return deliveryId;
    }

    void SessionRecorderObserver::completeMutationDelivery(SurfaceId surfaceId, uint64_t deliveryId)
    {
        if (deliveryId == 0) return;
        std::lock_guard<std::mutex> queueLock(debounceQueueLock_);
        const auto inFlight = inFlightMutationCaptures_.find(surfaceId);
        if (inFlight == inFlightMutationCaptures_.end()) return;
        auto &captures = inFlight->second;
        captures.erase(std::remove_if(captures.begin(), captures.end(),
            [deliveryId](const MutationCapture &capture) { return capture.deliveryId == deliveryId; }), captures.end());
        if (captures.empty()) inFlightMutationCaptures_.erase(inFlight);
        // A delivery without a covering ACK leaves the consequence runnable.
        pendingSignal_.notify_one();
    }

    void SessionRecorderObserver::observeInteractionRevisionLocked(
        SurfaceId surfaceId, const RootShadowNode::Shared &root, int64_t revisionNumber)
    {
        const auto state = freshnessBySurface_.find(surfaceId);
        if (state == freshnessBySurface_.end()) return;
        auto &opportunity = state->second.interactionConsequence;
        if (!opportunity || opportunity->deadline < 0 || opportunity->requiredRevision >= 0) return;
        if (opportunity->captureGeneration != captureGeneration_ || opportunity->family != root->getFamilyShared()) return;
        if (const auto mutationRoot = opportunity->mutationRoot.lock(); mutationRoot == root)
        {
            // A revision number is credited only after this hook's root is
            // actually published. Old ACKs, failed commit attempts and timing
            // proximity alone cannot prove causal coverage.
            opportunity->requiredRevision = revisionNumber;
            opportunity->mutationRoot.reset();
            satisfyInteractionConsequenceLocked(surfaceId);
        }
    }

    void SessionRecorderObserver::satisfyInteractionConsequenceLocked(SurfaceId surfaceId)
    {
        auto &state = freshnessBySurface_.at(surfaceId);
        const auto &opportunity = state.interactionConsequence;
        if (opportunity && opportunity->requiredRevision >= 0 &&
            opportunity->captureGeneration == captureGeneration_ &&
            state.materializedCaptureGeneration == captureGeneration_ &&
            state.materializedRevision >= opportunity->requiredRevision)
        {
            state.interactionConsequence.reset();
            pendingSignal_.notify_one();
        }
    }
}
