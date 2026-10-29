#pragma once

#if defined(__APPLE__)

#include "SessionRecorderObserver.h"
#include "SessionRecorderPlatformLog.h"
#include <jsi/jsi.h>
#include <react/renderer/uimanager/UIManager.h>

namespace lomtree
{
    // One companion/runtime owns this handoff. Never shared through a process global.
    class SessionRecorderIOSOwner final : public facebook::jsi::HostObject
    {
    public:
        using ActiveTouchCallback = std::function<bool(const std::string &, uint64_t,
                                                      facebook::react::SurfaceId, const std::string &, uint64_t, bool, const std::function<void()> &)>;

        void setActiveTouchCallback(ActiveTouchCallback callback)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!invalidated_) activeTouch_ = std::move(callback);
        }

        bool withActiveTouch(const std::string &activation, uint64_t pointerId,
                             facebook::react::SurfaceId surface, const std::string &instance, uint64_t captureGeneration,
                             bool awaitingLom, const std::function<void()> &perform)
        {
            ActiveTouchCallback callback;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (invalidated_ || !alive_ || !alive_->load()) return false;
                callback = activeTouch_;
            }
            return callback && callback(activation, pointerId, surface, instance, captureGeneration, awaitingLom, perform);
        }

        void setScanClock(ScanClock clock)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!invalidated_ && observer_) observer_->setScanClock(std::move(clock));
        }

        void setTouchRecoveryCallbacks(TouchRecoveryCheck check, TouchRecoveryClose close)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!invalidated_ && observer_)
                observer_->setTouchRecoveryCallbacks(std::move(check), std::move(close));
        }

        void requestTouchCapture(PointerDownRecovery recovery)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!invalidated_ && alive_ && alive_->load() && observer_)
                observer_->requestTouchCapture(std::move(recovery));
        }

        ~SessionRecorderIOSOwner() override
        {
            invalidate();
        }

        bool prepare(std::shared_ptr<facebook::react::UIManager> uiManager)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (invalidated_ || !uiManager ||
                (uiManager_ && uiManager_ != uiManager))
                return false;
            uiManager_ = std::move(uiManager);
            return true;
        }

        bool install(
            facebook::react::UIManager &bindingUIManager,
            std::shared_ptr<std::atomic<bool>> alive,
            const std::function<bool(std::shared_ptr<SessionRecorderObserver>)> &installer)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (invalidated_ || !uiManager_ || uiManager_.get() != &bindingUIManager)
                return false;

            if (alive_) *alive_ = false;
            alive_ = std::move(alive);
            try
            {
                if (observer_)
                {
                    observer_->clearSnapshotCallback();
                    observer_->clearCaptureResultCallback();
                    observer_->resetUIManager();
                }
                else
                {
                    observer_ = std::make_shared<SessionRecorderObserver>();
                }
                if (installer(observer_)) return true;
            }
            catch (...)
            {
                LOG_ERROR("iOS JSI installation failed");
            }

            *alive_ = false;
            if (observer_)
            {
                observer_->clearSnapshotCallback();
                observer_->clearCaptureResultCallback();
                observer_->resetUIManager();
            }
            return false;
        }

        std::shared_ptr<SessionRecorderPagerRegistry> pagerRegistry()
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return observer_ ? observer_->pagerRegistry() : nullptr;
        }

        bool startObserver(
            facebook::react::UIManager &bindingUIManager,
            const std::shared_ptr<SessionRecorderObserver> &observer,
            bool requestInitialCapture)
        {
            // Serialize registration with native invalidation, including the alive check.
            std::lock_guard<std::mutex> lock(mutex_);
            if (invalidated_ || !alive_ || !alive_->load() ||
                uiManager_.get() != &bindingUIManager || observer != observer_)
                return false;
            observer_->setPaused(false);
            if (!observer_->isRegistered())
                bindingUIManager.registerCommitHook(*observer_);
            if (requestInitialCapture) observer_->requestInitialCapture();
            return true;
        }

        void invalidate() noexcept
        {
            try
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (invalidated_ && !observer_ && !uiManager_) return;
                invalidated_ = true;
                activeTouch_ = nullptr;
                if (alive_) *alive_ = false;
                if (observer_)
                {
                    // UIManager stays owned until scans finish and the hook is removed.
                    observer_->clearSnapshotCallback();
                    observer_->clearCaptureResultCallback();
                    observer_->resetUIManager();
                    observer_->shutdown();
                    observer_.reset();
                }
                uiManager_.reset();
                alive_.reset();
            }
            catch (...)
            {
                // Do not let native cleanup throw into the host. Retain ownership
                // if unregister did not complete rather than dropping its dependencies.
                LOG_ERROR("iOS native owner cleanup failed");
            }
        }

    private:
        std::mutex mutex_;
        ActiveTouchCallback activeTouch_;
        bool invalidated_ = false;
        std::shared_ptr<facebook::react::UIManager> uiManager_;
        std::shared_ptr<SessionRecorderObserver> observer_;
        std::shared_ptr<std::atomic<bool>> alive_;
    };
}

#endif
