#pragma once

#include "plugin_base.h"
#include <clap/clap.h>
#include <clap/events.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>
#include <clap/ext/gui.h>
#include <clap/ext/latency.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/thread-check.h>
#include <clap/ext/timer-support.h>
#include <clap/ext/log.h>
#include <memory>
#include <vector>
#include <string>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <readerwriterqueue/readerwriterqueue.h>

namespace praccy::plugins {

struct ClapEventBuffer {
    static constexpr size_t kMaxEvents = 128;
    std::array<clap_event_param_value_t, kMaxEvents> events{};
    uint32_t count{0};

    void clear() noexcept { count = 0; }

    bool pushParam(clap_id paramId, double val) noexcept {
        if (count >= kMaxEvents) return false;
        auto& ev = events[count++];
        ev.header.size = sizeof(clap_event_param_value_t);
        ev.header.time = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags = 0;
        ev.param_id = paramId;
        ev.cookie = nullptr;
        ev.note_id = -1;
        ev.port_index = -1;
        ev.channel = -1;
        ev.key = -1;
        ev.value = val;
        return true;
    }

    static uint32_t CLAP_ABI sizeCallback(const clap_input_events_t* list) {
        auto* self = static_cast<const ClapEventBuffer*>(list->ctx);
        return self ? self->count : 0;
    }

    static const clap_event_header_t* CLAP_ABI getCallback(const clap_input_events_t* list, uint32_t index) {
        auto* self = static_cast<const ClapEventBuffer*>(list->ctx);
        if (!self || index >= self->count) return nullptr;
        return &self->events[index].header;
    }

    clap_input_events_t makeInputEvents() noexcept {
        clap_input_events_t inEvs{};
        inEvs.ctx = this;
        inEvs.size = &sizeCallback;
        inEvs.get = &getCallback;
        return inEvs;
    }
};

class ClapPluginInstance : public IPluginInstance {
public:
    static std::unique_ptr<ClapPluginInstance> loadFromFile(const std::string& path, uint32_t pluginIndex = 0);
    static void setMainThreadId(DWORD tid) noexcept;
    [[nodiscard]] static DWORD mainThreadId() noexcept;

    ~ClapPluginInstance() override;

    void prepare(double sampleRate, uint32_t maxBlockSize) override;
    void process(audio::AudioProcessContext& ctx) override;
    void reset() override;

    [[nodiscard]] audio::NodeType type() const noexcept override { return audio::NodeType::Plugin; }
    [[nodiscard]] const std::string& name() const noexcept override { return m_name; }
    [[nodiscard]] const std::string& vendor() const noexcept override { return m_vendor; }
    [[nodiscard]] const std::string& version() const noexcept override { return m_version; }
    [[nodiscard]] const std::string& path() const noexcept override { return m_path; }
    [[nodiscard]] const std::string& bundleFileName() const noexcept override { return m_bundleFileName; }
    [[nodiscard]] const std::string& clapId() const noexcept override { return m_clapId; }

    [[nodiscard]] size_t numParameters() const noexcept override;
    [[nodiscard]] PluginParameterDesc getParameterDesc(size_t index) const override;
    void setParameterValue(uint32_t paramId, float value) override;
    void setParameterFromAudioThread(uint32_t paramId, float value) noexcept override;
    [[nodiscard]] float getParameterValue(uint32_t paramId) const override;

    [[nodiscard]] bool hasCustomGui() const noexcept override;
    bool openGui(HWND parentHwnd) override;
    void openGuiAsync(HWND parentHwnd, std::function<void(bool success)> onComplete) override;
    void pollAsyncGuiResults() override;
    void closeGui() override;
    void getPreferredSize(int& width, int& height) const override;
    void onGuiResize(int width, int height) override;
    bool adjustSize(int& width, int& height) override;
    [[nodiscard]] float getParameterValueByIndex(size_t index) const override;
    void getAllParameterValues(std::vector<float>& out) const override;

    [[nodiscard]] std::vector<uint8_t> saveState() const override;
    bool loadState(const std::vector<uint8_t>& state) override;

    ClapPluginInstance();

    void onPluginOutputEvent(const clap_event_header_t* event) noexcept;
    void resetFault() noexcept override;
    void handleMainThreadCallbacks() noexcept override;
    void fireTimer(clap_id timerId);
    void rescanParamValues() noexcept;

private:
    static const void* clapHostGetExtension(const clap_host* host, const char* extensionId);
    static void clapHostRequestRestart(const clap_host* host);
    static void clapHostRequestProcess(const clap_host* host);
    static void clapHostRequestCallback(const clap_host* host);

    static void CLAP_ABI clapHostParamsRescan(const clap_host_t* host, clap_param_rescan_flags flags);
    static void CLAP_ABI clapHostParamsClear(const clap_host_t* host, clap_id paramId, clap_param_clear_flags flags);
    static void CLAP_ABI clapHostParamsRequestFlush(const clap_host_t* host);
    static void CLAP_ABI clapHostStateMarkDirty(const clap_host_t* host);

    // Thread check extension
    static bool CLAP_ABI clapHostThreadCheckIsMainThread(const clap_host_t* host);
    static bool CLAP_ABI clapHostThreadCheckIsAudioThread(const clap_host_t* host);

    // Audio ports extension
    static bool CLAP_ABI clapHostAudioPortsIsRescanFlagSupported(const clap_host_t* host, uint32_t flag);
    static void CLAP_ABI clapHostAudioPortsRescan(const clap_host_t* host, uint32_t flags);

    // Latency extension
    static void CLAP_ABI clapHostLatencyChanged(const clap_host_t* host);

    // Log extension
    static void CLAP_ABI clapHostLog(const clap_host_t* host, clap_log_severity severity, const char* msg);

    // GUI extension
    static void CLAP_ABI clapHostGuiResizeHintsChanged(const clap_host_t* host);
    static bool CLAP_ABI clapHostGuiRequestResize(const clap_host_t* host, uint32_t width, uint32_t height);
    static bool CLAP_ABI clapHostGuiRequestShow(const clap_host_t* host);
    static bool CLAP_ABI clapHostGuiRequestHide(const clap_host_t* host);
    static void CLAP_ABI clapHostGuiClosed(const clap_host_t* host, bool was_destroyed);

    // Timer support extension
    static bool CLAP_ABI clapHostRegisterTimer(const clap_host_t* host, uint32_t period_ms, clap_id* timer_id);
    static bool CLAP_ABI clapHostUnregisterTimer(const clap_host_t* host, clap_id timer_id);

    void suspendAndFenceAudio() noexcept;
    void resumeAudio() noexcept;
    void flushPendingParamsIfStopped();
    void drainGuiOutParamQueue() const;

    HMODULE m_libraryModule{nullptr};
    const clap_plugin_entry* m_entry{nullptr};
    const clap_plugin* m_plugin{nullptr};
    clap_host m_host{};

    const clap_plugin_gui* m_guiExt{nullptr};
    const clap_plugin_params* m_paramsExt{nullptr};
    const clap_plugin_state* m_stateExt{nullptr};
    const clap_plugin_latency* m_latencyExt{nullptr};
    const clap_plugin_audio_ports* m_audioPortsExt{nullptr};
    const clap_plugin_timer_support* m_timerExt{nullptr};

    std::string m_path;
    std::string m_bundleFileName;
    std::string m_clapId;
    std::string m_name{"CLAP Plugin"};
    std::string m_vendor{"Unknown"};
    std::string m_version{"1.0.0"};

    HWND m_guiParentHwnd{nullptr};
    bool m_guiCreated{false};
    bool m_hasPendingAsyncGui{false};
    HWND m_pendingAsyncGuiHwnd{nullptr};
    std::function<void(bool success)> m_pendingAsyncGuiCallback;

    DWORD m_mainThreadId{0};
    double m_sampleRate{48000.0};
    uint32_t m_maxBlockSize{256};
    uint32_t m_numAudioInputs{1};
    uint32_t m_numAudioOutputs{1};
    std::atomic<bool> m_callbackRequested{false};
    std::atomic<bool> m_restartRequested{false};
    std::unordered_map<clap_id, UINT_PTR> m_activeTimers;
    std::shared_ptr<std::atomic<ClapPluginInstance*>> m_timerLifetimeToken;

    moodycamel::ReaderWriterQueue<std::pair<clap_id, double>> m_paramQueue{128};
    mutable moodycamel::ReaderWriterQueue<std::pair<clap_id, double>> m_guiOutParamQueue{128};

    struct ClapAudioParamEvent {
        clap_id id{0};
        double value{0.0};
    };
    std::array<ClapAudioParamEvent, 256> m_audioThreadEvents{};
    size_t m_audioThreadEventCount{0};

    std::array<float*, 16> m_inChannelPtrs{};
    std::array<float*, 16> m_outChannelPtrs{};
    clap_event_transport_t m_transportEvent{};

    ClapEventBuffer m_eventBuffer;
    bool m_isActivated{false};
    std::atomic<bool> m_isProcessing{false};
    std::atomic<bool> m_suspendProcessing{false};
    std::atomic<uint32_t> m_activeProcessCallbacks{0};
    std::atomic<DWORD> m_audioThreadId{0};
    std::atomic<uint64_t> m_paramPushGen{0};
    std::atomic<uint64_t> m_paramDrainGen{0};

    mutable std::mutex m_paramCacheMutex;
    mutable std::unordered_map<uint32_t, float> m_paramCache;
    std::atomic<bool> m_inResize{false};
    int m_preferredWidth{850};
    int m_preferredHeight{600};
};

} // namespace praccy::plugins
