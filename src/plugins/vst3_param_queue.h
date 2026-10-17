#pragma once

#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace praccy::plugins {

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

class FastParamValueQueue : public Steinberg::Vst::IParamValueQueue {
public:
    FastParamValueQueue() = default;
    virtual ~FastParamValueQueue() = default;

    void init(Steinberg::Vst::ParamID id, double value) noexcept {
        m_id = id;
        m_value = value;
    }

    Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return m_id; }
    Steinberg::int32 PLUGIN_API getPointCount() override { return 1; }

    Steinberg::tresult PLUGIN_API getPoint(Steinberg::int32 index, Steinberg::int32& sampleOffset, double& value) override {
        if (index != 0) return Steinberg::kResultFalse;
        sampleOffset = 0;
        value = m_value;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API addPoint(Steinberg::int32 /*sampleOffset*/, double /*value*/, Steinberg::int32& /*index*/) override {
        return Steinberg::kResultFalse;
    }

    // Steinberg COM refcounting
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID queryIid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(queryIid, Steinberg::Vst::IParamValueQueue::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(queryIid, Steinberg::FUnknown::iid)) {
            addRef();
            *obj = static_cast<Steinberg::Vst::IParamValueQueue*>(this);
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    Steinberg::uint32 PLUGIN_API release() override {
        const uint32_t prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
        return (prev > 0) ? (prev - 1) : 0;
    }

private:
    Steinberg::Vst::ParamID m_id{0};
    double m_value{0.0};
    std::atomic<uint32_t> m_refCount{1};
};

class FastParameterChanges : public Steinberg::Vst::IParameterChanges {
public:
    static constexpr size_t kMaxQueuedParams = 128;

    FastParameterChanges() = default;
    virtual ~FastParameterChanges() = default;

    void clear() noexcept { m_count = 0; }

    bool addParameterChange(Steinberg::Vst::ParamID id, double value) noexcept {
        for (size_t i = 0; i < m_count; ++i) {
            if (m_queues[i].getParameterId() == id) {
                m_queues[i].init(id, value);
                return true;
            }
        }
        if (m_count >= kMaxQueuedParams) return false;
        m_queues[m_count].init(id, value);
        m_count++;
        return true;
    }

    Steinberg::int32 PLUGIN_API getParameterCount() override {
        return static_cast<Steinberg::int32>(m_count);
    }

    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData(Steinberg::int32 index) override {
        if (index < 0 || index >= static_cast<Steinberg::int32>(m_count)) return nullptr;
        return &m_queues[index];
    }

    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData(const Steinberg::Vst::ParamID& id, Steinberg::int32& index) override {
        if (m_count >= kMaxQueuedParams) return nullptr;
        index = static_cast<Steinberg::int32>(m_count);
        m_queues[m_count].init(id, 0.0);
        m_count++;
        return &m_queues[index];
    }

    // Steinberg COM refcounting
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID queryIid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(queryIid, Steinberg::Vst::IParameterChanges::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(queryIid, Steinberg::FUnknown::iid)) {
            addRef();
            *obj = static_cast<Steinberg::Vst::IParameterChanges*>(this);
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    Steinberg::uint32 PLUGIN_API release() override {
        const uint32_t prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
        return (prev > 0) ? (prev - 1) : 0;
    }

private:
    std::array<FastParamValueQueue, kMaxQueuedParams> m_queues;
    size_t m_count{0};
    std::atomic<uint32_t> m_refCount{1};
};

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace praccy::plugins
