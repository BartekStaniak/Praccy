#include "clap_host.h"
#include "crash_isolation.h"
#include <iostream>
#include <sstream>
#include <filesystem>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace praccy::plugins {

namespace {

std::atomic<DWORD> s_mainThreadId{0};

bool CLAP_ABI clapOutputEventsTryPush(const clap_output_events_t* list, const clap_event_header_t* event) {
    if (!list || !list->ctx || !event) return false;
    auto* self = static_cast<ClapPluginInstance*>(list->ctx);
    self->onPluginOutputEvent(event);
    return true;
}

struct Win32TimerRecord {
    std::shared_ptr<std::atomic<ClapPluginInstance*>> lifetimeToken;
    clap_id clapTimerId{0};
    UINT_PTR timerId{0};
};

std::mutex s_timerMutex;
std::unordered_map<UINT_PTR, Win32TimerRecord> s_win32Timers;
clap_id s_nextClapTimerId = 1;

VOID CALLBACK clapTimerCallback(HWND /*hwnd*/, UINT /*uMsg*/, UINT_PTR idEvent, DWORD /*dwTime*/) {
    std::shared_ptr<std::atomic<ClapPluginInstance*>> token;
    clap_id cTimerId = 0;
    {
        std::lock_guard<std::mutex> lock(s_timerMutex);
        auto it = s_win32Timers.find(idEvent);
        if (it != s_win32Timers.end()) {
            token = it->second.lifetimeToken;
            cTimerId = it->second.clapTimerId;
        }
    }
    if (token) {
        if (auto* inst = token->load(std::memory_order_acquire)) {
            inst->fireTimer(cTimerId);
        }
    }
}

} // anonymous namespace

ClapPluginInstance::ClapPluginInstance()
    : m_mainThreadId(mainThreadId()),
      m_timerLifetimeToken(std::make_shared<std::atomic<ClapPluginInstance*>>(this)) {}

void ClapPluginInstance::setMainThreadId(DWORD tid) noexcept {
    s_mainThreadId.store(tid, std::memory_order_release);
}

DWORD ClapPluginInstance::mainThreadId() noexcept {
    DWORD tid = s_mainThreadId.load(std::memory_order_acquire);
    if (tid == 0) {
        tid = GetCurrentThreadId();
        DWORD expected = 0;
        if (!s_mainThreadId.compare_exchange_strong(expected, tid, std::memory_order_acq_rel)) {
            tid = expected;
        }
    }
    return tid;
}

void ClapPluginInstance::suspendAndFenceAudio() noexcept {
    m_suspendProcessing.store(true, std::memory_order_seq_cst);
    if (GetCurrentThreadId() != m_audioThreadId.load(std::memory_order_relaxed)) {
        while (m_activeProcessCallbacks.load(std::memory_order_acquire) != 0) {
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#endif
        }
    }
}

void ClapPluginInstance::resumeAudio() noexcept {
    m_suspendProcessing.store(false, std::memory_order_release);
}

const void* ClapPluginInstance::clapHostGetExtension(const clap_host* /*host*/, const char* extensionId) {
    if (!extensionId) return nullptr;
    if (std::strcmp(extensionId, CLAP_EXT_PARAMS) == 0) {
        static const clap_host_params s_hostParams{
            &ClapPluginInstance::clapHostParamsRescan,
            &ClapPluginInstance::clapHostParamsClear,
            &ClapPluginInstance::clapHostParamsRequestFlush
        };
        return &s_hostParams;
    }
    if (std::strcmp(extensionId, CLAP_EXT_STATE) == 0) {
        static const clap_host_state s_hostState{
            &ClapPluginInstance::clapHostStateMarkDirty
        };
        return &s_hostState;
    }
    if (std::strcmp(extensionId, CLAP_EXT_THREAD_CHECK) == 0) {
        static const clap_host_thread_check s_hostThreadCheck{
            &ClapPluginInstance::clapHostThreadCheckIsMainThread,
            &ClapPluginInstance::clapHostThreadCheckIsAudioThread
        };
        return &s_hostThreadCheck;
    }
    if (std::strcmp(extensionId, CLAP_EXT_AUDIO_PORTS) == 0) {
        static const clap_host_audio_ports s_hostAudioPorts{
            &ClapPluginInstance::clapHostAudioPortsIsRescanFlagSupported,
            &ClapPluginInstance::clapHostAudioPortsRescan
        };
        return &s_hostAudioPorts;
    }
    if (std::strcmp(extensionId, CLAP_EXT_LATENCY) == 0) {
        static const clap_host_latency s_hostLatency{
            &ClapPluginInstance::clapHostLatencyChanged
        };
        return &s_hostLatency;
    }
    if (std::strcmp(extensionId, CLAP_EXT_LOG) == 0) {
        static const clap_host_log s_hostLog{
            &ClapPluginInstance::clapHostLog
        };
        return &s_hostLog;
    }
    if (std::strcmp(extensionId, CLAP_EXT_GUI) == 0) {
        static const clap_host_gui s_hostGui{
            &ClapPluginInstance::clapHostGuiResizeHintsChanged,
            &ClapPluginInstance::clapHostGuiRequestResize,
            &ClapPluginInstance::clapHostGuiRequestShow,
            &ClapPluginInstance::clapHostGuiRequestHide,
            &ClapPluginInstance::clapHostGuiClosed
        };
        return &s_hostGui;
    }
    if (std::strcmp(extensionId, CLAP_EXT_TIMER_SUPPORT) == 0) {
        static const clap_host_timer_support s_hostTimer{
            &ClapPluginInstance::clapHostRegisterTimer,
            &ClapPluginInstance::clapHostUnregisterTimer
        };
        return &s_hostTimer;
    }
    return nullptr;
}

void ClapPluginInstance::clapHostRequestRestart(const clap_host* host) {
    if (!host || !host->host_data) return;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    self->m_restartRequested.store(true, std::memory_order_release);
    self->notifyGuiParamEdited(0xFFFFFFFFu, true);
}

void ClapPluginInstance::clapHostRequestProcess(const clap_host* /*host*/) {}

void ClapPluginInstance::clapHostRequestCallback(const clap_host* host) {
    if (!host || !host->host_data) return;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    if (GetCurrentThreadId() == self->m_mainThreadId) {
        if (self->m_plugin && self->m_plugin->on_main_thread) {
            safeCallPlugin([&]() {
                self->m_plugin->on_main_thread(self->m_plugin);
            });
        }
    } else {
        self->m_callbackRequested.store(true, std::memory_order_release);
    }
}

bool CLAP_ABI ClapPluginInstance::clapHostThreadCheckIsMainThread(const clap_host_t* host) {
    if (!host || !host->host_data) return false;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    return GetCurrentThreadId() == self->m_mainThreadId;
}

bool CLAP_ABI ClapPluginInstance::clapHostThreadCheckIsAudioThread(const clap_host_t* host) {
    if (!host || !host->host_data) return false;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    return GetCurrentThreadId() == self->m_audioThreadId.load(std::memory_order_relaxed);
}

bool CLAP_ABI ClapPluginInstance::clapHostAudioPortsIsRescanFlagSupported(const clap_host_t* /*host*/, uint32_t /*flag*/) {
    return false;
}

void CLAP_ABI ClapPluginInstance::clapHostAudioPortsRescan(const clap_host_t* /*host*/, uint32_t /*flags*/) {}

void CLAP_ABI ClapPluginInstance::clapHostLatencyChanged(const clap_host_t* host) {
    if (!host || !host->host_data) return;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    if (self->m_latencyExt && self->m_plugin && self->m_isActivated) {
        self->m_latencySamples = self->m_latencyExt->get(self->m_plugin);
    }
}

void CLAP_ABI ClapPluginInstance::clapHostLog(const clap_host_t* /*host*/, clap_log_severity severity, const char* msg) {
    if (severity >= CLAP_LOG_WARNING) {
        std::cerr << "[CLAP Log] " << (msg ? msg : "") << std::endl;
    }
}

void CLAP_ABI ClapPluginInstance::clapHostGuiResizeHintsChanged(const clap_host_t* /*host*/) {}

bool CLAP_ABI ClapPluginInstance::clapHostGuiRequestResize(const clap_host_t* host, uint32_t width, uint32_t height) {
    if (!host || !host->host_data) return false;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    if (self->m_guiParentHwnd && IsWindow(self->m_guiParentHwnd)) {
        RECT wr = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        AdjustWindowRectEx(&wr,
                           static_cast<DWORD>(GetWindowLongW(self->m_guiParentHwnd, GWL_STYLE)),
                           FALSE,
                           static_cast<DWORD>(GetWindowLongW(self->m_guiParentHwnd, GWL_EXSTYLE)));
        SetWindowPos(self->m_guiParentHwnd, nullptr, 0, 0, wr.right - wr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
        return true;
    }
    return false;
}

bool CLAP_ABI ClapPluginInstance::clapHostGuiRequestShow(const clap_host_t* /*host*/) { return true; }
bool CLAP_ABI ClapPluginInstance::clapHostGuiRequestHide(const clap_host_t* /*host*/) { return true; }
void CLAP_ABI ClapPluginInstance::clapHostGuiClosed(const clap_host_t* /*host*/, bool /*was_destroyed*/) {}

bool CLAP_ABI ClapPluginInstance::clapHostRegisterTimer(const clap_host_t* host, uint32_t period_ms, clap_id* timer_id) {
    if (!host || !host->host_data || !timer_id) return false;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    UINT period = std::max(10u, period_ms);
    UINT_PTR tid = SetTimer(nullptr, 0, period, &clapTimerCallback);
    if (!tid) return false;

    clap_id cId = 0;
    {
        std::lock_guard<std::mutex> lock(s_timerMutex);
        cId = s_nextClapTimerId++;
        s_win32Timers[tid] = Win32TimerRecord{self->m_timerLifetimeToken, cId, tid};
        self->m_activeTimers[cId] = tid;
    }
    *timer_id = cId;
    return true;
}

bool CLAP_ABI ClapPluginInstance::clapHostUnregisterTimer(const clap_host_t* host, clap_id timer_id) {
    if (!host || !host->host_data) return false;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    std::lock_guard<std::mutex> lock(s_timerMutex);
    auto it = self->m_activeTimers.find(timer_id);
    if (it != self->m_activeTimers.end()) {
        KillTimer(nullptr, it->second);
        s_win32Timers.erase(it->second);
        self->m_activeTimers.erase(it);
        return true;
    }
    return false;
}

void ClapPluginInstance::fireTimer(clap_id timerId) {
    if (m_timerExt && m_plugin && !m_faulted.load(std::memory_order_relaxed)) {
        safeCallPlugin([&]() {
            m_timerExt->on_timer(m_plugin, timerId);
        });
    }
}

void ClapPluginInstance::handleMainThreadCallbacks() noexcept {
    if (m_restartRequested.exchange(false, std::memory_order_acq_rel)) {
        if (m_plugin && m_isActivated) {
            suspendAndFenceAudio();
            safeCallPlugin([&]() {
                if (m_isProcessing.load(std::memory_order_acquire)) {
                    m_plugin->stop_processing(m_plugin);
                    m_isProcessing.store(false, std::memory_order_release);
                }
                m_plugin->deactivate(m_plugin);
                m_isActivated = false;
                if (m_sampleRate > 0.0 && m_maxBlockSize > 0) {
                    if (m_plugin->activate(m_plugin, m_sampleRate, 1, m_maxBlockSize)) {
                        m_isActivated = true;
                    }
                }
            });
            resumeAudio();
        }
    }
    if (m_callbackRequested.exchange(false, std::memory_order_acq_rel)) {
        if (m_plugin && m_plugin->on_main_thread) {
            safeCallPlugin([&]() {
                m_plugin->on_main_thread(m_plugin);
            });
        }
    }
}

void ClapPluginInstance::rescanParamValues() noexcept {
    if (!m_paramsExt || !m_plugin) return;
    std::lock_guard<std::mutex> lock(m_paramCacheMutex);
    const uint32_t paramCount = m_paramsExt->count(m_plugin);
    for (uint32_t i = 0; i < paramCount; ++i) {
        clap_param_info info{};
        if (m_paramsExt->get_info(m_plugin, i, &info)) {
            double val = info.default_value;
            if (m_paramsExt->get_value(m_plugin, info.id, &val)) {
                m_paramCache[info.id] = static_cast<float>(val);
            }
        }
    }
}

void CLAP_ABI ClapPluginInstance::clapHostParamsRescan(const clap_host_t* host, clap_param_rescan_flags flags) {
    if (!host || !host->host_data) return;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    if (flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_ALL)) {
        self->rescanParamValues();
        self->notifyGuiParamEdited(0xFFFFFFFFu, true);
    }
}

void CLAP_ABI ClapPluginInstance::clapHostParamsClear(const clap_host_t* /*host*/, clap_id /*paramId*/, clap_param_clear_flags /*flags*/) {}

void CLAP_ABI ClapPluginInstance::clapHostParamsRequestFlush(const clap_host_t* host) {
    if (!host || !host->host_data) return;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    if (!self->m_isProcessing.load(std::memory_order_acquire)) {
        self->flushPendingParamsIfStopped();
    }
    self->notifyGuiParamEdited(0xFFFFFFFFu, false);
}

void CLAP_ABI ClapPluginInstance::clapHostStateMarkDirty(const clap_host_t* host) {
    if (!host || !host->host_data) return;
    auto* self = static_cast<ClapPluginInstance*>(host->host_data);
    self->notifyGuiParamEdited(0xFFFFFFFFu, true);
}

void ClapPluginInstance::onPluginOutputEvent(const clap_event_header_t* event) noexcept {
    if (!event || event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    if (event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) {
        if (event->size >= sizeof(clap_event_param_gesture_t)) {
            const auto* gev = reinterpret_cast<const clap_event_param_gesture_t*>(event);
            setGuiEditingActive(true);
            m_lastGuiEditedParamId.store(gev->param_id, std::memory_order_relaxed);
        }
    } else if (event->type == CLAP_EVENT_PARAM_VALUE) {
        if (event->size >= sizeof(clap_event_param_value_t)) {
            const auto* pev = reinterpret_cast<const clap_event_param_value_t*>(event);
            m_guiOutParamQueue.try_enqueue({pev->param_id, pev->value});
            notifyGuiParamEdited(pev->param_id, false);
        }
    } else if (event->type == CLAP_EVENT_PARAM_GESTURE_END) {
        if (event->size >= sizeof(clap_event_param_gesture_t)) {
            const auto* gev = reinterpret_cast<const clap_event_param_gesture_t*>(event);
            setGuiEditingActive(false);
            notifyGuiGestureEnded(gev->param_id);
        }
    }
}

void ClapPluginInstance::flushPendingParamsIfStopped() {
    if (!m_plugin || !m_paramsExt || !m_paramsExt->flush || m_faulted.load(std::memory_order_relaxed)) {
        return;
    }
    // According to CLAP specification [active ? audio-thread : main-thread]:
    // If active, flush MUST NOT be called from main thread.
    if (m_isActivated) {
        return;
    }

    m_activeProcessCallbacks.fetch_add(1, std::memory_order_seq_cst);
    struct FlushCallbackGuard {
        std::atomic<uint32_t>& counter;
        ~FlushCallbackGuard() noexcept {
            counter.fetch_sub(1, std::memory_order_release);
        }
    } guard{m_activeProcessCallbacks};
    if (m_isActivated ||
        m_isProcessing.load(std::memory_order_acquire) ||
        m_suspendProcessing.load(std::memory_order_acquire) ||
        m_activeProcessCallbacks.load(std::memory_order_acquire) != 1) {
        return;
    }
    const uint64_t pushGenBefore = m_paramPushGen.load(std::memory_order_acquire);
    ClapEventBuffer flushBuf;
    std::pair<clap_id, double> ev;
    while (m_paramQueue.try_dequeue(ev)) {
        flushBuf.pushParam(ev.first, ev.second);
    }
    clap_input_events_t inEvs = flushBuf.makeInputEvents();
    clap_output_events_t outEvs{};
    outEvs.ctx = this;
    outEvs.try_push = &clapOutputEventsTryPush;

    safeCallPlugin([&]() {
        m_paramsExt->flush(m_plugin, &inEvs, &outEvs);
    });
    m_paramDrainGen.store(pushGenBefore, std::memory_order_release);
}

void ClapPluginInstance::drainGuiOutParamQueue() const {
    std::pair<clap_id, double> guiEv;
    while (m_guiOutParamQueue.try_dequeue(guiEv)) {
        std::lock_guard<std::mutex> lock(m_paramCacheMutex);
        m_paramCache[static_cast<uint32_t>(guiEv.first)] = static_cast<float>(guiEv.second);
    }
}

std::unique_ptr<ClapPluginInstance> ClapPluginInstance::loadFromFile(const std::string& path, uint32_t pluginIndex) {
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(wlen > 0 ? wlen - 1 : 0, L'\0');
    if (wlen > 1) {
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    }
    HMODULE hLib = LoadLibraryW(wpath.c_str());
    if (!hLib) {
        // Fallback to ANSI if UTF-8 conversion fails or module not found
        hLib = LoadLibraryA(path.c_str());
    }
    if (!hLib) {
        std::cerr << "Failed to LoadLibrary for CLAP: " << path << "\n";
        return nullptr;
    }

    auto* entry = reinterpret_cast<const clap_plugin_entry*>(reinterpret_cast<void*>(GetProcAddress(hLib, "clap_entry")));
    if (!entry) {
        std::cerr << "CLAP entry point 'clap_entry' not found in: " << path << "\n";
        FreeLibrary(hLib);
        return nullptr;
    }

    if (!entry->init(path.c_str())) {
        std::cerr << "CLAP entry->init() failed for: " << path << "\n";
        FreeLibrary(hLib);
        return nullptr;
    }

    auto* factory = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) {
        std::cerr << "CLAP get_factory(CLAP_PLUGIN_FACTORY_ID) failed\n";
        entry->deinit();
        FreeLibrary(hLib);
        return nullptr;
    }

    const uint32_t count = factory->get_plugin_count(factory);
    if (pluginIndex >= count) {
        entry->deinit();
        FreeLibrary(hLib);
        return nullptr;
    }

    const clap_plugin_descriptor* desc = factory->get_plugin_descriptor(factory, pluginIndex);
    if (!desc) {
        entry->deinit();
        FreeLibrary(hLib);
        return nullptr;
    }

    if (!clap_version_is_compatible(entry->clap_version)) {
        std::cerr << "Incompatible CLAP version in: " << path << "\n";
        entry->deinit();
        FreeLibrary(hLib);
        return nullptr;
    }

    auto instance = std::make_unique<ClapPluginInstance>();
    instance->m_libraryModule = hLib;
    instance->m_entry = entry;
    instance->m_path = path;
    instance->m_bundleFileName = std::filesystem::path(path).filename().string();
    instance->m_clapId = desc->id ? desc->id : "";
    instance->m_name = desc->name ? desc->name : "CLAP Plugin";
    instance->m_vendor = desc->vendor ? desc->vendor : "Unknown";
    instance->m_version = desc->version ? desc->version : "1.0.0";
    instance->m_mainThreadId = ClapPluginInstance::mainThreadId();

    instance->m_host.clap_version = CLAP_VERSION;
    instance->m_host.host_data = instance.get();
    instance->m_host.name = "Praccy";
    instance->m_host.vendor = "Praccy Audio";
    instance->m_host.url = "https://github.com/praccy";
    instance->m_host.version = "1.0.0";
    instance->m_host.get_extension = &ClapPluginInstance::clapHostGetExtension;
    instance->m_host.request_restart = &ClapPluginInstance::clapHostRequestRestart;
    instance->m_host.request_process = &ClapPluginInstance::clapHostRequestProcess;
    instance->m_host.request_callback = &ClapPluginInstance::clapHostRequestCallback;

    instance->m_plugin = factory->create_plugin(factory, &instance->m_host, desc->id);
    if (!instance->m_plugin) {
        std::cerr << "factory->create_plugin failed for id: " << desc->id << "\n";
        entry->deinit();
        FreeLibrary(hLib);
        return nullptr;
    }

    if (!instance->m_plugin->init(instance->m_plugin)) {
        std::cerr << "plugin->init() failed\n";
        instance->m_plugin->destroy(instance->m_plugin);
        entry->deinit();
        FreeLibrary(hLib);
        return nullptr;
    }

    // Query standard extensions
    instance->m_guiExt = static_cast<const clap_plugin_gui*>(instance->m_plugin->get_extension(instance->m_plugin, CLAP_EXT_GUI));
    instance->m_paramsExt = static_cast<const clap_plugin_params*>(instance->m_plugin->get_extension(instance->m_plugin, CLAP_EXT_PARAMS));
    instance->m_stateExt = static_cast<const clap_plugin_state*>(instance->m_plugin->get_extension(instance->m_plugin, CLAP_EXT_STATE));
    instance->m_latencyExt = static_cast<const clap_plugin_latency*>(instance->m_plugin->get_extension(instance->m_plugin, CLAP_EXT_LATENCY));
    instance->m_audioPortsExt = static_cast<const clap_plugin_audio_ports*>(instance->m_plugin->get_extension(instance->m_plugin, CLAP_EXT_AUDIO_PORTS));
    instance->m_timerExt = static_cast<const clap_plugin_timer_support*>(instance->m_plugin->get_extension(instance->m_plugin, CLAP_EXT_TIMER_SUPPORT));

    if (instance->m_audioPortsExt && instance->m_audioPortsExt->count) {
        instance->m_numAudioInputs = instance->m_audioPortsExt->count(instance->m_plugin, true);
        instance->m_numAudioOutputs = instance->m_audioPortsExt->count(instance->m_plugin, false);
    }

    if (instance->m_paramsExt) {
        const uint32_t paramCount = instance->m_paramsExt->count(instance->m_plugin);
        for (uint32_t i = 0; i < paramCount; ++i) {
            clap_param_info info{};
            if (instance->m_paramsExt->get_info(instance->m_plugin, i, &info)) {
                double val = info.default_value;
                instance->m_paramsExt->get_value(instance->m_plugin, info.id, &val);
                instance->m_paramCache[info.id] = static_cast<float>(val);
            }
        }
    }

    return instance;
}

ClapPluginInstance::~ClapPluginInstance() {
    suspendAndFenceAudio();
    if (m_timerLifetimeToken) {
        m_timerLifetimeToken->store(nullptr, std::memory_order_release);
    }
    closeGui();
    {
        std::lock_guard<std::mutex> lock(s_timerMutex);
        for (auto& [cId, tid] : m_activeTimers) {
            KillTimer(nullptr, tid);
            s_win32Timers.erase(tid);
        }
        m_activeTimers.clear();
    }
    if (m_plugin) {
        if (m_isProcessing.load(std::memory_order_acquire)) {
            m_plugin->stop_processing(m_plugin);
            m_isProcessing.store(false, std::memory_order_release);
        }
        if (m_isActivated) {
            m_plugin->deactivate(m_plugin);
            m_isActivated = false;
        }
        m_plugin->destroy(m_plugin);
        m_plugin = nullptr;
    }
    if (m_entry) {
        m_entry->deinit();
        m_entry = nullptr;
    }
    if (m_libraryModule) {
        FreeLibrary(m_libraryModule);
        m_libraryModule = nullptr;
    }
}

void ClapPluginInstance::prepare(double sampleRate, uint32_t maxBlockSize) {
    suspendAndFenceAudio();
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;
    if (m_plugin) {
        if (m_isProcessing.load(std::memory_order_acquire)) {
            m_plugin->stop_processing(m_plugin);
            m_isProcessing.store(false, std::memory_order_release);
        }
        if (m_isActivated) {
            m_plugin->deactivate(m_plugin);
            m_isActivated = false;
        }
        if (m_plugin->activate(m_plugin, sampleRate, 1, maxBlockSize)) {
            m_isActivated = true;
        }
        if (m_latencyExt && m_latencyExt->get && m_isActivated) {
            m_latencySamples = m_latencyExt->get(m_plugin);
        }
    }
    resumeAudio();
}

void ClapPluginInstance::process(audio::AudioProcessContext& ctx) {
    if (!m_plugin || ctx.numSamples == 0 ||
        m_faulted.load(std::memory_order_relaxed) ||
        m_suspendProcessing.load(std::memory_order_acquire)) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    m_activeProcessCallbacks.fetch_add(1, std::memory_order_seq_cst);
    struct ActiveCallbackGuard {
        std::atomic<uint32_t>& counter;
        ~ActiveCallbackGuard() noexcept {
            counter.fetch_sub(1, std::memory_order_release);
        }
    } callbackGuard{m_activeProcessCallbacks};
    if (m_suspendProcessing.load(std::memory_order_acquire) ||
        m_faulted.load(std::memory_order_relaxed)) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    m_audioThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    const uint64_t pushGenBefore = m_paramPushGen.load(std::memory_order_acquire);

    const uint32_t numSamples = ctx.numSamples;
    const uint32_t inCh = ctx.input.numChannels();
    const uint32_t outCh = ctx.output.numChannels();

    const uint32_t safeInCh = (std::min)(inCh, static_cast<uint32_t>(m_inChannelPtrs.size()));
    for (uint32_t c = 0; c < safeInCh; ++c) {
        m_inChannelPtrs[c] = ctx.input.channel(c);
    }
    clap_audio_buffer inBuf{};
    inBuf.data32 = m_inChannelPtrs.data();
    inBuf.channel_count = safeInCh;
    inBuf.latency = 0;
    inBuf.constant_mask = 0;

    const uint32_t safeOutCh = (std::min)(outCh, static_cast<uint32_t>(m_outChannelPtrs.size()));
    for (uint32_t c = 0; c < safeOutCh; ++c) {
        m_outChannelPtrs[c] = ctx.output.channel(c);
    }
    clap_audio_buffer outBuf{};
    outBuf.data32 = m_outChannelPtrs.data();
    outBuf.channel_count = safeOutCh;
    outBuf.latency = 0;
    outBuf.constant_mask = 0;

    m_eventBuffer.clear();
    // 1. Drain UI thread queue (SPSC: UI producer, Audio consumer)
    std::pair<clap_id, double> ev;
    while (m_paramQueue.try_dequeue(ev)) {
        m_eventBuffer.pushParam(ev.first, ev.second);
    }
    // 2. Drain audio-thread automation events
    for (size_t i = 0; i < m_audioThreadEventCount; ++i) {
        m_eventBuffer.pushParam(m_audioThreadEvents[i].id, m_audioThreadEvents[i].value);
    }
    m_audioThreadEventCount = 0;

    clap_input_events_t inEvs = m_eventBuffer.makeInputEvents();
    clap_output_events_t outEvs{};
    outEvs.ctx = this;
    outEvs.try_push = &clapOutputEventsTryPush;

    // Live transport broadcast (F-12)
    m_transportEvent = {};
    m_transportEvent.header.size = sizeof(clap_event_transport_t);
    m_transportEvent.header.time = 0;
    m_transportEvent.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    m_transportEvent.header.type = CLAP_EVENT_TRANSPORT;
    m_transportEvent.header.flags = 0;
    m_transportEvent.flags = CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
    m_transportEvent.tempo = (ctx.tempoBpm > 1.0) ? ctx.tempoBpm : 120.0;
    m_transportEvent.tsig_num = static_cast<uint16_t>((ctx.timeSigNumerator > 0) ? ctx.timeSigNumerator : 4);
    m_transportEvent.tsig_denom = static_cast<uint16_t>((ctx.timeSigDenominator > 0) ? ctx.timeSigDenominator : 4);
    const double effectiveSr = (ctx.sampleRate > 0.0) ? ctx.sampleRate : m_sampleRate;
    m_transportEvent.song_pos_seconds = (effectiveSr > 0.0) ? static_cast<clap_sectime>(static_cast<double>(ctx.projectTimeSamples) / effectiveSr * static_cast<double>(CLAP_SECTIME_FACTOR)) : 0;

    clap_process processData{};
    processData.steady_time = -1;
    processData.frames_count = numSamples;
    processData.transport = &m_transportEvent;
    processData.audio_inputs = (m_numAudioInputs > 0 && inCh > 0) ? &inBuf : nullptr;
    processData.audio_outputs = (m_numAudioOutputs > 0 && outCh > 0) ? &outBuf : nullptr;
    processData.audio_inputs_count = (m_numAudioInputs > 0 && inCh > 0) ? 1 : 0;
    processData.audio_outputs_count = (m_numAudioOutputs > 0 && outCh > 0) ? 1 : 0;
    processData.in_events = &inEvs;
    processData.out_events = &outEvs;

    DWORD exCode = 0;
    bool ok = safeCallPluginAudio([&]() {
        if (!m_isProcessing.load(std::memory_order_relaxed)) {
            if (m_plugin->start_processing(m_plugin)) {
                m_isProcessing.store(true, std::memory_order_release);
            } else {
                return;
            }
        }
        m_plugin->process(m_plugin, &processData);
    }, &exCode);

    m_eventBuffer.clear();
    m_paramDrainGen.store(pushGenBefore, std::memory_order_release);

    if (!ok) {
        m_faulted.store(true, std::memory_order_release);
        m_faultCode.store(exCode, std::memory_order_release);
        formatFaultReason(m_faultReasonBuf, "CLAP crash: ", exCode);
        m_faultReason.store(m_faultReasonBuf, std::memory_order_release);
        ctx.output.copyFrom(ctx.input);
    }
}

void ClapPluginInstance::reset() {
    suspendAndFenceAudio();
    std::pair<clap_id, double> dummy;
    while (m_paramQueue.try_dequeue(dummy)) {}
    m_audioThreadEventCount = 0;
    m_eventBuffer.clear();
    if (m_plugin && !m_faulted.load(std::memory_order_relaxed)) {
        safeCallPlugin([&]() {
            m_plugin->reset(m_plugin);
        });
    }
    resumeAudio();
}

size_t ClapPluginInstance::numParameters() const noexcept {
    if (m_paramsExt && m_plugin && !m_faulted.load(std::memory_order_relaxed)) {
        size_t count = 0;
        safeCallPlugin([&]() {
            count = m_paramsExt->count(m_plugin);
        });
        return count;
    }
    return 0;
}

PluginParameterDesc ClapPluginInstance::getParameterDesc(size_t index) const {
    PluginParameterDesc desc;
    if (m_paramsExt && m_plugin && !m_faulted.load(std::memory_order_relaxed)) {
        safeCallPlugin([&]() {
            clap_param_info info{};
            if (m_paramsExt->get_info(m_plugin, static_cast<uint32_t>(index), &info)) {
                desc.id = info.id;
                desc.name = info.name;
                desc.minValue = static_cast<float>(info.min_value);
                desc.maxValue = static_cast<float>(info.max_value);
                desc.defaultValue = static_cast<float>(info.default_value);
                desc.currentValue = getParameterValue(info.id);
            }
        });
    }
    return desc;
}

void ClapPluginInstance::setParameterValue(uint32_t paramId, float value) {
    if (m_faulted.load(std::memory_order_relaxed)) return;
    if (GetCurrentThreadId() != m_audioThreadId.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(m_paramCacheMutex);
        m_paramCache[paramId] = value;
    }
    m_paramPushGen.fetch_add(1, std::memory_order_release);
    m_paramQueue.try_enqueue({static_cast<clap_id>(paramId), static_cast<double>(value)});

    if (GetCurrentThreadId() != m_audioThreadId.load(std::memory_order_relaxed) &&
        !m_isProcessing.load(std::memory_order_acquire)) {
        flushPendingParamsIfStopped();
    }
}

void ClapPluginInstance::setParameterFromAudioThread(uint32_t paramId, float value) noexcept {
    if (m_faulted.load(std::memory_order_relaxed)) return;
    const auto cid = static_cast<clap_id>(paramId);
    const double dval = static_cast<double>(value);
    // SPSC: m_guiOutParamQueue written ONLY by audio thread, read ONLY by UI thread
    m_guiOutParamQueue.try_enqueue({cid, dval});

    // Audio-thread local queue drained at top of process() - never touch m_paramQueue
    for (size_t i = 0; i < m_audioThreadEventCount; ++i) {
        if (m_audioThreadEvents[i].id == cid) {
            m_audioThreadEvents[i].value = dval;
            return;
        }
    }
    if (m_audioThreadEventCount < m_audioThreadEvents.size()) {
        m_audioThreadEvents[m_audioThreadEventCount++] = {cid, dval};
    }
}

float ClapPluginInstance::getParameterValue(uint32_t paramId) const {
    if (m_faulted.load(std::memory_order_relaxed)) return 0.0f;

    drainGuiOutParamQueue();

    // If a UI-thread parameter change is still queued awaiting the next audio block, return the cached value
    if (m_paramDrainGen.load(std::memory_order_acquire) < m_paramPushGen.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(m_paramCacheMutex);
        auto it = m_paramCache.find(paramId);
        if (it != m_paramCache.end()) {
            return it->second;
        }
    }

    if (m_paramsExt && m_plugin) {
        float result = 0.0f;
        bool gotVal = false;
        safeCallPlugin([&]() {
            double val = 0.0;
            if (m_paramsExt->get_value(m_plugin, paramId, &val)) {
                result = static_cast<float>(val);
                gotVal = true;
            }
        });
        if (gotVal) {
            std::lock_guard<std::mutex> lock(m_paramCacheMutex);
            m_paramCache[paramId] = result;
            return result;
        }
    }

    std::lock_guard<std::mutex> lock(m_paramCacheMutex);
    auto it = m_paramCache.find(paramId);
    return (it != m_paramCache.end()) ? it->second : 0.0f;
}

bool ClapPluginInstance::hasCustomGui() const noexcept {
    return m_guiExt != nullptr;
}

bool ClapPluginInstance::openGui(HWND parentHwnd) {
    if (m_faulted.load(std::memory_order_acquire)) return false;
    if (!m_guiExt || !m_plugin || !parentHwnd) return false;

    if (!m_guiExt->is_api_supported(m_plugin, CLAP_WINDOW_API_WIN32, false)) {
        return false;
    }

    bool opened = false;
    DWORD exCode = 0;
    bool ok = safeCallPluginGui([&]() {
        if (!m_guiCreated) {
            if (!m_guiExt->create(m_plugin, CLAP_WINDOW_API_WIN32, false)) {
                return;
            }
            m_guiCreated = true;
        }

        clap_window win{};
        win.api = CLAP_WINDOW_API_WIN32;
        win.win32 = parentHwnd;

        if (!m_guiExt->set_parent(m_plugin, &win)) {
            return;
        }

        m_guiParentHwnd = parentHwnd;
        opened = m_guiExt->show(m_plugin);
    }, &exCode);

    if (!ok) {
        m_faulted.store(true, std::memory_order_release);
        m_faultCode.store(exCode, std::memory_order_release);
        formatFaultReason(m_faultReasonBuf, "CLAP GUI error: ", exCode);
        m_faultReason.store(m_faultReasonBuf, std::memory_order_release);
        return false;
    }

    return opened;
}

void ClapPluginInstance::onGuiResize(int width, int height) {
    if (m_guiExt && m_plugin && m_guiCreated && width > 64 && height > 64 && !m_faulted.load(std::memory_order_relaxed)) {
        safeCallPluginGui([&]() {
            if (m_guiExt->set_size) {
                m_guiExt->set_size(m_plugin, static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            }
        });
    }
}

void ClapPluginInstance::closeGui() {
    if (m_guiExt && m_plugin && m_guiCreated) {
        safeCallPluginGui([&]() {
            m_guiExt->hide(m_plugin);
            m_guiExt->destroy(m_plugin);
        });
        m_guiCreated = false;
        m_guiParentHwnd = nullptr;
    }
}

void ClapPluginInstance::getPreferredSize(int& width, int& height) const {
    if (m_guiExt && m_plugin && m_guiCreated && !m_faulted.load(std::memory_order_relaxed)) {
        safeCallPlugin([&]() {
            uint32_t w = 850, h = 600;
            if (m_guiExt->get_size && m_guiExt->get_size(m_plugin, &w, &h)) {
                if (w > 100 && h > 100) {
                    width = static_cast<int>(w);
                    height = static_cast<int>(h);
                }
            }
        });
        return;
    }
    width = 850;
    height = 600;
}

void ClapPluginInstance::resetFault() noexcept {
    suspendAndFenceAudio();
    std::pair<clap_id, double> dummy;
    while (m_paramQueue.try_dequeue(dummy)) {}
    while (m_guiOutParamQueue.try_dequeue(dummy)) {}
    m_audioThreadEventCount = 0;
    m_eventBuffer.clear();
    m_isProcessing.store(false, std::memory_order_release);
    if (m_plugin) {
        safeCallPlugin([&]() {
            m_plugin->reset(m_plugin);
        });
    }
    IPluginInstance::resetFault();
    resumeAudio();
}

std::vector<uint8_t> ClapPluginInstance::saveState() const {
    if (!m_stateExt || !m_plugin || m_faulted.load(std::memory_order_relaxed)) {
        return {};
    }

    std::vector<uint8_t> buffer;
    clap_ostream stream{};
    stream.ctx = &buffer;
    stream.write = [](const struct clap_ostream* s, const void* data, uint64_t size) -> int64_t {
        if (!s || !s->ctx || !data || size == 0) return 0;
        auto* vec = static_cast<std::vector<uint8_t>*>(s->ctx);
        const auto* bytes = static_cast<const uint8_t*>(data);
        vec->insert(vec->end(), bytes, bytes + size);
        return static_cast<int64_t>(size);
    };

    bool ok = false;
    safeCallPlugin([&]() {
        ok = m_stateExt->save(m_plugin, &stream);
    });

    if (ok && !buffer.empty()) {
        updateStateHash(buffer);
    }
    return ok ? buffer : std::vector<uint8_t>{};
}

bool ClapPluginInstance::loadState(const std::vector<uint8_t>& state) {
    if (state.empty() || !m_stateExt || !m_plugin || m_faulted.load(std::memory_order_relaxed)) {
        return false;
    }

    suspendAndFenceAudio();

    // Drain any stale queued parameter changes while audio processing is fenced
    std::pair<clap_id, double> dummy;
    while (m_paramQueue.try_dequeue(dummy)) {}
    while (m_guiOutParamQueue.try_dequeue(dummy)) {}
    m_audioThreadEventCount = 0;
    m_eventBuffer.clear();

    struct ReadCtx {
        const uint8_t* data;
        size_t size;
        size_t offset;
    } ctx{state.data(), state.size(), 0};

    clap_istream stream{};
    stream.ctx = &ctx;
    stream.read = [](const struct clap_istream* s, void* data, uint64_t size) -> int64_t {
        if (!s || !s->ctx || !data || size == 0) return 0;
        auto* c = static_cast<ReadCtx*>(s->ctx);
        if (c->offset >= c->size) return 0; // EOF
        size_t toRead = std::min(static_cast<size_t>(size), c->size - c->offset);
        std::memcpy(data, c->data + c->offset, toRead);
        c->offset += toRead;
        return static_cast<int64_t>(toRead);
    };

    bool ok = false;
    safeCallPlugin([&]() {
        ok = m_stateExt->load(m_plugin, &stream);
    });

    if (ok && m_paramsExt) {
        std::lock_guard<std::mutex> lock(m_paramCacheMutex);
        const uint32_t paramCount = m_paramsExt->count(m_plugin);
        for (uint32_t i = 0; i < paramCount; ++i) {
            clap_param_info info{};
            if (m_paramsExt->get_info(m_plugin, i, &info)) {
                double val = info.default_value;
                if (m_paramsExt->get_value(m_plugin, info.id, &val)) {
                    m_paramCache[info.id] = static_cast<float>(val);
                }
            }
        }
    }
    m_paramDrainGen.store(m_paramPushGen.load(std::memory_order_relaxed), std::memory_order_release);
    resumeAudio();

    if (ok) {
        updateStateHash(state);
    }
    clearGuiParamEdit();
    return ok;
}

} // namespace praccy::plugins
