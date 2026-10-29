#pragma once

#include <react/renderer/core/ShadowNode.h>
#include <react/renderer/components/view/BaseViewProps.h>
#if defined(__APPLE__)
#include <react/renderer/components/rnscreens/Props.h>
#endif
#include <react/renderer/components/root/RootShadowNode.h>
#include <react/renderer/uimanager/UIManager.h>
#include <react/renderer/mounting/ShadowTree.h>
#include <react/renderer/mounting/ShadowTreeRegistry.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lomtree
{
    class SessionRecorderPagerRegistry final
    {
    public:
        enum class Phase { Unknown = -1, Idle = 0, Dragging = 1, Settling = 2 };
        struct Pager
        {
            facebook::react::SurfaceId surface;
            facebook::react::Tag tag;
            uint64_t instance = 0;
            uint64_t nativeInstance = 0;
            facebook::react::ShadowNodeFamily::Shared family;
            facebook::react::ShadowNodeFamily::Shared rootFamily;
            std::vector<facebook::react::ShadowNodeFamily::Shared> children;
            Phase phase = Phase::Unknown;
            // This index is accredited only while phase == Idle.
            std::optional<int> index;
        };
        struct Snapshot
        {
            uint64_t version = 0;
            std::vector<Pager> pagers;
            bool allows(const facebook::react::ShadowNode &parent, const facebook::react::ShadowNode &child,
                        const facebook::react::ShadowNodeFamily::Shared &rootFamily) const
            {
                const char *name = parent.getComponentName();
                if (!name || std::strcmp(name, "RNCViewPager") != 0) return true;
                for (const auto &pager : pagers)
                {
                    if (pager.surface != parent.getSurfaceId() || pager.tag != parent.getTag() ||
                        pager.family != parent.getFamilyShared() || pager.rootFamily != rootFamily) continue;
                    const auto &children = parent.getChildren();
                    if (!pager.nativeInstance || pager.phase != Phase::Idle || !pager.index ||
                        *pager.index < 0 || static_cast<size_t>(*pager.index) >= children.size() ||
                        pager.children.size() != children.size()) return false;
                    for (size_t i = 0; i < children.size(); ++i)
                        if (pager.children[i] != children[i]->getFamilyShared()) return false;
                    return children[*pager.index]->getFamilyShared() == child.getFamilyShared();
                }
                return false;
            }
        };
        struct Update { bool active; bool capture; uint64_t version; };

        void start(const std::string &activation)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (activation_ == activation) return;
            activation_ = activation;
            pagers_.clear();
            changedLocked();
        }
        void stop(const std::string &activation = {})
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!activation.empty() && activation != activation_) return;
            if (activation_.empty() && pagers_.empty()) return;
            activation_.clear();
            pagers_.clear();
            changedLocked();
        }
        void removeSurface(facebook::react::SurfaceId surface)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            bool removed = false;
            for (auto it = pagers_.begin(); it != pagers_.end();)
                if (it->second.surface == surface) { it = pagers_.erase(it); removed = true; } else ++it;
            if (removed) changedLocked();
        }
        // Called only by platform mounting observers on main/UI. No native view is
        // touched while the Fabric registry is locked.
        std::vector<Pager> reconcile(const std::string &activation, const facebook::react::UIManager &manager)
        {
            std::vector<Pager> mounted;
            manager.getShadowTreeRegistry().enumerate([&](const facebook::react::ShadowTree &tree, bool &) {
                auto root = tree.getMountingCoordinator()->getBaseRevision().rootShadowNode;
                if (root) collect(*root, root->getFamilyShared(), mounted);
            });
            std::lock_guard<std::mutex> lock(mutex_);
            if (activation.empty() || activation != activation_) return {};
            std::unordered_map<uint64_t, Pager> next;
            bool changed = false;
            for (auto &pager : mounted)
            {
                auto old = pagers_.find(key(pager.surface, pager.tag));
                if (old != pagers_.end() && old->second.surface == pager.surface &&
                    old->second.family == pager.family && old->second.rootFamily == pager.rootFamily &&
                    old->second.children == pager.children) pager = old->second;
                else { pager.instance = ++nextInstance_; changed = true; }
                next.emplace(key(pager.surface, pager.tag), pager);
            }
            changed = changed || next.size() != pagers_.size();
            pagers_ = std::move(next);
            if (changed) changedLocked();
            return mounted;
        }
        Update publish(const std::string &activation, facebook::react::SurfaceId surface,
                       facebook::react::Tag tag, uint64_t instance, uint64_t nativeInstance,
                       int phase, int index)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pagers_.find(key(surface, tag));
            if (activation.empty() || activation != activation_ || it == pagers_.end() ||
                it->second.surface != surface || it->second.instance != instance)
                return {active_.load(), false, version_.load()};
            auto &pager = it->second;
            const auto oldPhase = pager.phase;
            const auto oldAccreditedIndex = oldPhase == Phase::Idle ? pager.index : std::nullopt;
            const bool wasActive = oldPhase == Phase::Dragging || oldPhase == Phase::Settling;
            const bool nativeChanged = pager.nativeInstance != nativeInstance;
            if (nativeChanged)
            {
                pager.nativeInstance = nativeInstance;
                pager.phase = Phase::Unknown;
                pager.index.reset();
            }
            if (index != -1)
            {
                if (index >= 0 && static_cast<size_t>(index) < pager.children.size()) {
                    pager.index = index;
                } else {
                    pager.index.reset();
                    if (pager.phase == Phase::Idle) pager.phase = Phase::Unknown;
                }
            }
            if (phase >= 0 && phase <= 2) pager.phase = static_cast<Phase>(phase);
            const auto accreditedIndex = pager.phase == Phase::Idle ? pager.index : std::nullopt;
            const bool isActive = pager.phase == Phase::Dragging || pager.phase == Phase::Settling;
            const bool capture = accreditedIndex &&
                (nativeChanged || oldPhase != Phase::Idle || oldAccreditedIndex != accreditedIndex);
            // Candidate changes while pages remain omitted do not stale a LOM.
            // Activity entry/exit still invalidates in-flight capture coverage.
            if (nativeChanged || wasActive != isActive || oldAccreditedIndex != accreditedIndex)
                changedLocked();
            return {active_.load(), capture, version_.load()};
        }
        void detach(const std::string &activation, facebook::react::SurfaceId surface,
                    facebook::react::Tag tag, uint64_t instance, uint64_t nativeInstance)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pagers_.find(key(surface, tag));
            if (activation != activation_ || it == pagers_.end() || it->second.instance != instance ||
                it->second.nativeInstance != nativeInstance) return;
            it->second.nativeInstance = 0;
            it->second.phase = Phase::Unknown;
            it->second.index.reset();
            changedLocked();
        }
        Update status() const { return {active_.load(), false, version_.load()}; }
        bool active() const { return active_.load(); }
        bool current(uint64_t version) const { return !active() && version_.load() == version; }
        Snapshot snapshot() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Snapshot result{version_.load(), {}};
            for (const auto &entry : pagers_) result.pagers.push_back(entry.second);
            return result;
        }
    private:
        static uint64_t key(facebook::react::SurfaceId surface, facebook::react::Tag tag)
        { return (static_cast<uint64_t>(static_cast<uint32_t>(surface)) << 32) | static_cast<uint32_t>(tag); }
        static void collect(const facebook::react::ShadowNode &node,
                            const facebook::react::ShadowNodeFamily::Shared &rootFamily,
                            std::vector<Pager> &result)
        {
            if (node.getTraits().check(facebook::react::ShadowNodeTraits::Trait::Hidden)) return;
            const auto *view = dynamic_cast<const facebook::react::BaseViewProps *>(node.getProps().get());
            if (view && view->opacity == 0) return;
            const char *name = node.getComponentName();
            if (name && std::strcmp(name, "RNSScreen") == 0) {
#if defined(__APPLE__)
                const auto *screen = dynamic_cast<const facebook::react::RNSScreenProps *>(node.getProps().get());
                if (screen && screen->activityState == 0) return;
#elif defined(RN_SERIALIZABLE_STATE)
                const auto &props = node.getProps();
                if (props && props->rawProps.isObject()) {
                    const auto state = props->rawProps.find("activityState");
                    if (state != props->rawProps.items().end() && state->second.isNumber() && state->second.asDouble() == 0) return;
                }
#endif
            }
            if (name && std::strcmp(name, "RNCViewPager") == 0)
            {
                Pager pager{node.getSurfaceId(), node.getTag()};
                pager.family = node.getFamilyShared();
                pager.rootFamily = rootFamily;
                for (const auto &child : node.getChildren()) pager.children.push_back(child->getFamilyShared());
                result.push_back(std::move(pager));
            }
            for (const auto &child : node.getChildren()) collect(*child, rootFamily, result);
        }
        void changedLocked()
        {
            bool active = false;
            for (const auto &entry : pagers_)
                active = active || entry.second.phase == Phase::Dragging || entry.second.phase == Phase::Settling;
            active_ = active;
            ++version_;
        }
        mutable std::mutex mutex_;
        std::string activation_;
        std::unordered_map<uint64_t, Pager> pagers_;
        uint64_t nextInstance_ = 0;
        std::atomic<uint64_t> version_{0};
        std::atomic<bool> active_{false};
    };
}
