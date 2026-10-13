#pragma once

#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <array>
#include <cstdint>
#include <cstddef>

namespace praccy::plugins {

class FastParamValueQueue : public Steinberg::Vst::IParamValueQueue {
public:
    FastParamValueQueue() = default;

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
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IParamValueQueue::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IParamValueQueue*>(this);
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1000; }
    Steinberg::uint32 PLUGIN_API release() override { return 1000; }

private:
    Steinberg::Vst::ParamID m_id{0};
    double m_value{0.0};
};

class FastParameterChanges : public Steinberg::Vst::IParameterChanges {
public:
    static constexpr size_t kMaxQueuedParams = 128;

    FastParameterChanges() = default;

    void clear() noexcept { m_count = 0; }

    bool addParameterChange(Steinberg::Vst::ParamID id, double value) noexcept {
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
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IParameterChanges::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid)) {
            *obj = static_cast<Steinberg::Vst::IParameterChanges*>(this);
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1000; }
    Steinberg::uint32 PLUGIN_API release() override { return 1000; }

private:
    std::array<FastParamValueQueue, kMaxQueuedParams> m_queues;
    size_t m_count{0};
};

} // namespace praccy::plugins
