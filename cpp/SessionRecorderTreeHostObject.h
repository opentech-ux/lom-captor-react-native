#pragma once

#include "SessionRecorderTreeScanner.h"
#include "SessionRecorderObserver.h"
#if defined(__APPLE__)
#include "SessionRecorderIOSOwner.h"
#endif
#include <jsi/jsi.h>
#include <ReactCommon/CallInvoker.h>
#include <memory>
#include <cmath>
#include <limits>

namespace lomtree
{
    using namespace facebook::react;
    using namespace facebook::jsi;

    class SessionRecorderTreeHostObject final : public HostObject
    {
    private:
        std::shared_ptr<SessionRecorderObserver> observer_;

        std::weak_ptr<CallInvoker> jsInvoker_;

        std::shared_ptr<std::atomic<bool>> isAlive_;
#if defined(__APPLE__)
        std::shared_ptr<SessionRecorderIOSOwner> iosOwner_;
#endif

    public:
        SessionRecorderTreeHostObject(
            std::shared_ptr<SessionRecorderObserver> observer,
            std::weak_ptr<CallInvoker> jsInvoker,
            std::shared_ptr<std::atomic<bool>> isAlive
#if defined(__APPLE__)
            , std::shared_ptr<SessionRecorderIOSOwner> iosOwner
#endif
            )
            : observer_(std::move(observer)), jsInvoker_(std::move(jsInvoker)), isAlive_(std::move(isAlive))
#if defined(__APPLE__)
            , iosOwner_(std::move(iosOwner))
#endif
        {
        }

        void invalidateEarly()
        {
            if (isAlive_ != nullptr)
                *isAlive_ = false;
        }

        ~SessionRecorderTreeHostObject()
        {
            invalidateEarly();
        }

        Value get(Runtime &rt, const PropNameID &nameId) override
        {
            const auto nameUTF = nameId.utf8(rt);

#if defined(__APPLE__)
            if (nameUTF == "withActiveTouch")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, nameUTF.c_str()), 7,
                    [this](Runtime &runtime, const Value &, const Value *args, size_t count) -> Value
                    {
                        if (count != 7 || !isAlive_ || !isAlive_->load() || !observer_ || !iosOwner_ ||
                            !args[0].isString() || !args[1].isNumber() || !args[2].isNumber() ||
                            !args[3].isString() || !args[4].isNumber() || !args[5].isBool() ||
                            !args[6].isObject() || !args[6].asObject(runtime).isFunction(runtime)) return Value(false);
                        const auto activation = args[0].asString(runtime).utf8(runtime);
                        const double pointer = args[1].asNumber();
                        const double surface = args[2].asNumber();
                        const auto instance = args[3].asString(runtime).utf8(runtime);
                        const double generation = args[4].asNumber();
                        const bool awaitingLom = args[5].getBool();
                        for (double value : {pointer, surface, generation})
                            if (!std::isfinite(value) || value < 0 || std::floor(value) != value ||
                                value > 9007199254740991.0) return Value(false);
                        if (surface > std::numeric_limits<SurfaceId>::max()) return Value(false);
                        if (!observer_->isCaptureGenerationCurrent(static_cast<uint64_t>(generation)) ||
                            !observer_->isSurfaceInstanceCurrent(static_cast<SurfaceId>(surface), instance)) return Value(false);
                        auto perform = args[6].asObject(runtime).asFunction(runtime);
                        return Value(iosOwner_->withActiveTouch(activation, static_cast<uint64_t>(pointer),
                            static_cast<SurfaceId>(surface), instance, static_cast<uint64_t>(generation), awaitingLom, [&] {
                                // Internal callback only: arm/retire the candidate.
                                // Native contact retirement is excluded until this returns.
                                perform.call(runtime);
                            }));
                    });
            }
#endif

            // Both entrypoints request asynchronous capture through the same worker
            // and snapshot callback ({ signature, roots }); scan no longer returns an array.
            if (nameUTF == "startObserver" || nameUTF == "scan")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "startObserver"), 1,
                    [this](Runtime &rt, const Value &, const Value *args, size_t count) -> Value
                    {
                        // Shield against JS calls while the app is reloading
                        if (isAlive_ == nullptr || !(*isAlive_))
                        {
                            return Value(false);
                        }

                        auto binding = UIManagerBinding::getBinding(rt);
                        if (binding && observer_)
                        {
                            const bool requestInitialCapture = count == 0 || !args[0].isBool() || args[0].getBool();
#if defined(__APPLE__)
                            return Value(iosOwner_ && iosOwner_->startObserver(
                                binding->getUIManager(), observer_, requestInitialCapture));
#else
                            observer_->setPaused(false);
                            const bool alreadyRegistered = observer_->isRegistered();
                            if (!alreadyRegistered)
                            {
                                binding->getUIManager().registerCommitHook(*observer_);
                            }
                            if (requestInitialCapture)
                            {
                                observer_->requestInitialCapture();
                            }
                            return Value(true);
#endif
                        }
                        return Value(false);
                    });
            }

            if (nameUTF == "setPaused")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "setPaused"), 1,
                    [this](Runtime &, const Value &, const Value *args, size_t count) -> Value
                    {
                        if (isAlive_ == nullptr || !(*isAlive_) || !observer_ || count < 1 || !args[0].isBool())
                        {
                            return Value::undefined();
                        }
                        return Value(static_cast<double>(observer_->setPaused(args[0].getBool())));
                    });
            }

            if (nameUTF == "getActiveSurfaces")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "getActiveSurfaces"), 0,
                    [this](Runtime &runtime, const Value &, const Value *, size_t) -> Value
                    {
                        if (!isAlive_ || !(*isAlive_) || !observer_)
                            return Value::null();
                        const auto surfaces = observer_->getActiveSurfaces();
                        if (!surfaces)
                            return Value::null();
                        Array result(runtime, surfaces->size());
                        for (size_t i = 0; i < surfaces->size(); ++i)
                        {
                            Object entry(runtime);
                            entry.setProperty(runtime, "surfaceId", Value((double)(*surfaces)[i].surfaceId));
                            entry.setProperty(runtime, "surfaceInstance", String::createFromUtf8(runtime, (*surfaces)[i].surfaceInstance));
                            result.setValueAtIndex(runtime, i, std::move(entry));
                        }
                        return Value(std::move(result));
                    });
            }

            if (nameUTF == "isSurfaceInstanceCurrent")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "isSurfaceInstanceCurrent"), 2,
                    [this](Runtime &runtime, const Value &, const Value *args, size_t count) -> Value
                    {
                        if (!isAlive_ || !(*isAlive_) || !observer_ || count < 2 ||
                            !args[0].isNumber() || !args[1].isString())
                            return Value(false);
                        return Value(observer_->isSurfaceInstanceCurrent(
                            static_cast<SurfaceId>(args[0].asNumber()), args[1].asString(runtime).utf8(runtime)));
                    });
            }

            if (nameUTF == "armInteractionConsequence")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "armInteractionConsequence"), 3,
                    [this](Runtime &runtime, const Value &, const Value *args, size_t count) -> Value
                    {
                        if (!isAlive_ || !(*isAlive_) || !observer_ || count < 3 ||
                            !args[0].isNumber() || !args[1].isString() || !args[2].isNumber())
                            return Value(false);
                        const auto generation = args[2].asNumber();
                        if (!(generation >= 0 && generation <= 9007199254740991.0) ||
                            static_cast<uint64_t>(generation) != generation)
                            return Value(false);
                        return Value(observer_->armInteractionConsequence(
                            static_cast<SurfaceId>(args[0].asNumber()), args[1].asString(runtime).utf8(runtime),
                            static_cast<uint64_t>(generation)));
                    });
            }

            if (nameUTF == "captureIfDirty")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "captureIfDirty"), 3,
                    [this](Runtime &runtime, const Value &, const Value *args, size_t count) -> Value
                    {
                        Object payload(runtime);
                        if (isAlive_ == nullptr || !(*isAlive_) || !observer_ ||
                            count < 2 || !args[0].isNumber() || !args[1].isString())
                        {
                            payload.setProperty(runtime, "status", String::createFromAscii(runtime, "unavailable"));
                            payload.setProperty(runtime, "priorityAttempted", Value(false));
                            return Value(runtime, payload);
                        }

                        uint64_t priorityDebtGeneration = 0;
                        if (count >= 3)
                        {
                            if (!args[2].isNumber())
                            {
                                payload.setProperty(runtime, "status", String::createFromAscii(runtime, "unavailable"));
                                payload.setProperty(runtime, "priorityAttempted", Value(false));
                                return Value(runtime, payload);
                            }
                            const auto generation = args[2].asNumber();
                            if (!(generation >= 0 && generation <= 9007199254740991.0) ||
                                static_cast<uint64_t>(generation) != generation)
                            {
                                payload.setProperty(runtime, "status", String::createFromAscii(runtime, "unavailable"));
                                payload.setProperty(runtime, "priorityAttempted", Value(false));
                                return Value(runtime, payload);
                            }
                            priorityDebtGeneration = static_cast<uint64_t>(generation);
                        }
                        const auto surfaceId = static_cast<SurfaceId>(args[0].asNumber());
                        auto capture = observer_->captureIfDirty(
                            surfaceId, args[1].asString(runtime).utf8(runtime), priorityDebtGeneration);
                        try
                        {
                            const char *status = capture.status == DirtyCaptureStatus::Clean
                                                     ? "clean"
                                                 : capture.status == DirtyCaptureStatus::Captured
                                                     ? "captured"
                                                     : "unavailable";
                            payload.setProperty(runtime, "status", String::createFromAscii(runtime, status));
                            payload.setProperty(runtime, "surfaceId", Value((double)capture.surfaceId));
                            payload.setProperty(runtime, "surfaceInstance", String::createFromUtf8(runtime, capture.surfaceInstance));
                            payload.setProperty(runtime, "latestRevision", Value((double)capture.latestRevision));
                            payload.setProperty(runtime, "materializedRevision", Value((double)capture.materializedRevision));
                            payload.setProperty(runtime, "observedGeneration", Value((double)capture.observedGeneration));
                            payload.setProperty(runtime, "captureGeneration", Value((double)capture.captureGeneration));
                            payload.setProperty(runtime, "debtGeneration", Value((double)capture.debtGeneration));
                            payload.setProperty(runtime, "priorityAttempted", Value(capture.priorityAttempted));
                            payload.setProperty(runtime, "dirty", Value(capture.dirty));

                            if (capture.status == DirtyCaptureStatus::Captured)
                            {
                                payload.setProperty(runtime, "roots", toObjectArray(runtime, capture.tree.roots));
                                payload.setProperty(runtime, "signature", String::createFromUtf8(runtime, std::to_string(capture.signature)));
                                payload.setProperty(runtime, "viewportWidth", Value((double)capture.tree.viewportWidth));
                                payload.setProperty(runtime, "viewportHeight", Value((double)capture.tree.viewportHeight));
                                payload.setProperty(runtime, "revisionNumber", Value((double)capture.tree.revisionNumber));
                                payload.setProperty(runtime, "pagerVersion", Value((double)capture.tree.pagerVersion));
#if defined(ANDROID) || defined(__APPLE__)
                                payload.setProperty(runtime, "visibilityGeneration", Value((double)capture.tree.visibilityGeneration));
#endif
                                observer_->stageDirtySnapshot(capture.surfaceId, capture.surfaceInstance, capture.captureGeneration,
                                                              capture.signature, std::move(capture.tree));
                            }
                        }
                        catch (...)
                        {
                            if (priorityDebtGeneration == 0)
                                throw;
                            payload.setProperty(runtime, "status", String::createFromAscii(runtime, "unavailable"));
                            payload.setProperty(runtime, "priorityAttempted", Value(capture.priorityAttempted));
                            payload.setProperty(runtime, "debtGeneration", Value((double)capture.debtGeneration));
                        }
                        return Value(runtime, payload);
                    });
            }

            if (nameUTF == "acceptMaterializedRevision")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "acceptMaterializedRevision"),
#if defined(ANDROID) || defined(__APPLE__)
                    6,
#else
                    5,
#endif
                    [this](Runtime &runtime, const Value &, const Value *args, size_t count) -> Value
                    {
                        if (isAlive_ == nullptr || !(*isAlive_) || !observer_ || count < 5 ||
                            !args[0].isNumber() || !args[1].isNumber() || !args[2].isNumber() || !args[3].isString() || !args[4].isNumber())
                        {
                            return Value(false);
                        }
#if defined(ANDROID) || defined(__APPLE__)
                        if (count < 6 || !args[5].isNumber() || !std::isfinite(args[5].asNumber()) ||
                            args[5].asNumber() < 0 || args[5].asNumber() > 9007199254740991.0 ||
                            std::trunc(args[5].asNumber()) != args[5].asNumber()) return Value(false);
#endif
                        return Value(observer_->acceptMaterializedRevision(
                            static_cast<SurfaceId>(args[0].asNumber()),
                            static_cast<int64_t>(args[1].asNumber()),
                            static_cast<uint64_t>(args[2].asNumber()),
                            args[3].asString(runtime).utf8(runtime), static_cast<uint64_t>(args[4].asNumber())
#if defined(ANDROID) || defined(__APPLE__)
                            , static_cast<uint64_t>(args[5].asNumber())
#endif
                        ));
                    });
            }

            if (nameUTF == "stopObserver")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "stopObserver"), 0,
                    [this](Runtime &, const Value &, const Value *, size_t) -> Value
                    {
                        if (isAlive_ == nullptr || !(*isAlive_) || !observer_)
                        {
                            return Value(false);
                        }

                        observer_->clearSnapshotCallback();
                        observer_->clearCaptureResultCallback();
                        observer_->resetUIManager();
                        return Value(true);
                    });
            }

            if (nameUTF == "setScrollSuppressed")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "setScrollSuppressed"), 2,
                    [this](Runtime &, const Value &, const Value *args, size_t count) -> Value
                    {
                        if (isAlive_ == nullptr || !(*isAlive_) || !observer_ || count < 1 || !args[0].isBool())
                        {
                            return Value(false);
                        }
                        const bool restorePending = count >= 2 && args[1].isBool() && args[1].getBool();
                        observer_->setScrollSuppressed(args[0].getBool(), restorePending);
                        return Value(true);
                    });
            }

            if (nameUTF == "scanPostScroll")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "scanPostScroll"), 2,
                    [this](Runtime &, const Value &, const Value *args, size_t count) -> Value
                    {
                        if (isAlive_ == nullptr ||
                            !(*isAlive_) ||
                            !observer_ ||
                            count < 2 ||
                            !args[0].isNumber() ||
                            !args[1].isNumber())
                        {
                            return Value(false);
                        }
                        const auto requestId = static_cast<uint64_t>(args[0].asNumber());
                        const auto debtGeneration = static_cast<uint64_t>(args[1].asNumber());
                        return Value(observer_->requestExplicitCapture(requestId, debtGeneration));
                    });
            }

            if (nameUTF == "setPostScrollDebtGeneration")
            {
                return Function::createFromHostFunction(
                    rt, PropNameID::forAscii(rt, "setPostScrollDebtGeneration"), 1,
                    [this](Runtime &, const Value &, const Value *args, size_t count) -> Value
                    {
                        if (isAlive_ == nullptr ||
                            !(*isAlive_) ||
                            !observer_ ||
                            count < 1 ||
                            !args[0].isNumber())
                        {
                            return Value(false);
                        }
                        observer_->setPostScrollDebtGeneration(
                            static_cast<uint64_t>(args[0].asNumber()));
                        return Value(true);
                    });
            }

            return Value::undefined();
        }

        static Value toObjectArray(Runtime &rt, const std::vector<Root> &roots)
        {
            size_t rootsLength = roots.size();
            auto array = Array(rt, rootsLength);
            for (size_t i = 0; i < rootsLength; i++)
            {
                const auto &root = roots[i];
                auto object = Object(rt);
                object.setProperty(rt, "id", Value((double)root.tag));
                object.setProperty(rt, "parentId", root.parentTag >= 0 ? Value((double)root.parentTag) : Value::null());
                object.setProperty(rt, "depth", Value((double)root.depth));
                object.setProperty(rt, "type", String::createFromUtf8(rt, root.name));
                object.setProperty(rt, "x", Value((double)root.x));
                object.setProperty(rt, "y", Value((double)root.y));
                object.setProperty(rt, "width", Value((double)root.width));
                object.setProperty(rt, "height", Value((double)root.height));

                array.setValueAtIndex(rt, i, std::move(object));
            }

            return array;
        }
    };

}
