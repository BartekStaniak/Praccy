#include "vst3_host.h"
#include "crash_isolation.h"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstmessage.h>
#include <pluginterfaces/vst/ivstprocesscontext.h>

namespace Steinberg {
DEF_CLASS_IID (IPlugView)
DEF_CLASS_IID (IPlugFrame)
namespace Vst {
DEF_CLASS_IID (IComponent)
DEF_CLASS_IID (IAudioProcessor)
DEF_CLASS_IID (IEditController)
DEF_CLASS_IID (IComponentHandler)
DEF_CLASS_IID (IConnectionPoint)
DEF_CLASS_IID (IMessage)
DEF_CLASS_IID (IParameterChanges)
DEF_CLASS_IID (IParamValueQueue)
}
}

namespace praccy::plugins {

struct AsyncGuiOpenReq {
    HWND parentHwnd{nullptr};
    std::function<void(bool)> callback;
};

struct SyncHostTaskReq {
    std::function<void()> fn;
    HANDLE ackEvent{nullptr};
    std::atomic<int> refCount{2};
    void release() {
        if (refCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            if (ackEvent) {
                CloseHandle(ackEvent);
            }
            delete this;
        }
    }
};

struct AsyncHostTaskReq {
    std::function<void()> fn;
};

using InitDllProc = bool (PLUGIN_API *)();
using ExitDllProc = bool (PLUGIN_API *)();
using GetFactoryProc = Steinberg::IPluginFactory* (PLUGIN_API *)();

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

class PraccyMemoryStream : public Steinberg::IBStream {
public:
    std::vector<char> buffer;
    Steinberg::int64 pos{0};

    PraccyMemoryStream() = default;
    virtual ~PraccyMemoryStream() = default;
    explicit PraccyMemoryStream(const std::vector<uint8_t>& data) {
        buffer.assign(data.begin(), data.end());
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::IBStream::iid, sizeof(Steinberg::TUID)) == 0 ||
            memcmp(_iid, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0) {
            addRef();
            *obj = static_cast<Steinberg::IBStream*>(this);
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    std::atomic<uint32_t> m_refCount{1};
    Steinberg::uint32 PLUGIN_API addRef() override {
        return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    Steinberg::uint32 PLUGIN_API release() override {
        const uint32_t prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
        return (prev > 0) ? (prev - 1) : 0;
    }

    Steinberg::tresult PLUGIN_API read(void* dest, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override {
        if (!dest || numBytes < 0) return Steinberg::kInvalidArgument;
        if (pos < 0 || static_cast<size_t>(pos) >= buffer.size()) {
            if (numBytesRead) *numBytesRead = 0;
            return Steinberg::kResultOk;
        }
        const size_t avail = buffer.size() - static_cast<size_t>(pos);
        const Steinberg::int32 available = static_cast<Steinberg::int32>(
            std::min<size_t>(avail, static_cast<size_t>(std::numeric_limits<Steinberg::int32>::max())));
        Steinberg::int32 toRead = (std::min)(numBytes, available);
        if (toRead > 0) {
            memcpy(dest, buffer.data() + pos, static_cast<size_t>(toRead));
            pos += toRead;
        }
        if (numBytesRead) *numBytesRead = toRead;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API write(void* src, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten) override {
        if (!src || numBytes < 0 || pos < 0) return Steinberg::kInvalidArgument;
        const size_t endPos = static_cast<size_t>(pos) + static_cast<size_t>(numBytes);
        if (endPos > buffer.size()) {
            buffer.resize(endPos);
        }
        if (numBytes > 0) {
            memcpy(buffer.data() + pos, src, static_cast<size_t>(numBytes));
            pos += numBytes;
        }
        if (numBytesWritten) *numBytesWritten = numBytes;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 offset, Steinberg::int32 mode, Steinberg::int64* result) override {
        const auto bufSize = static_cast<Steinberg::int64>(buffer.size());
        if (mode == kIBSeekSet) pos = offset;
        else if (mode == kIBSeekCur) pos += offset;
        else if (mode == kIBSeekEnd) pos = bufSize + offset;
        if (pos < 0) pos = 0;
        if (pos > bufSize) pos = bufSize;
        if (result) *result = pos;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* result) override {
        if (result) *result = pos;
        return Steinberg::kResultOk;
    }
};

class PraccyComponentHandler : public Steinberg::Vst::IComponentHandler {
public:
    std::atomic<Vst3PluginInstance*> owner{nullptr};
    std::atomic<uint32_t> m_refCount{1};

    virtual ~PraccyComponentHandler() = default;

    void detachHost() noexcept {
        owner.store(nullptr, std::memory_order_release);
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::Vst::IComponentHandler::iid, sizeof(Steinberg::TUID)) == 0 ||
            memcmp(_iid, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0) {
            addRef();
            *obj = static_cast<Steinberg::Vst::IComponentHandler*>(this);
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

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override {
        auto* o = owner.load(std::memory_order_acquire);
        if (o) o->onGuiBeginEdit(id);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) override {
        auto* o = owner.load(std::memory_order_acquire);
        if (o) o->onGuiPerformEdit(id, valueNormalized);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override {
        auto* o = owner.load(std::memory_order_acquire);
        if (o) o->onGuiEndEdit(id);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override {
        auto* o = owner.load(std::memory_order_acquire);
        if (o) o->onGuiRestartComponent(flags);
        return Steinberg::kResultOk;
    }
};

class PraccyPlugFrame : public Steinberg::IPlugFrame {
public:
    std::atomic<HWND> parentHwnd{nullptr};
    std::atomic<bool> inResize{false};
    std::atomic<uint32_t> m_refCount{1};

    virtual ~PraccyPlugFrame() = default;

    void detachHost() noexcept {
        parentHwnd.store(nullptr, std::memory_order_release);
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::IPlugFrame::iid, sizeof(Steinberg::TUID)) == 0 ||
            memcmp(_iid, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0) {
            addRef();
            *obj = static_cast<Steinberg::IPlugFrame*>(this);
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

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override {
        if (!view || !newSize) return Steinberg::kInvalidArgument;
        HWND hwnd = parentHwnd.load(std::memory_order_acquire);
        if (!hwnd || !IsWindow(hwnd)) return Steinberg::kResultOk;
        bool expected = false;
        if (!inResize.compare_exchange_strong(expected, true)) return Steinberg::kResultOk;
        int newW = newSize->right - newSize->left;
        int newH = newSize->bottom - newSize->top;
        const DWORD winThread = GetWindowThreadProcessId(hwnd, nullptr);
        if (GetCurrentThreadId() == winThread) {
            RECT wr = {0, 0, newW, newH};
            AdjustWindowRectEx(&wr, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)), FALSE, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_EXSTYLE)));
            int totalW = wr.right - wr.left;
            int totalH = wr.bottom - wr.top;
            SetWindowPos(hwnd, nullptr, 0, 0, totalW, totalH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            PostMessageW(hwnd, WM_USER + 201, static_cast<WPARAM>(newW), static_cast<LPARAM>(newH));
        }
        view->onSize(newSize);
        inResize.store(false, std::memory_order_release);
        return Steinberg::kResultOk;
    }
};

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

struct Vst3PluginInstance::Impl {
    PraccyComponentHandler componentHandler;
    PraccyPlugFrame plugFrame;
    Steinberg::Vst::ProcessContext processContext{};
};

Vst3PluginInstance::Vst3PluginInstance() : m_impl(std::make_unique<Impl>()) {
    m_impl->componentHandler.owner.store(this, std::memory_order_release);
}

void Vst3PluginInstance::suspendAndFenceAudio() noexcept {
    m_suspendProcessing.store(true, std::memory_order_seq_cst);
    if (GetCurrentThreadId() != m_audioThreadId.load(std::memory_order_relaxed)) {
        while (m_activeProcessCallbacks.load(std::memory_order_acquire) != 0) {
#if defined(_M_X64) || defined(__x86_64__)
            _mm_pause();
#endif
        }
    }
}

void Vst3PluginInstance::resumeAudio() noexcept {
    m_suspendProcessing.store(false, std::memory_order_release);
}

void Vst3PluginInstance::enqueueParamForAudio(Steinberg::Vst::ParamID paramId, double normVal) noexcept {
    const DWORD hostTid = m_hostThreadId.load(std::memory_order_acquire);
    if (hostTid != 0 && GetCurrentThreadId() == hostTid) {
        m_guiHostParamQueue.try_enqueue({paramId, normVal});
    } else {
        std::lock_guard<std::mutex> lock(m_uiParamQueueMutex);
        m_uiParamQueue.try_enqueue({paramId, normVal});
    }
}

void Vst3PluginInstance::drainAllParamQueues() noexcept {
    std::pair<Steinberg::Vst::ParamID, double> dummy;
    while (m_uiParamQueue.try_dequeue(dummy)) {}
    while (m_guiHostParamQueue.try_dequeue(dummy)) {}
}

void Vst3PluginInstance::invokeOnHostThread(const std::function<void()>& fn, DWORD timeoutMs) const {
    if (!fn) return;
    const DWORD tid = m_hostThreadId.load(std::memory_order_acquire);
    if (tid == 0 || GetCurrentThreadId() == tid) {
        fn();
        return;
    }

    HANDLE ack = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!ack) {
        fn();
        return;
    }

    auto* req = new SyncHostTaskReq{fn, ack, 2};
    if (!PostThreadMessageW(tid, WM_USER + 105, reinterpret_cast<WPARAM>(req), 0)) {
        req->release();
        req->release();
        CloseHandle(ack);
        fn();
        return;
    }

    const auto startTime = std::chrono::steady_clock::now();
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= timeoutMs) {
            break;
        }
        DWORD remainingMs = static_cast<DWORD>(timeoutMs - elapsed);
        DWORD waitRes = MsgWaitForMultipleObjectsEx(1, &ack, remainingMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waitRes == WAIT_OBJECT_0) {
            break;
        } else if (waitRes == WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        } else {
            break;
        }
    }
    CloseHandle(ack);
    req->release();
}

void Vst3PluginInstance::postToHostThread(std::function<void()> fn) const {
    if (!fn) return;
    const DWORD tid = m_hostThreadId.load(std::memory_order_acquire);
    if (tid == 0 || GetCurrentThreadId() == tid) {
        fn();
        return;
    }
    auto* req = new AsyncHostTaskReq{std::move(fn)};
    if (!PostThreadMessageW(tid, WM_USER + 106, reinterpret_cast<WPARAM>(req), 0)) {
        delete req;
    }
}

void Vst3PluginInstance::onGuiBeginEdit(Steinberg::Vst::ParamID paramId) noexcept {
    if (m_suppressHandlerCallbacks.load(std::memory_order_relaxed)) return;
    setGuiEditingActive(true);
    m_lastGuiEditedParamId.store(static_cast<uint32_t>(paramId), std::memory_order_relaxed);
}

void Vst3PluginInstance::onGuiPerformEdit(Steinberg::Vst::ParamID paramId, Steinberg::Vst::ParamValue valueNormalized) noexcept {
    if (m_suppressHandlerCallbacks.load(std::memory_order_relaxed)) return;
    if (m_faulted.load(std::memory_order_relaxed)) return;

    const float normVal = static_cast<float>(std::clamp(valueNormalized, 0.0, 1.0));
    {
        std::lock_guard<std::mutex> lock(m_paramCacheMutex);
        m_paramCache[static_cast<uint32_t>(paramId)] = normVal;
        auto idxIt = m_paramIdToIndex.find(static_cast<uint32_t>(paramId));
        if (idxIt != m_paramIdToIndex.end() && idxIt->second < m_paramDescs.size()) {
            m_paramDescs[idxIt->second].currentValue = normVal;
        }
    }

    // Forward GUI parameter edit to dedicated SPSC audio processor queue
    enqueueParamForAudio(paramId, static_cast<double>(normVal));
    notifyGuiParamEdited(static_cast<uint32_t>(paramId), false);
}

void Vst3PluginInstance::onGuiEndEdit(Steinberg::Vst::ParamID paramId) noexcept {
    if (m_suppressHandlerCallbacks.load(std::memory_order_relaxed)) return;
    setGuiEditingActive(false);
    notifyGuiGestureEnded(static_cast<uint32_t>(paramId));
}

void Vst3PluginInstance::onGuiRestartComponent(Steinberg::int32 flags) noexcept {
    if (m_suppressHandlerCallbacks.load(std::memory_order_relaxed)) return;

    if (flags & Steinberg::Vst::RestartFlags::kLatencyChanged) {
        if (m_processor) {
            m_latencySamples = m_processor->getLatencySamples();
        }
    }

    if (flags & Steinberg::Vst::RestartFlags::kParamTitlesChanged) {
        if (m_controller) {
            std::lock_guard<std::mutex> lock(m_paramCacheMutex);
            const int32_t pCount = m_controller->getParameterCount();
            for (int32_t i = 0; i < pCount && i < static_cast<int32_t>(m_paramDescs.size()); ++i) {
                Steinberg::Vst::ParameterInfo info{};
                if (m_controller->getParameterInfo(i, info) == Steinberg::kResultOk) {
                    char nameBuf[128] = {0};
                    for (int c = 0; c < 127 && info.title[c] != 0; ++c) {
                        nameBuf[c] = static_cast<char>(info.title[c]);
                    }
                    m_paramDescs[i].name = nameBuf;
                    m_paramDescs[i].defaultValue = static_cast<float>(info.defaultNormalizedValue);
                }
            }
        }
    }

    if (flags & Steinberg::Vst::RestartFlags::kParamValuesChanged) {
        if (m_controller) {
            std::lock_guard<std::mutex> lock(m_paramCacheMutex);
            for (auto& desc : m_paramDescs) {
                const double normVal = m_controller->getParamNormalized(desc.id);
                const float fVal = static_cast<float>(normVal);
                m_paramCache[desc.id] = fVal;
                desc.currentValue = fVal;
                enqueueParamForAudio(desc.id, normVal);
            }
        }
        setGuiEditingActive(false);
        notifyGuiParamEdited(0xFFFFFFFFu, true);
    }
}

bool Vst3PluginInstance::initHostInternal(const std::string& path) {
    std::filesystem::path p(path);
    std::filesystem::path dllPath = p;

    // Check if it's a VST3 bundle directory (Windows 10/11 VST3 standard layout)
    if (std::filesystem::is_directory(p)) {
        auto candidate = p / "Contents" / "x86_64-win" / (p.stem().string() + ".vst3");
        if (std::filesystem::exists(candidate)) {
            dllPath = candidate;
        } else {
            auto archDir = p / "Contents" / "x86_64-win";
            if (std::filesystem::exists(archDir)) {
                for (const auto& entry : std::filesystem::directory_iterator(archDir)) {
                    if (entry.path().extension() == ".vst3" || entry.path().extension() == ".dll") {
                        dllPath = entry.path();
                        break;
                    }
                }
            }
        }
    }

    HMODULE hLib = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(pluginDllMutex());
        hLib = LoadLibraryW(dllPath.wstring().c_str());
    }
    if (!hLib) {
        std::cerr << "Failed to LoadLibrary for VST3: " << dllPath.string() << "\n";
        return false;
    }

    auto* initDll = reinterpret_cast<InitDllProc>(reinterpret_cast<void*>(GetProcAddress(hLib, "InitDll")));
    if (initDll) {
        initDll();
    }

    auto* getFactory = reinterpret_cast<GetFactoryProc>(reinterpret_cast<void*>(GetProcAddress(hLib, "GetPluginFactory")));
    if (!getFactory) {
        std::cerr << "GetPluginFactory not found in: " << dllPath.string() << "\n";
        FreeLibrary(hLib);
        return false;
    }

    Steinberg::IPluginFactory* factory = getFactory();
    if (!factory) {
        std::cerr << "GetPluginFactory returned nullptr in: " << dllPath.string() << "\n";
        FreeLibrary(hLib);
        return false;
    }

    // Find Audio Effect Class
    const int32_t classCount = factory->countClasses();
    int32_t targetClassIdx = -1;
    Steinberg::PClassInfo classInfo{};

    for (int32_t i = 0; i < classCount; ++i) {
        if (factory->getClassInfo(i, &classInfo) == Steinberg::kResultOk) {
            if (std::strcmp(classInfo.category, kVstAudioEffectClass) == 0) {
                targetClassIdx = i;
                break;
            }
        }
    }

    if (targetClassIdx == -1) {
        std::cerr << "No audio effect class found in: " << dllPath.string() << "\n";
        factory->release();
        FreeLibrary(hLib);
        return false;
    }

    m_module = hLib;
    m_factory = factory;
    m_path = path;
    m_bundleFileName = std::filesystem::path(path).filename().string();
    Steinberg::FUID fuid = Steinberg::FUID::fromTUID(classInfo.cid);
    char guidStr[33] = {0};
    fuid.toString(guidStr);
    m_classGuid = guidStr;
    m_name = classInfo.name;
    m_vendor = "VST3 Plugin";

    // Query Vendor from factory info if available
    Steinberg::PFactoryInfo factoryInfo{};
    if (factory->getFactoryInfo(&factoryInfo) == Steinberg::kResultOk) {
        if (factoryInfo.vendor[0] != '\0') {
            m_vendor = factoryInfo.vendor;
        }
    }

    // Create Component Instance
    if (factory->createInstance(classInfo.cid, Steinberg::Vst::IComponent::iid, reinterpret_cast<void**>(&m_component)) != Steinberg::kResultOk || !m_component) {
        std::cerr << "Failed to create IComponent for: " << m_name << "\n";
        return false;
    }

    // Query Audio Processor
    if (m_component->queryInterface(Steinberg::Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&m_processor)) != Steinberg::kResultOk || !m_processor) {
        std::cerr << "Failed to query IAudioProcessor for: " << m_name << "\n";
        return false;
    }

    m_component->initialize(nullptr);

    m_impl->processContext.state = Steinberg::Vst::ProcessContext::kPlaying |
                                   Steinberg::Vst::ProcessContext::kTempoValid |
                                   Steinberg::Vst::ProcessContext::kTimeSigValid;
    m_impl->processContext.sampleRate = m_sampleRate;
    m_impl->processContext.tempo = 120.0;
    m_impl->processContext.timeSigNumerator = 4;
    m_impl->processContext.timeSigDenominator = 4;
    m_impl->processContext.projectTimeSamples = 0;

    // Initialize controller and cache on host thread
    ensureControllerInitialized();

    return true;
}

std::unique_ptr<Vst3PluginInstance> Vst3PluginInstance::loadFromFile(const std::string& path) {
    auto instance = std::make_unique<Vst3PluginInstance>();
    auto loadPromise = std::make_shared<std::promise<bool>>();
    auto loadFuture = loadPromise->get_future();

    instance->m_hostThread = std::thread([inst = instance.get(), path, p = loadPromise]() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        (void)hr;
        inst->m_hostThreadId.store(GetCurrentThreadId(), std::memory_order_release);

        // Force creation of Win32 message queue for this thread
        MSG dummyMsg;
        PeekMessageW(&dummyMsg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        bool ok = inst->initHostInternal(path);
        p->set_value(ok);
        if (!ok) {
            inst->m_hostThreadId.store(0, std::memory_order_release);
            inst->teardownHostInternal();
            CoUninitialize();
            return;
        }

        // Host thread Win32 message pump for plugin GUI and event dispatch
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            if (msg.message == WM_USER + 101) { // OPEN_GUI
                HWND parent = reinterpret_cast<HWND>(msg.lParam);
                HANDLE ack = reinterpret_cast<HANDLE>(msg.wParam);
                inst->openGuiInternal(parent);
                if (ack) SetEvent(ack);
            } else if (msg.message == WM_USER + 102) { // CLOSE_GUI
                HANDLE ack = reinterpret_cast<HANDLE>(msg.wParam);
                inst->closeGuiInternal();
                if (ack) SetEvent(ack);
            } else if (msg.message == WM_USER + 103) { // QUIT
                inst->m_hostThreadId.store(0, std::memory_order_release);
                inst->closeGuiInternal();
                PostQuitMessage(0);
            } else if (msg.message == WM_USER + 104) { // OPEN_GUI_ASYNC
                auto* req = reinterpret_cast<AsyncGuiOpenReq*>(msg.wParam);
                if (req) {
                    bool guiOk = inst->openGuiInternal(req->parentHwnd);
                    inst->m_asyncGuiResults.try_enqueue({ req->parentHwnd, guiOk, std::move(req->callback) });
                    delete req;
                }
            } else if (msg.message == WM_USER + 105) { // SYNC_HOST_TASK
                auto* req = reinterpret_cast<SyncHostTaskReq*>(msg.wParam);
                if (req) {
                    if (req->fn) {
                        req->fn();
                    }
                    if (req->ackEvent) {
                        SetEvent(req->ackEvent);
                    }
                    req->release();
                }
            } else if (msg.message == WM_USER + 106) { // ASYNC_HOST_TASK
                auto* req = reinterpret_cast<AsyncHostTaskReq*>(msg.wParam);
                if (req) {
                    if (req->fn) {
                        req->fn();
                    }
                    delete req;
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        inst->teardownHostInternal();
        CoUninitialize();
    });

    if (!loadFuture.get()) {
        if (instance->m_hostThread.joinable()) {
            instance->m_hostThread.join();
        }
        return nullptr;
    }

    return instance;
}

bool Vst3PluginInstance::ensureControllerInitialized() {
    if (m_controllerInitialized.load(std::memory_order_acquire)) {
        return m_controller != nullptr;
    }
    std::lock_guard<std::mutex> lock(m_controllerInitMutex);
    if (m_controllerInitialized.load(std::memory_order_relaxed)) {
        return m_controller != nullptr;
    }

    if (!m_component || !m_factory) {
        m_controllerInitialized.store(true, std::memory_order_release);
        return false;
    }

    // Create / Query Edit Controller on calling (message/UI) thread
    Steinberg::TUID controllerCid{};
    if (m_component->getControllerClassId(controllerCid) == Steinberg::kResultOk) {
        m_factory->createInstance(controllerCid, Steinberg::Vst::IEditController::iid, reinterpret_cast<void**>(&m_controller));
    }
    if (!m_controller) {
        m_component->queryInterface(Steinberg::Vst::IEditController::iid, reinterpret_cast<void**>(&m_controller));
    }
    if (!m_controller) {
        const int32_t classCount = m_factory->countClasses();
        for (int32_t i = 0; i < classCount; ++i) {
            Steinberg::PClassInfo cInfo{};
            if (m_factory->getClassInfo(i, &cInfo) == Steinberg::kResultOk) {
                if (std::strcmp(cInfo.category, kVstComponentControllerClass) == 0) {
                    m_factory->createInstance(cInfo.cid, Steinberg::Vst::IEditController::iid, reinterpret_cast<void**>(&m_controller));
                    if (m_controller) break;
                }
            }
        }
    }

    if (m_controller) {
        m_controller->initialize(nullptr);
        m_controller->setComponentHandler(&m_impl->componentHandler);

        // Connect IConnectionPoint between component and controller
        Steinberg::Vst::IConnectionPoint* cpComp = nullptr;
        Steinberg::Vst::IConnectionPoint* cpCtrl = nullptr;
        m_component->queryInterface(Steinberg::Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&cpComp));
        m_controller->queryInterface(Steinberg::Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&cpCtrl));
        if (cpComp && cpCtrl) {
            cpComp->connect(cpCtrl);
            cpCtrl->connect(cpComp);
        }
        if (cpComp) cpComp->release();
        if (cpCtrl) cpCtrl->release();

        // Sync initial state from component to controller
        m_suppressHandlerCallbacks.store(true, std::memory_order_relaxed);
        PraccyMemoryStream stateStream;
        if (m_component->getState(&stateStream) == Steinberg::kResultOk) {
            stateStream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            m_controller->setComponentState(&stateStream);
        }
        m_suppressHandlerCallbacks.store(false, std::memory_order_relaxed);

        // Populate initial parameter cache and parameter descriptors
        std::lock_guard<std::mutex> paramLock(m_paramCacheMutex);
        const int32_t pCount = m_controller->getParameterCount();
        m_paramDescs.clear();
        m_paramIdToIndex.clear();
        m_paramCache.clear();
        m_paramDescs.reserve(pCount);
        m_paramIdToIndex.reserve(pCount);
        m_paramCache.reserve(pCount);
        for (int32_t i = 0; i < pCount; ++i) {
            Steinberg::Vst::ParameterInfo info{};
            if (m_controller->getParameterInfo(i, info) == Steinberg::kResultOk) {
                PluginParameterDesc desc;
                desc.id = info.id;
                char nameBuf[128] = {0};
                for (int c = 0; c < 127 && info.title[c] != 0; ++c) {
                    nameBuf[c] = static_cast<char>(info.title[c]);
                }
                desc.name = nameBuf;
                desc.minValue = 0.0f;
                desc.maxValue = 1.0f;
                desc.defaultValue = static_cast<float>(info.defaultNormalizedValue);
                float curVal = static_cast<float>(m_controller->getParamNormalized(info.id));
                desc.currentValue = curVal;

                m_paramIdToIndex[info.id] = m_paramDescs.size();
                m_paramDescs.push_back(std::move(desc));
                m_paramCache[info.id] = curVal;
            }
        }
    }

    m_controllerInitialized.store(true, std::memory_order_release);
    return m_controller != nullptr;
}

void Vst3PluginInstance::teardownHostInternal() {
    closeGuiInternal();

    // 1. Disconnect connection points between component and controller
    if (m_component && m_controller) {
        Steinberg::Vst::IConnectionPoint* cpComp = nullptr;
        Steinberg::Vst::IConnectionPoint* cpCtrl = nullptr;
        m_component->queryInterface(Steinberg::Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&cpComp));
        m_controller->queryInterface(Steinberg::Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&cpCtrl));
        if (cpComp && cpCtrl) {
            cpComp->disconnect(cpCtrl);
            cpCtrl->disconnect(cpComp);
        }
        if (cpComp) cpComp->release();
        if (cpCtrl) cpCtrl->release();
    }

    // 2. Stop audio processing and deactivate component
    if (m_processor) {
        m_processor->setProcessing(false);
    }
    if (m_component) {
        m_component->setActive(false);
    }

    // 3. Detach component handler from controller
    if (m_controller) {
        m_controller->setComponentHandler(nullptr);
    }

    // 4. Check if controller and component share the same COM identity
    bool isSingleComponent = false;
    if (m_controller && m_component) {
        Steinberg::Vst::IComponent* ctrlComp = nullptr;
        if (m_controller->queryInterface(Steinberg::Vst::IComponent::iid, reinterpret_cast<void**>(&ctrlComp)) == Steinberg::kResultOk && ctrlComp) {
            if (ctrlComp == m_component) {
                isSingleComponent = true;
            }
            ctrlComp->release();
        }
    }

    // 5. Terminate and release controller (only terminate if separate from component)
    if (m_controller) {
        if (!isSingleComponent) {
            m_controller->terminate();
        }
        m_controller->release();
        m_controller = nullptr;
    }

    // 6. Release audio processor before terminating component
    if (m_processor) {
        m_processor->release();
        m_processor = nullptr;
    }

    // 7. Terminate and release component
    if (m_component) {
        m_component->terminate();
        m_component->release();
        m_component = nullptr;
    }

    // 7b. Release IPluginFactory COM reference before ExitDll
    if (m_factory) {
        m_factory->release();
        m_factory = nullptr;
    }

    // 8. Cleanly invoke ExitDll and unload module
    if (m_module) {
        auto* exitDll = reinterpret_cast<ExitDllProc>(reinterpret_cast<void*>(GetProcAddress(m_module, "ExitDll")));
        if (exitDll) exitDll();
        FreeLibrary(m_module);
        m_module = nullptr;
    }
}

Vst3PluginInstance::~Vst3PluginInstance() {
    suspendAndFenceAudio();
    if (m_impl) {
        m_impl->componentHandler.detachHost();
        m_impl->plugFrame.detachHost();
    }
    if (m_hostThread.joinable()) {
        const DWORD tid = m_hostThreadId.load(std::memory_order_acquire);
        if (tid != 0) {
            PostThreadMessageW(tid, WM_USER + 103, 0, 0);
        }
        m_hostThread.join();
    }
}

void Vst3PluginInstance::prepare(double sampleRate, uint32_t maxBlockSize) {
    suspendAndFenceAudio();
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;

    if (!m_component || !m_processor) {
        resumeAudio();
        return;
    }

    invokeOnHostThread([&]() {
        safeCallPlugin([&]() {
            m_processor->setProcessing(false);
            m_component->setActive(false);

            m_component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, true);
            m_component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true);

            Steinberg::Vst::ProcessSetup setup{};
            setup.processMode = Steinberg::Vst::kRealtime;
            setup.symbolicSampleSize = Steinberg::Vst::kSample32;
            setup.maxSamplesPerBlock = static_cast<Steinberg::int32>(maxBlockSize);
            setup.sampleRate = sampleRate;

            m_processor->setupProcessing(setup);
            m_component->setActive(true);
            m_processor->setProcessing(true);

            m_latencySamples = m_processor->getLatencySamples();
        });
    });

    m_impl->processContext.state = Steinberg::Vst::ProcessContext::kPlaying |
                                   Steinberg::Vst::ProcessContext::kTempoValid |
                                   Steinberg::Vst::ProcessContext::kTimeSigValid;
    m_impl->processContext.sampleRate = sampleRate;
    m_impl->processContext.tempo = 120.0;
    m_impl->processContext.timeSigNumerator = 4;
    m_impl->processContext.timeSigDenominator = 4;
    m_impl->processContext.projectTimeSamples = 0;
    resumeAudio();
}

void Vst3PluginInstance::process(audio::AudioProcessContext& ctx) {
    if (!m_processor || ctx.numSamples == 0 ||
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

    m_inputParamChanges.clear();
    // 1a. Drain UI thread parameter changes (SPSC)
    std::pair<Steinberg::Vst::ParamID, double> paramChange;
    while (m_uiParamQueue.try_dequeue(paramChange)) {
        m_inputParamChanges.addParameterChange(paramChange.first, paramChange.second);
    }
    // 1b. Drain GUI STA host thread parameter changes (SPSC)
    while (m_guiHostParamQueue.try_dequeue(paramChange)) {
        m_inputParamChanges.addParameterChange(paramChange.first, paramChange.second);
    }
    // 2. Drain audio-thread parameter automation
    for (size_t i = 0; i < m_audioThreadParamCount; ++i) {
        m_inputParamChanges.addParameterChange(m_audioThreadParamEvents[i].id, m_audioThreadParamEvents[i].value);
    }
    m_audioThreadParamCount = 0;

    const uint32_t numSamples = ctx.numSamples;
    Steinberg::Vst::ProcessData data{};
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = numSamples;

    // Live transport broadcast (F-12)
    m_impl->processContext.sampleRate = ctx.sampleRate > 0.0 ? ctx.sampleRate : m_sampleRate;
    m_impl->processContext.tempo = (ctx.tempoBpm > 1.0) ? ctx.tempoBpm : 120.0;
    m_impl->processContext.timeSigNumerator = (ctx.timeSigNumerator > 0) ? ctx.timeSigNumerator : 4;
    m_impl->processContext.timeSigDenominator = (ctx.timeSigDenominator > 0) ? ctx.timeSigDenominator : 4;
    m_impl->processContext.projectTimeSamples = ctx.projectTimeSamples;
    data.processContext = &m_impl->processContext;

    const uint32_t inCh = ctx.input.numChannels();
    const uint32_t safeInCh = (std::min)(inCh, static_cast<uint32_t>(m_inChannelPtrs.size()));
    for (uint32_t c = 0; c < safeInCh; ++c) {
        m_inChannelPtrs[c] = ctx.input.channel(c);
    }
    Steinberg::Vst::AudioBusBuffers inBus{};
    inBus.numChannels = static_cast<Steinberg::int32>(safeInCh);
    inBus.channelBuffers32 = m_inChannelPtrs.data();

    const uint32_t outCh = ctx.output.numChannels();
    const uint32_t safeOutCh = (std::min)(outCh, static_cast<uint32_t>(m_outChannelPtrs.size()));
    for (uint32_t c = 0; c < safeOutCh; ++c) {
        m_outChannelPtrs[c] = ctx.output.channel(c);
    }
    Steinberg::Vst::AudioBusBuffers outBus{};
    outBus.numChannels = static_cast<Steinberg::int32>(safeOutCh);
    outBus.channelBuffers32 = m_outChannelPtrs.data();

    data.numInputs = 1;
    data.inputs = &inBus;
    data.numOutputs = 1;
    data.outputs = &outBus;

    if (m_inputParamChanges.getParameterCount() > 0) {
        data.inputParameterChanges = &m_inputParamChanges;
    } else {
        data.inputParameterChanges = nullptr;
    }

    DWORD exCode = 0;
    bool ok = safeCallPluginAudio([&]() {
        m_processor->process(data);
    }, &exCode);

    m_impl->processContext.projectTimeSamples += numSamples;

    m_inputParamChanges.clear();

    if (!ok) {
        m_faulted.store(true, std::memory_order_release);
        m_faultCode.store(exCode, std::memory_order_release);
        formatFaultReason(m_faultReasonBuf, "VST3 crash: ", exCode);
        m_faultReason.store(m_faultReasonBuf, std::memory_order_release);
        ctx.output.copyFrom(ctx.input);
    }
}

void Vst3PluginInstance::reset() {
    suspendAndFenceAudio();
    drainAllParamQueues();
    m_audioThreadParamCount = 0;
    m_inputParamChanges.clear();
    m_impl->processContext.projectTimeSamples = 0;
    if (m_processor) {
        m_processor->setProcessing(false);
        m_processor->setProcessing(true);
    }
    resumeAudio();
}

void Vst3PluginInstance::resetFault() noexcept {
    suspendAndFenceAudio();
    drainAllParamQueues();
    m_audioThreadParamCount = 0;
    m_inputParamChanges.clear();
    m_impl->processContext.projectTimeSamples = 0;
    if (m_processor) {
        safeCallPlugin([&]() {
            m_processor->setProcessing(false);
        });
    }
    if (m_component) {
        safeCallPlugin([&]() {
            m_component->setActive(false);
            m_component->setActive(true);
        });
    }
    if (m_processor) {
        safeCallPlugin([&]() {
            m_processor->setProcessing(true);
        });
    }
    IPluginInstance::resetFault();
    resumeAudio();
}

size_t Vst3PluginInstance::numParameters() const noexcept {
    const_cast<Vst3PluginInstance*>(this)->ensureControllerInitialized();
    std::lock_guard<std::mutex> lock(m_paramCacheMutex);
    return m_paramDescs.size();
}

PluginParameterDesc Vst3PluginInstance::getParameterDesc(size_t index) const {
    const_cast<Vst3PluginInstance*>(this)->ensureControllerInitialized();
    std::lock_guard<std::mutex> lock(m_paramCacheMutex);
    if (index < m_paramDescs.size()) {
        PluginParameterDesc desc = m_paramDescs[index];
        auto it = m_paramCache.find(desc.id);
        if (it != m_paramCache.end()) {
            desc.currentValue = it->second;
        }
        return desc;
    }
    return {};
}

void Vst3PluginInstance::setParameterValue(uint32_t paramId, float value) {
    if (m_faulted.load(std::memory_order_relaxed)) return;

    const float normVal = std::clamp(value, 0.0f, 1.0f);

    {
        std::lock_guard<std::mutex> lock(m_paramCacheMutex);
        m_paramCache[paramId] = normVal;
        auto idxIt = m_paramIdToIndex.find(paramId);
        if (idxIt != m_paramIdToIndex.end() && idxIt->second < m_paramDescs.size()) {
            m_paramDescs[idxIt->second].currentValue = normVal;
        }
    }

    // Queue parameter change into real-time buffer for audio-thread process()
    enqueueParamForAudio(static_cast<Steinberg::Vst::ParamID>(paramId), static_cast<double>(normVal));

    // Only invoke IEditController on GUI/STA host thread, NEVER from the high-priority ASIO audio thread!
    if (m_controller && GetCurrentThreadId() != m_audioThreadId.load(std::memory_order_relaxed)) {
        postToHostThread([this, paramId, normVal]() {
            if (!m_controller) return;
            m_suppressHandlerCallbacks.store(true, std::memory_order_relaxed);
            safeCallPlugin([&]() {
                m_controller->setParamNormalized(paramId, normVal);
            });
            m_suppressHandlerCallbacks.store(false, std::memory_order_relaxed);
        });
    }
}

void Vst3PluginInstance::setParameterFromAudioThread(uint32_t paramId, float value) noexcept {
    if (m_faulted.load(std::memory_order_relaxed)) return;
    const float normVal = std::clamp(value, 0.0f, 1.0f);
    const auto vid = static_cast<Steinberg::Vst::ParamID>(paramId);
    const double dval = static_cast<double>(normVal);
    // Real-time audio thread automation: write directly into audio-thread array (never touch SPSC queues)
    for (size_t i = 0; i < m_audioThreadParamCount; ++i) {
        if (m_audioThreadParamEvents[i].id == vid) {
            m_audioThreadParamEvents[i].value = dval;
            return;
        }
    }
    if (m_audioThreadParamCount < m_audioThreadParamEvents.size()) {
        m_audioThreadParamEvents[m_audioThreadParamCount++] = {vid, dval};
    }
}

float Vst3PluginInstance::getParameterValue(uint32_t paramId) const {
    if (m_faulted.load(std::memory_order_relaxed)) return 0.0f;
    const_cast<Vst3PluginInstance*>(this)->ensureControllerInitialized();
    std::lock_guard<std::mutex> lock(m_paramCacheMutex);
    auto it = m_paramCache.find(paramId);
    return (it != m_paramCache.end()) ? it->second : 0.0f;
}

bool Vst3PluginInstance::hasCustomGui() const noexcept {
    const_cast<Vst3PluginInstance*>(this)->ensureControllerInitialized();
    return m_controller != nullptr;
}

bool Vst3PluginInstance::openGuiInternal(HWND parentHwnd) {
    if (m_faulted.load(std::memory_order_acquire)) return false;
    ensureControllerInitialized();
    if (!m_controller || !parentHwnd) return false;

    DWORD exCode = 0;
    bool ok = safeCallPluginGui([&]() {
        if (!m_plugView) {
            m_plugView = m_controller->createView(Steinberg::Vst::ViewType::kEditor);
        }

        if (m_plugView && m_plugView->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) == Steinberg::kResultOk) {
            m_impl->plugFrame.parentHwnd.store(parentHwnd, std::memory_order_release);
            m_plugView->setFrame(&m_impl->plugFrame);

            if (m_plugView->attached(reinterpret_cast<void*>(parentHwnd), Steinberg::kPlatformTypeHWND) == Steinberg::kResultOk) {
                m_guiParentHwnd = parentHwnd;
                Steinberg::ViewRect rect{};
                if (m_plugView->getSize(&rect) == Steinberg::kResultOk) {
                    int w = rect.right - rect.left;
                    int h = rect.bottom - rect.top;
                    if (w > 100 && h > 100) {
                        m_preferredWidth = w;
                        m_preferredHeight = h;
                    }
                }
            } else {
                m_plugView->setFrame(nullptr);
            }
        }
    }, &exCode);

    if (!ok) {
        m_faulted.store(true, std::memory_order_release);
        m_faultCode.store(exCode, std::memory_order_release);
        formatFaultReason(m_faultReasonBuf, "VST3 GUI error: ", exCode);
        m_faultReason.store(m_faultReasonBuf, std::memory_order_release);
        return false;
    }

    return m_guiParentHwnd != nullptr;
}

bool Vst3PluginInstance::openGui(HWND parentHwnd) {
    if (m_faulted.load(std::memory_order_acquire)) return false;
    if (!parentHwnd) return false;

    const DWORD tid = m_hostThreadId.load(std::memory_order_acquire);
    if (tid == 0 || GetCurrentThreadId() == tid) {
        return openGuiInternal(parentHwnd);
    }

    HANDLE ack = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    PostThreadMessageW(tid, WM_USER + 101, reinterpret_cast<WPARAM>(ack), reinterpret_cast<LPARAM>(parentHwnd));

    const auto startTime = std::chrono::steady_clock::now();
    const DWORD timeoutMs = 30000;
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= timeoutMs) {
            break;
        }
        DWORD remainingMs = static_cast<DWORD>(timeoutMs - elapsed);
        DWORD waitRes = MsgWaitForMultipleObjectsEx(1, &ack, remainingMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waitRes == WAIT_OBJECT_0) {
            break;
        } else if (waitRes == WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        } else {
            break;
        }
    }
    CloseHandle(ack);
    return m_guiParentHwnd != nullptr;
}

void Vst3PluginInstance::openGuiAsync(HWND parentHwnd, std::function<void(bool success)> onComplete) {
    if (m_faulted.load(std::memory_order_acquire) || !parentHwnd) {
        if (onComplete) onComplete(false);
        return;
    }

    const DWORD tid = m_hostThreadId.load(std::memory_order_acquire);
    if (tid == 0 || GetCurrentThreadId() == tid) {
        bool ok = openGuiInternal(parentHwnd);
        if (onComplete) onComplete(ok);
        return;
    }

    auto* req = new AsyncGuiOpenReq{ parentHwnd, std::move(onComplete) };
    if (!PostThreadMessageW(tid, WM_USER + 104, reinterpret_cast<WPARAM>(req), 0)) {
        delete req;
        if (onComplete) onComplete(false);
    }
}

void Vst3PluginInstance::pollAsyncGuiResults() {
    AsyncGuiOpenResult res;
    while (m_asyncGuiResults.try_dequeue(res)) {
        if (res.callback) {
            res.callback(res.success);
        }
    }
}

void Vst3PluginInstance::getPreferredSize(int& width, int& height) const {
    if (m_preferredWidth > 0 && m_preferredHeight > 0) {
        width = m_preferredWidth;
        height = m_preferredHeight;
        return;
    }

    if (m_plugView) {
        invokeOnHostThread([this, &width, &height]() {
            if (!m_plugView) return;
            Steinberg::ViewRect rect{};
            if (m_plugView->getSize(&rect) == Steinberg::kResultOk) {
                int w = rect.right - rect.left;
                int h = rect.bottom - rect.top;
                if (w > 100 && h > 100) {
                    m_preferredWidth = w;
                    m_preferredHeight = h;
                    width = w;
                    height = h;
                }
            }
        });
        if (m_preferredWidth > 0 && m_preferredHeight > 0) {
            width = m_preferredWidth;
            height = m_preferredHeight;
            return;
        }
    }

    width = 850;
    height = 600;
}

void Vst3PluginInstance::onGuiResize(int width, int height) {
    if (m_impl && m_impl->plugFrame.inResize.load(std::memory_order_acquire)) {
        return;
    }
    if (m_plugView && width > 64 && height > 64 && !m_faulted.load(std::memory_order_relaxed)) {
        postToHostThread([this, width, height]() {
            if (!m_plugView || m_faulted.load(std::memory_order_relaxed)) return;
            safeCallPluginGui([&]() {
                Steinberg::ViewRect rect{0, 0, width, height};
                m_plugView->checkSizeConstraint(&rect);
                m_plugView->onSize(&rect);
                m_preferredWidth = rect.right - rect.left;
                m_preferredHeight = rect.bottom - rect.top;
            });
        });
    }
}

bool Vst3PluginInstance::adjustSize(int& width, int& height) {
    if (m_plugView && !m_faulted.load(std::memory_order_relaxed)) {
        Steinberg::ViewRect rect{0, 0, width, height};
        if (m_plugView->checkSizeConstraint(&rect) == Steinberg::kResultOk) {
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
            return true;
        }
    }
    return false;
}

float Vst3PluginInstance::getParameterValueByIndex(size_t index) const {
    std::lock_guard<std::mutex> lock(m_paramCacheMutex);
    if (index < m_paramDescs.size()) {
        return m_paramDescs[index].currentValue;
    }
    return 0.0f;
}

void Vst3PluginInstance::getAllParameterValues(std::vector<float>& out) const {
    std::lock_guard<std::mutex> lock(m_paramCacheMutex);
    out.resize(m_paramDescs.size());
    for (size_t i = 0; i < m_paramDescs.size(); ++i) {
        out[i] = m_paramDescs[i].currentValue;
    }
}

void Vst3PluginInstance::closeGuiInternal() {
    if (m_plugView) {
        safeCallPluginGui([&]() {
            m_plugView->removed();
            m_plugView->setFrame(nullptr);
            m_plugView->release();
        });
        m_plugView = nullptr;
        m_guiParentHwnd = nullptr;
        if (m_impl) {
            m_impl->plugFrame.detachHost();
        }
    }
}

void Vst3PluginInstance::closeGui() {
    const DWORD tid = m_hostThreadId.load(std::memory_order_acquire);
    if (tid == 0 || GetCurrentThreadId() == tid) {
        closeGuiInternal();
        return;
    }

    HANDLE ack = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    PostThreadMessageW(tid, WM_USER + 102, reinterpret_cast<WPARAM>(ack), 0);

    while (true) {
        DWORD waitRes = MsgWaitForMultipleObjectsEx(1, &ack, 5000, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waitRes == WAIT_OBJECT_0) {
            break;
        } else if (waitRes == WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        } else {
            break;
        }
    }
    CloseHandle(ack);
}

std::vector<uint8_t> Vst3PluginInstance::saveState() const {
    if (!m_component || m_faulted.load(std::memory_order_relaxed)) return {};
    std::vector<uint8_t> result;
    invokeOnHostThread([this, &result]() {
        if (!m_component || m_faulted.load(std::memory_order_relaxed)) return;
        safeCallPlugin([&]() {
            PraccyMemoryStream stream;
            if (m_component->getState(&stream) == Steinberg::kResultOk) {
                result = std::vector<uint8_t>(stream.buffer.begin(), stream.buffer.end());
            }
        });
    });
    if (!result.empty()) {
        updateStateHash(result);
    }
    return result;
}

bool Vst3PluginInstance::loadState(const std::vector<uint8_t>& state) {
    if (!m_component || state.empty() || m_faulted.load(std::memory_order_relaxed)) return false;

    suspendAndFenceAudio();

    // Drain any stale pending parameter changes while audio thread is fenced
    drainAllParamQueues();
    m_audioThreadParamCount = 0;
    m_inputParamChanges.clear();

    bool success = false;
    invokeOnHostThread([this, &state, &success]() {
        m_suppressHandlerCallbacks.store(true, std::memory_order_relaxed);
        safeCallPlugin([&]() {
            PraccyMemoryStream stream(state);
            if (m_component->setState(&stream) == Steinberg::kResultOk) {
                if (m_controller) {
                    stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
                    m_controller->setComponentState(&stream);
                }
                success = true;
            }
        });
        m_suppressHandlerCallbacks.store(false, std::memory_order_relaxed);

        if (success && m_controller) {
            std::lock_guard<std::mutex> lock(m_paramCacheMutex);
            for (auto& desc : m_paramDescs) {
                const double normVal = m_controller->getParamNormalized(desc.id);
                const float fVal = static_cast<float>(normVal);
                m_paramCache[desc.id] = fVal;
                desc.currentValue = fVal;
                enqueueParamForAudio(desc.id, normVal);
            }
        }
    });
    resumeAudio();

    if (success) {
        updateStateHash(state);
    }
    clearGuiParamEdit();
    return success;
}

} // namespace praccy::plugins
