#pragma once

#include "../audio/audio_node.h"
#include "crash_isolation.h"
#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <mutex>
#if defined(_WIN32)
#include <windows.h>
#else
using HWND = void*;
using HMODULE = void*;
using UINT_PTR = uintptr_t;
#endif

namespace praccy::plugins {

/// Process-wide mutex to serialize LoadLibrary/FreeLibrary calls across threads,
/// preventing copy-protection (PACE / Eden / iLok) driver deadlocks.
inline std::recursive_mutex& pluginDllMutex() {
    static std::recursive_mutex s_mutex;
    return s_mutex;
}

struct FaultState {
    std::atomic<bool> faulted{false};
    std::atomic<uint32_t> exceptionCode{0};
    char reason[128]{};
};

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
    virtual void openGuiAsync(HWND parentHwnd, std::function<void(bool success)> onComplete) {
        bool ok = openGui(parentHwnd);
        if (onComplete) onComplete(ok);
    }
    virtual void pollAsyncGuiResults() {}
    virtual void closeGui() = 0;
    virtual void getPreferredSize(int& width, int& height) const {
        width = 850;
        height = 600;
    }
    virtual void onGuiResize(int /*width*/, int /*height*/) {}
    virtual bool adjustSize(int& /*width*/, int& /*height*/) { return false; }
    [[nodiscard]] virtual float getParameterValueByIndex(size_t index) const {
        return getParameterDesc(index).currentValue;
    }
    virtual void getAllParameterValues(std::vector<float>& out) const {
        const size_t n = numParameters();
        out.resize(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = getParameterValueByIndex(i);
        }
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

    [[nodiscard]] static uint64_t computeStateHash(const std::vector<uint8_t>& data) noexcept {
        if (data.empty()) return 0ULL;
        uint64_t hash = 14695981039346656037ULL;
        for (uint8_t b : data) {
            hash ^= static_cast<uint64_t>(b);
            hash *= 1099511628211ULL;
        }
        return hash == 0ULL ? 1ULL : hash;
    }

    [[nodiscard]] uint64_t lastStateHash() const noexcept {
        return m_lastStateHash.load(std::memory_order_acquire);
    }

    void updateStateHash(const std::vector<uint8_t>& state) const noexcept {
        m_lastStateHash.store(computeStateHash(state), std::memory_order_release);
    }

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
    [[nodiscard]] virtual uint32_t faultCode() const noexcept {
        return m_faultCode.load(std::memory_order_acquire);
    }
    virtual void setFaultCode(uint32_t code) noexcept {
        m_faultCode.store(code, std::memory_order_release);
    }
    [[nodiscard]] virtual const char* faultReason() const noexcept {
        const char* r = m_faultReason.load(std::memory_order_relaxed);
        if (r && r[0] != '\0') return r;
        if (m_faultReasonBuf[0] != '\0') return m_faultReasonBuf;
        return "Unknown fault";
    }
    virtual void setFaultReason(const char* reason) noexcept {
        m_faultReason.store(reason, std::memory_order_release);
    }
    virtual void setFaultReasonFormatted(const char* prefix, uint32_t code) noexcept {
        m_faultCode.store(code, std::memory_order_release);
        formatFaultReason(m_faultReasonBuf, prefix, code);
        m_faultReason.store(m_faultReasonBuf, std::memory_order_release);
    }
    virtual void resetFault() noexcept {
        m_faulted.store(false, std::memory_order_release);
        m_faultCode.store(0, std::memory_order_release);
        m_faultReason.store(nullptr, std::memory_order_release);
        m_faultReasonBuf[0] = '\0';
    }
    virtual void handleMainThreadCallbacks() noexcept {}

    char* faultReasonBuffer() noexcept { return m_faultReasonBuf; }
    const char* faultReasonBuffer() const noexcept { return m_faultReasonBuf; }

protected:
    std::atomic<bool> m_faulted{false};
    std::atomic<uint32_t> m_faultCode{0};
    std::atomic<const char*> m_faultReason{nullptr};
    char m_faultReasonBuf[128]{};
    std::atomic<bool> m_guiParamDirty{false};
    std::atomic<bool> m_guiGestureEnded{false};
    std::atomic<bool> m_guiEditingActive{false};
    std::atomic<uint32_t> m_lastGuiEditedParamId{0};
    mutable std::atomic<uint64_t> m_lastStateHash{0};
};

} // namespace praccy::plugins
