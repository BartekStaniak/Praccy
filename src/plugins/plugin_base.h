#pragma once

#include "../audio/audio_node.h"
#include <string>
#include <vector>
#include <atomic>
#include <windows.h>

namespace praccy::plugins {

struct PluginParameterDesc {
    uint32_t id{0};
    std::string name;
    float minValue{0.0f};
    float maxValue{1.0f};
    float defaultValue{0.0f};
    float currentValue{0.0f};
};

class IPluginInstance : public audio::AudioNode {
public:
    virtual ~IPluginInstance() = default;

    [[nodiscard]] virtual const std::string& vendor() const noexcept = 0;
    [[nodiscard]] virtual const std::string& version() const noexcept = 0;

    [[nodiscard]] virtual size_t numParameters() const noexcept = 0;
    [[nodiscard]] virtual PluginParameterDesc getParameterDesc(size_t index) const = 0;
    virtual void setParameterValue(uint32_t paramId, float value) = 0;
    virtual void setParameterFromAudioThread(uint32_t paramId, float value) noexcept {
        setParameterValue(paramId, value);
    }
    [[nodiscard]] virtual float getParameterValue(uint32_t paramId) const = 0;

    [[nodiscard]] virtual bool hasCustomGui() const noexcept = 0;
    virtual bool openGui(HWND parentHwnd) = 0;
    virtual void closeGui() = 0;
    virtual void getPreferredSize(int& width, int& height) const {
        width = 850;
        height = 600;
    }

    [[nodiscard]] virtual const std::string& path() const noexcept {
        static const std::string s_empty;
        return s_empty;
    }

    [[nodiscard]] virtual const std::string& bundleFileName() const noexcept {
        static const std::string s_empty;
        return s_empty;
    }

    [[nodiscard]] virtual const std::string& classGuid() const noexcept {
        static const std::string s_empty;
        return s_empty;
    }

    [[nodiscard]] virtual const std::string& clapId() const noexcept {
        static const std::string s_empty;
        return s_empty;
    }

    [[nodiscard]] virtual std::vector<uint8_t> saveState() const = 0;
    virtual bool loadState(const std::vector<uint8_t>& state) = 0;

    // GUI parameter edit notifications (wait-free, lock-free)
    void notifyGuiParamEdited(uint32_t paramId, bool gestureEnded = false) noexcept {
        m_lastGuiEditedParamId.store(paramId, std::memory_order_relaxed);
        if (gestureEnded) {
            m_guiGestureEnded.store(true, std::memory_order_release);
        }
        m_guiParamDirty.store(true, std::memory_order_release);
    }

    void notifyGuiGestureEnded(uint32_t paramId) noexcept {
        m_lastGuiEditedParamId.store(paramId, std::memory_order_relaxed);
        m_guiGestureEnded.store(true, std::memory_order_release);
        m_guiParamDirty.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool consumeGuiParamEdit(uint32_t& outParamId, bool& outGestureEnded) noexcept {
        const bool dirty = m_guiParamDirty.exchange(false, std::memory_order_acq_rel);
        const bool ended = m_guiGestureEnded.exchange(false, std::memory_order_acq_rel);
        if (!dirty && !ended) {
            outGestureEnded = false;
            return false;
        }
        outParamId = m_lastGuiEditedParamId.load(std::memory_order_relaxed);
        outGestureEnded = ended;
        return true;
    }

    void clearGuiParamEdit() noexcept {
        m_guiParamDirty.store(false, std::memory_order_release);
        m_guiGestureEnded.store(false, std::memory_order_release);
        m_guiEditingActive.store(false, std::memory_order_release);
    }

    void setGuiEditingActive(bool active) noexcept {
        m_guiEditingActive.store(active, std::memory_order_release);
    }

    [[nodiscard]] bool isGuiEditingActive() const noexcept {
        return m_guiEditingActive.load(std::memory_order_acquire);
    }

    // Fault state tracking
    [[nodiscard]] virtual bool isFaulted() const noexcept {
        return m_faulted.load(std::memory_order_acquire);
    }
    virtual void setFaulted(bool faulted) noexcept {
        m_faulted.store(faulted, std::memory_order_release);
    }
    [[nodiscard]] virtual const char* faultReason() const noexcept {
        const char* r = m_faultReason.load(std::memory_order_relaxed);
        return r ? r : "Unknown fault";
    }
    virtual void setFaultReason(const char* reason) noexcept {
        m_faultReason.store(reason, std::memory_order_release);
    }
    virtual void resetFault() noexcept {
        m_faulted.store(false, std::memory_order_release);
        m_faultReason.store(nullptr, std::memory_order_release);
    }

protected:
    std::atomic<bool> m_faulted{false};
    std::atomic<const char*> m_faultReason{nullptr};
    std::atomic<bool> m_guiParamDirty{false};
    std::atomic<bool> m_guiGestureEnded{false};
    std::atomic<bool> m_guiEditingActive{false};
    std::atomic<uint32_t> m_lastGuiEditedParamId{0};
};

} // namespace praccy::plugins
