#pragma once

#if defined(__APPLE__) || defined(ANDROID)
#include <react/renderer/core/ShadowNode.h>
#include <react/renderer/uimanager/UIManager.h>
#include <react/utils/ContextContainer.h>
#include <atomic>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lomtree
{
    class SessionRecorderScreenVisibilityRegistry final
    {
    public:
        using SurfaceId = facebook::react::SurfaceId;
        using Tag = facebook::react::Tag;
        using Family = facebook::react::ShadowNodeFamily::Shared;
        struct Identity
        {
            SurfaceId surface;
            Tag tag;
            Family rootFamily;
            Family family;
            bool matches(const Identity &other) const
            {
                return surface == other.surface && tag == other.tag &&
                    rootFamily == other.rootFamily && family == other.family;
            }
        };
        struct Observation
        {
            Identity identity;
            uint64_t nativeInstance = 0;
            int64_t mountedRevision = -1;
            bool known = false;
            bool inWindow = true;
#if defined(ANDROID)
            // Native header evidence; application-viewport logical coordinates, read on UI.
            struct BackButton {
                uint64_t instance = 0;
                float x = 0, y = 0, width = 0, height = 0;
                bool operator==(const BackButton &other) const {
                    return instance == other.instance && x == other.x && y == other.y &&
                        width == other.width && height == other.height;
                }
            } backButton;
            // Same private instance/box shape; never transports title content.
            using Title = BackButton;
            Title title;
#endif
        };
        static uint64_t key(SurfaceId surface, Tag tag)
        {
            return (uint64_t(uint32_t(surface)) << 32) | uint32_t(tag);
        }
        struct Snapshot
        {
            std::string activation;
            uint64_t generation = 0;
            std::unordered_map<uint64_t, Observation> screens;
#if defined(ANDROID)
            struct Alert {
                SurfaceId surface = 0;
                Family rootFamily;
                float viewportWidth = 0, viewportHeight = 0;
                Observation::BackButton dialog;
                std::array<Observation::BackButton, 3> buttons{};
                bool datePicker = false;
                bool timePicker = false;
                Observation::BackButton picker;
                bool operator==(const Alert &other) const {
                    return surface == other.surface && rootFamily == other.rootFamily &&
                        viewportWidth == other.viewportWidth && viewportHeight == other.viewportHeight &&
                        dialog == other.dialog && buttons == other.buttons &&
                        datePicker == other.datePicker && timePicker == other.timePicker && picker == other.picker;
                }
            };
            // Supported native dialog presence; Fabric remains in the capture.
            bool alertPresent = false;
            std::unordered_map<SurfaceId, Alert> alerts;
#endif
            const Observation *find(const Identity &identity) const
            {
                const auto it = screens.find(key(identity.surface, identity.tag));
                return it != screens.end() && it->second.identity.matches(identity) ? &it->second : nullptr;
            }
            bool detached(const Observation *observation, int64_t revision) const
            {
                return !activation.empty() && observation && observation->nativeInstance != 0 &&
                    observation->known &&
                    observation->mountedRevision == revision && !observation->inWindow;
            }
        };
        using RequestCallback = std::function<void(std::string, std::vector<Identity>)>;

        std::shared_ptr<const Snapshot> snapshot() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return snapshot_;
        }
        bool current(const Snapshot &snapshot) const
        {
            // Equivalent publications may advance mountedRevision without
            // changing the evidence held by an earlier immutable snapshot.
            return generation_.load() == snapshot.generation;
        }
#if defined(ANDROID) || defined(__APPLE__)
        bool current(uint64_t capturedGeneration) const noexcept
        {
            // Includes an invalidation reserved before the next snapshot is published.
            return generation_.load() == capturedGeneration;
        }
#endif
        void setRequestCallback(RequestCallback callback)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            request_ = std::move(callback);
        }
        void start(const std::string &activation)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (snapshot_->activation == activation) return;
            generation_.fetch_add(1);
            auto next = std::make_shared<Snapshot>();
            next->activation = activation;
            pending_.clear();
            publishLocked(std::move(next));
        }
        void stop(const std::string &activation = {})
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!activation.empty() && snapshot_->activation != activation) return;
            // Invalidate before allocations, including allocation failures.
            generation_.fetch_add(1);
            pending_.clear();
            auto next = std::make_shared<Snapshot>();
            publishLocked(std::move(next));
        }
        void request(std::vector<Identity> candidates)
        {
            RequestCallback callback;
            std::string activation;
            std::vector<Identity> missing;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (snapshot_->activation.empty() || !request_) return;
                for (auto &identity : candidates) {
                    const auto *existing = snapshot_->find(identity);
                    if (existing && existing->nativeInstance) continue;
                    auto k = key(identity.surface, identity.tag);
                    auto pending = pending_.find(k);
                    if (pending != pending_.end() && pending->second.matches(identity)) continue;
                    pending_.insert_or_assign(k, identity);
                    missing.push_back(std::move(identity));
                }
                if (missing.empty()) return;
                activation = snapshot_->activation;
                callback = request_;
            }
            callback(std::move(activation), std::move(missing));
        }
        void complete(const std::string &activation, const Identity &identity)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (snapshot_->activation != activation) return;
            auto it = pending_.find(key(identity.surface, identity.tag));
            if (it != pending_.end() && it->second.matches(identity)) pending_.erase(it);
        }
        void invalidateSurface(SurfaceId surface)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool changed = false;
            for (const auto &entry : snapshot_->screens)
                if (entry.second.identity.surface == surface && entry.second.known) { changed = true; break; }
            if (!changed) return;
            generation_.fetch_add(1);
            auto next = std::make_shared<Snapshot>(*snapshot_);
            for (auto &entry : next->screens)
                if (entry.second.identity.surface == surface) entry.second.known = false;
            publishLocked(std::move(next));
        }
        void invalidate(const std::string &activation, const Identity &identity, uint64_t instance)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto *existing = snapshot_->find(identity);
            if (snapshot_->activation != activation || !existing || existing->nativeInstance != instance || !existing->known) return;
            generation_.fetch_add(1);
            auto next = std::make_shared<Snapshot>(*snapshot_);
            next->screens.at(key(identity.surface, identity.tag)).known = false;
            publishLocked(std::move(next));
        }
        void publish(const std::string &activation, Observation observation)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activation.empty() || snapshot_->activation != activation) return;
            const auto *existing = snapshot_->find(observation.identity);
            if (existing && existing->nativeInstance > observation.nativeInstance) return;
            if (!existing || !sameEvidence(*existing, observation)) generation_.fetch_add(1);
            auto next = std::make_shared<Snapshot>(*snapshot_);
            next->screens.insert_or_assign(key(observation.identity.surface, observation.identity.tag), std::move(observation));
            publishLocked(std::move(next));
        }
        void publishSurface(const std::string &activation, SurfaceId surface, std::vector<Observation> observations)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activation.empty() || snapshot_->activation != activation) return;
            size_t previousCount = 0;
            for (const auto &entry : snapshot_->screens)
                if (entry.second.identity.surface == surface) ++previousCount;
            bool changed = previousCount != observations.size();
            if (!changed) {
                for (const auto &observation : observations) {
                    const auto *existing = snapshot_->find(observation.identity);
                    if (!existing || !sameEvidence(*existing, observation)) { changed = true; break; }
                }
            }
            // Revision-only updates keep the semantic generation. Real changes
            // invalidate existing captures before allocations, including failures.
            if (changed) generation_.fetch_add(1);
            auto next = std::make_shared<Snapshot>(*snapshot_);
            for (auto it = next->screens.begin(); it != next->screens.end();)
                if (it->second.identity.surface == surface) it = next->screens.erase(it); else ++it;
            for (auto &observation : observations) {
                next->screens.insert_or_assign(key(surface, observation.identity.tag), std::move(observation));
            }
            publishLocked(std::move(next));
        }
#if defined(ANDROID)
        void publishAlerts(const std::string &activation, bool present, std::vector<Snapshot::Alert> alerts)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activation.empty() || snapshot_->activation != activation) return;
            bool changed = snapshot_->alertPresent != present || snapshot_->alerts.size() != alerts.size();
            for (const auto &alert : alerts) {
                const auto old = snapshot_->alerts.find(alert.surface);
                if (old == snapshot_->alerts.end() || !(old->second == alert)) changed = true;
            }
            if (!changed) return;
            generation_.fetch_add(1);
            auto next = std::make_shared<Snapshot>(*snapshot_);
            next->alertPresent = present;
            next->alerts.clear();
            for (auto &alert : alerts) next->alerts.emplace(alert.surface, std::move(alert));
            publishLocked(std::move(next));
        }
#endif
        void remove(const std::string &activation, const Identity &identity, uint64_t instance)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto *existing = snapshot_->find(identity);
            if (snapshot_->activation != activation || !existing || existing->nativeInstance != instance) return;
            generation_.fetch_add(1);
            auto next = std::make_shared<Snapshot>(*snapshot_);
            next->screens.erase(key(identity.surface, identity.tag));
            publishLocked(std::move(next));
        }
        void removeSurface(SurfaceId surface)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto &entry : snapshot_->screens)
                if (entry.second.identity.surface == surface) { generation_.fetch_add(1); break; }
            auto next = std::make_shared<Snapshot>(*snapshot_);
            for (auto it = next->screens.begin(); it != next->screens.end();)
                if (it->second.identity.surface == surface) it = next->screens.erase(it); else ++it;
            for (auto it = pending_.begin(); it != pending_.end();)
                if (it->second.surface == surface) it = pending_.erase(it); else ++it;
            publishLocked(std::move(next));
        }
    private:
        static bool sameEvidence(const Observation &left, const Observation &right)
        {
            return left.identity.matches(right.identity) && left.nativeInstance == right.nativeInstance &&
                left.known == right.known && left.inWindow == right.inWindow
#if defined(ANDROID)
                && left.backButton == right.backButton && left.title == right.title
#endif
                ;
        }
        void publishLocked(std::shared_ptr<Snapshot> next)
        {
            // Semantic changes reserve the generation before allocations, under mutex_.
            next->generation = generation_.load();
            snapshot_ = std::move(next);
        }
        mutable std::mutex mutex_;
        std::atomic<uint64_t> generation_{0};
        std::shared_ptr<const Snapshot> snapshot_ = std::make_shared<Snapshot>();
        std::unordered_map<uint64_t, Identity> pending_;
        RequestCallback request_;
    };

    inline std::shared_ptr<SessionRecorderScreenVisibilityRegistry> screenVisibilityRegistry(
        const facebook::react::UIManager &manager)
    {
        auto context = manager.getContextContainer();
        if (!context) return nullptr;
        auto registry = context->find<std::weak_ptr<SessionRecorderScreenVisibilityRegistry>>(
            "SessionRecorderScreenVisibilityRegistry");
        return registry ? registry->lock() : nullptr;
    }
}
#endif
