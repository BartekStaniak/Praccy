#include "vst3_host.h"
#include "crash_isolation.h"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <vector>

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

using InitDllProc = bool (PLUGIN_API *)();
using ExitDllProc = bool (PLUGIN_API *)();
using GetFactoryProc = Steinberg::IPluginFactory* (PLUGIN_API *)();

class PraccyMemoryStream : public Steinberg::IBStream {
public:
    std::vector<char> buffer;
    Steinberg::int64 pos{0};

    PraccyMemoryStream() = default;
    explicit PraccyMemoryStream(const std::vector<uint8_t>& data) {
        buffer.assign(data.begin(), data.end());
    }

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::IBStream::iid, sizeof(Steinberg::TUID)) == 0 ||
            memcmp(_iid, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::tresult PLUGIN_API read(void* dest, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override {
        if (!dest || numBytes < 0) return Steinberg::kInvalidArgument;
        Steinberg::int32 available = static_cast<Steinberg::int32>(buffer.size() - pos);
        Steinberg::int32 toRead = (std::min)(numBytes, available);
        if (toRead > 0) {
            memcpy(dest, buffer.data() + pos, toRead);
            pos += toRead;
        }
        if (numBytesRead) *numBytesRead = toRead;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API write(void* src, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten) override {
        if (!src || numBytes < 0) return Steinberg::kInvalidArgument;
        if (pos + numBytes > static_cast<Steinberg::int64>(buffer.size())) {
            buffer.resize(pos + numBytes);
        }
        memcpy(buffer.data() + pos, src, numBytes);
        pos += numBytes;
        if (numBytesWritten) *numBytesWritten = numBytes;
        return Steinberg::kResultOk;
    }

    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 offset, Steinberg::int32 mode, Steinberg::int64* result) override {
        if (mode == kIBSeekSet) pos = offset;
        else if (mode == kIBSeekCur) pos += offset;
        else if (mode == kIBSeekEnd) pos = buffer.size() + offset;
        if (pos < 0) pos = 0;
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
    Vst3PluginInstance* owner{nullptr};

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::Vst::IComponentHandler::iid, sizeof(Steinberg::TUID)) == 0 ||
            memcmp(_iid, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override {
        if (owner) owner->onGuiBeginEdit(id);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue valueNormalized) override {
        if (owner) owner->onGuiPerformEdit(id, valueNormalized);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override {
        if (owner) owner->onGuiEndEdit(id);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override {
        if (owner) owner->onGuiRestartComponent(flags);
        return Steinberg::kResultOk;
    }
};

class PraccyPlugFrame : public Steinberg::IPlugFrame {
public:
    HWND parentHwnd{nullptr};
    bool inResize{false};

    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (memcmp(_iid, Steinberg::IPlugFrame::iid, sizeof(Steinberg::TUID)) == 0 ||
            memcmp(_iid, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0) {
            *obj = this;
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override {
        if (!view || !newSize) return Steinberg::kInvalidArgument;
        if (inResize) return Steinberg::kResultOk;
        inResize = true;
        if (parentHwnd && IsWindow(parentHwnd)) {
            int newW = newSize->right - newSize->left;
            int newH = newSize->bottom - newSize->top;
            RECT wr = {0, 0, newW, newH};
            AdjustWindowRectEx(&wr, GetWindowLongW(parentHwnd, GWL_STYLE), FALSE, GetWindowLongW(parentHwnd, GWL_EXSTYLE));
            int totalW = wr.right - wr.left;
            int totalH = wr.bottom - wr.top;
            SetWindowPos(parentHwnd, nullptr, 0, 0, totalW, totalH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            view->onSize(newSize);
        }
        inResize = false;
        return Steinberg::kResultOk;
    }
};

struct Vst3PluginInstance::Impl {
    PraccyComponentHandler componentHandler;
    PraccyPlugFrame plugFrame;
    Steinberg::Vst::ProcessContext processContext{};
};

Vst3PluginInstance::Vst3PluginInstance() : m_impl(std::make_unique<Impl>()) {
    m_impl->componentHandler.owner = this;
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

    // Forward GUI parameter edit to audio processor queue
    m_paramQueue.try_enqueue({paramId, static_cast<double>(normVal)});
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
                m_paramQueue.try_enqueue({desc.id, normVal});
            }
        }
        setGuiEditingActive(false);
        notifyGuiParamEdited(0xFFFFFFFFu, true);
    }
}

std::unique_ptr<Vst3PluginInstance> Vst3PluginInstance::loadFromFile(const std::string& path) {
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

    HMODULE hLib = LoadLibraryW(dllPath.wstring().c_str());
    if (!hLib) {
        std::cerr << "Failed to LoadLibrary for VST3: " << dllPath.string() << "\n";
        return nullptr;
    }

    auto* initDll = reinterpret_cast<InitDllProc>(reinterpret_cast<void*>(GetProcAddress(hLib, "InitDll")));
    if (initDll) {
        initDll();
    }

    auto* getFactory = reinterpret_cast<GetFactoryProc>(reinterpret_cast<void*>(GetProcAddress(hLib, "GetPluginFactory")));
    if (!getFactory) {
        std::cerr << "GetPluginFactory not found in: " << dllPath.string() << "\n";
        FreeLibrary(hLib);
        return nullptr;
    }

    Steinberg::IPluginFactory* factory = getFactory();
    if (!factory) {
        std::cerr << "GetPluginFactory returned nullptr in: " << dllPath.string() << "\n";
        FreeLibrary(hLib);
        return nullptr;
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
        FreeLibrary(hLib);
        return nullptr;
    }

    auto instance = std::unique_ptr<Vst3PluginInstance>(new Vst3PluginInstance());
    instance->m_module = hLib;
    instance->m_factory = factory;
    instance->m_path = path;
    instance->m_bundleFileName = std::filesystem::path(path).filename().string();
    Steinberg::FUID fuid = Steinberg::FUID::fromTUID(classInfo.cid);
    char guidStr[33] = {0};
    fuid.toString(guidStr);
    instance->m_classGuid = guidStr;
    instance->m_name = classInfo.name;
    instance->m_vendor = "VST3 Plugin";

    // Query Vendor from factory info if available
    Steinberg::PFactoryInfo factoryInfo{};
    if (factory->getFactoryInfo(&factoryInfo) == Steinberg::kResultOk) {
        if (factoryInfo.vendor[0] != '\0') {
            instance->m_vendor = factoryInfo.vendor;
        }
    }

    // Create Component Instance
    if (factory->createInstance(classInfo.cid, Steinberg::Vst::IComponent::iid, reinterpret_cast<void**>(&instance->m_component)) != Steinberg::kResultOk || !instance->m_component) {
        std::cerr << "Failed to create IComponent for: " << instance->m_name << "\n";
        FreeLibrary(hLib);
        return nullptr;
    }

    // Query Audio Processor
    if (instance->m_component->queryInterface(Steinberg::Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&instance->m_processor)) != Steinberg::kResultOk || !instance->m_processor) {
        std::cerr << "Failed to query IAudioProcessor for: " << instance->m_name << "\n";
        instance->m_component->release();
        FreeLibrary(hLib);
        return nullptr;
    }

    instance->m_component->initialize(nullptr);

    // Create / Query Edit Controller
    Steinberg::TUID controllerCid{};
    if (instance->m_component->getControllerClassId(controllerCid) == Steinberg::kResultOk) {
        factory->createInstance(controllerCid, Steinberg::Vst::IEditController::iid, reinterpret_cast<void**>(&instance->m_controller));
    }
    if (!instance->m_controller) {
        instance->m_component->queryInterface(Steinberg::Vst::IEditController::iid, reinterpret_cast<void**>(&instance->m_controller));
    }
    if (!instance->m_controller) {
        for (int32_t i = 0; i < classCount; ++i) {
            Steinberg::PClassInfo cInfo{};
            if (factory->getClassInfo(i, &cInfo) == Steinberg::kResultOk) {
                if (std::strcmp(cInfo.category, kVstComponentControllerClass) == 0) {
                    factory->createInstance(cInfo.cid, Steinberg::Vst::IEditController::iid, reinterpret_cast<void**>(&instance->m_controller));
                    if (instance->m_controller) break;
                }
            }
        }
    }

    if (instance->m_controller) {
        instance->m_controller->initialize(nullptr);
        instance->m_controller->setComponentHandler(&instance->m_impl->componentHandler);

        // Connect IConnectionPoint between component and controller
        Steinberg::Vst::IConnectionPoint* cpComp = nullptr;
        Steinberg::Vst::IConnectionPoint* cpCtrl = nullptr;
        instance->m_component->queryInterface(Steinberg::Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&cpComp));
        instance->m_controller->queryInterface(Steinberg::Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&cpCtrl));
        if (cpComp && cpCtrl) {
            cpComp->connect(cpCtrl);
            cpCtrl->connect(cpComp);
        }
        if (cpComp) cpComp->release();
        if (cpCtrl) cpCtrl->release();

        // Sync initial state from component to controller
        instance->m_suppressHandlerCallbacks.store(true, std::memory_order_relaxed);
        PraccyMemoryStream stateStream;
        if (instance->m_component->getState(&stateStream) == Steinberg::kResultOk) {
            stateStream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            instance->m_controller->setComponentState(&stateStream);
        }
        instance->m_suppressHandlerCallbacks.store(false, std::memory_order_relaxed);

        // Populate initial parameter cache and parameter descriptors
        const int32_t pCount = instance->m_controller->getParameterCount();
        instance->m_paramDescs.reserve(pCount);
        instance->m_paramIdToIndex.reserve(pCount);
        instance->m_paramCache.reserve(pCount);
        for (int32_t i = 0; i < pCount; ++i) {
            Steinberg::Vst::ParameterInfo info{};
            if (instance->m_controller->getParameterInfo(i, info) == Steinberg::kResultOk) {
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
                float curVal = static_cast<float>(instance->m_controller->getParamNormalized(info.id));
                desc.currentValue = curVal;

                instance->m_paramIdToIndex[info.id] = instance->m_paramDescs.size();
                instance->m_paramDescs.push_back(std::move(desc));
                instance->m_paramCache[info.id] = curVal;
            }
        }
    }

    instance->m_impl->processContext.state = Steinberg::Vst::ProcessContext::kPlaying |
                                           Steinberg::Vst::ProcessContext::kTempoValid |
                                           Steinberg::Vst::ProcessContext::kTimeSigValid;
    instance->m_impl->processContext.sampleRate = instance->m_sampleRate;
    instance->m_impl->processContext.tempo = 120.0;
    instance->m_impl->processContext.timeSigNumerator = 4;
    instance->m_impl->processContext.timeSigDenominator = 4;
    instance->m_impl->processContext.projectTimeSamples = 0;

    return instance;
}

Vst3PluginInstance::~Vst3PluginInstance() {
    closeGui();

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

    // 8. Cleanly invoke ExitDll and unload module
    if (m_module) {
        auto* exitDll = reinterpret_cast<ExitDllProc>(reinterpret_cast<void*>(GetProcAddress(m_module, "ExitDll")));
        if (exitDll) exitDll();
        FreeLibrary(m_module);
        m_module = nullptr;
    }
}

void Vst3PluginInstance::prepare(double sampleRate, uint32_t maxBlockSize) {
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;

    if (!m_component || !m_processor) return;

    m_component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, 0, true);
    m_component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, 0, true);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.sampleRate = sampleRate;

    m_processor->setupProcessing(setup);
    m_component->setActive(true);
    m_processor->setProcessing(true);

    m_latencySamples = m_processor->getLatencySamples();

    m_impl->processContext.state = Steinberg::Vst::ProcessContext::kPlaying |
                                   Steinberg::Vst::ProcessContext::kTempoValid |
                                   Steinberg::Vst::ProcessContext::kTimeSigValid;
    m_impl->processContext.sampleRate = sampleRate;
    m_impl->processContext.tempo = 120.0;
    m_impl->processContext.timeSigNumerator = 4;
    m_impl->processContext.timeSigDenominator = 4;
    m_impl->processContext.projectTimeSamples = 0;
}

void Vst3PluginInstance::process(audio::AudioProcessContext& ctx) {
    if (!m_processor || ctx.numSamples == 0 || m_faulted.load(std::memory_order_relaxed)) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    m_audioThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

    m_inputParamChanges.clear();
    std::pair<Steinberg::Vst::ParamID, double> paramChange;
    while (m_paramQueue.try_dequeue(paramChange)) {
        m_inputParamChanges.addParameterChange(paramChange.first, paramChange.second);
    }

    const uint32_t numSamples = ctx.numSamples;
    Steinberg::Vst::ProcessData data{};
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = numSamples;

    m_impl->processContext.sampleRate = ctx.sampleRate > 0.0 ? ctx.sampleRate : m_sampleRate;
    data.processContext = &m_impl->processContext;

    Steinberg::Vst::AudioBusBuffers inBus{};
    inBus.numChannels = ctx.input.numChannels();
    inBus.channelBuffers32 = const_cast<float**>(ctx.input.rawChannels());

    Steinberg::Vst::AudioBusBuffers outBus{};
    outBus.numChannels = ctx.output.numChannels();
    outBus.channelBuffers32 = const_cast<float**>(ctx.output.rawChannels());

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
        setFaultReason(getExceptionDescription(exCode));
        ctx.output.copyFrom(ctx.input);
    }
}

void Vst3PluginInstance::reset() {
    std::pair<Steinberg::Vst::ParamID, double> dummy;
    while (m_paramQueue.try_dequeue(dummy)) {}
    m_inputParamChanges.clear();
    m_impl->processContext.projectTimeSamples = 0;
    if (m_processor) {
        m_processor->setProcessing(false);
        m_processor->setProcessing(true);
    }
}

size_t Vst3PluginInstance::numParameters() const noexcept {
    std::lock_guard<std::mutex> lock(m_paramCacheMutex);
    return m_paramDescs.size();
}

PluginParameterDesc Vst3PluginInstance::getParameterDesc(size_t index) const {
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
    m_paramQueue.try_enqueue({static_cast<Steinberg::Vst::ParamID>(paramId), static_cast<double>(normVal)});

    // Only invoke IEditController on GUI/message thread, NEVER from the high-priority ASIO audio thread!
    if (m_controller && GetCurrentThreadId() != m_audioThreadId.load(std::memory_order_relaxed)) {
        m_suppressHandlerCallbacks.store(true, std::memory_order_relaxed);
        safeCallPlugin([&]() {
            m_controller->setParamNormalized(paramId, normVal);
        });
        m_suppressHandlerCallbacks.store(false, std::memory_order_relaxed);
    }
}

void Vst3PluginInstance::setParameterFromAudioThread(uint32_t paramId, float value) noexcept {
    if (m_faulted.load(std::memory_order_relaxed)) return;
    const float normVal = std::clamp(value, 0.0f, 1.0f);
    // Directly enqueue to param queue without touching m_controller
    m_paramQueue.try_enqueue({static_cast<Steinberg::Vst::ParamID>(paramId), static_cast<double>(normVal)});
}

float Vst3PluginInstance::getParameterValue(uint32_t paramId) const {
    if (m_faulted.load(std::memory_order_relaxed)) return 0.0f;
    std::lock_guard<std::mutex> lock(m_paramCacheMutex);
    auto it = m_paramCache.find(paramId);
    return (it != m_paramCache.end()) ? it->second : 0.0f;
}

bool Vst3PluginInstance::hasCustomGui() const noexcept {
    return m_controller != nullptr;
}

bool Vst3PluginInstance::openGui(HWND parentHwnd) {
    if (m_faulted.load(std::memory_order_acquire)) return false;
    if (!m_controller || !parentHwnd) return false;

    DWORD exCode = 0;
    bool ok = safeCallPluginGui([&]() {
        if (!m_plugView) {
            m_plugView = m_controller->createView(Steinberg::Vst::ViewType::kEditor);
        }

        if (m_plugView && m_plugView->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) == Steinberg::kResultOk) {
            m_impl->plugFrame.parentHwnd = parentHwnd;
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
        setFaultReason(getExceptionDescription(exCode));
        return false;
    }

    return m_guiParentHwnd != nullptr;
}

void Vst3PluginInstance::getPreferredSize(int& width, int& height) const {
    if (m_preferredWidth > 0 && m_preferredHeight > 0) {
        width = m_preferredWidth;
        height = m_preferredHeight;
        return;
    }

    if (m_plugView) {
        Steinberg::ViewRect rect{};
        if (m_plugView->getSize(&rect) == Steinberg::kResultOk) {
            int w = rect.right - rect.left;
            int h = rect.bottom - rect.top;
            if (w > 100 && h > 100) {
                m_preferredWidth = w;
                m_preferredHeight = h;
                width = w;
                height = h;
                return;
            }
        }
    }

    width = 850;
    height = 600;
}

void Vst3PluginInstance::closeGui() {
    if (m_plugView) {
        safeCallPluginGui([&]() {
            m_plugView->removed();
            m_plugView->setFrame(nullptr);
            m_plugView->release();
        });
        m_plugView = nullptr;
        m_guiParentHwnd = nullptr;
        m_impl->plugFrame.parentHwnd = nullptr;
    }
}

std::vector<uint8_t> Vst3PluginInstance::saveState() const {
    if (!m_component || m_faulted.load(std::memory_order_relaxed)) return {};
    std::vector<uint8_t> result;
    safeCallPlugin([&]() {
        PraccyMemoryStream stream;
        if (m_component->getState(&stream) == Steinberg::kResultOk) {
            result = std::vector<uint8_t>(stream.buffer.begin(), stream.buffer.end());
        }
    });
    return result;
}

bool Vst3PluginInstance::loadState(const std::vector<uint8_t>& state) {
    if (!m_component || state.empty() || m_faulted.load(std::memory_order_relaxed)) return false;

    // Drain any stale pending parameter changes so they don't overwrite the restored state
    std::pair<Steinberg::Vst::ParamID, double> dummy;
    while (m_paramQueue.try_dequeue(dummy)) {}

    bool success = false;
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
            m_paramQueue.try_enqueue({desc.id, normVal});
        }
    }
    clearGuiParamEdit();
    return success;
}

} // namespace praccy::plugins
