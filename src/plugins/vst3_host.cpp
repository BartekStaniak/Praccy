#include "vst3_host.h"
#include "crash_isolation.h"
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <vector>

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/vst/ivstmessage.h>

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

    Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
    Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32) override { return Steinberg::kResultOk; }
};

class PraccyPlugFrame : public Steinberg::IPlugFrame {
public:
    HWND parentHwnd{nullptr};

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
        return Steinberg::kResultOk;
    }
};

struct Vst3PluginInstance::Impl {
    PraccyComponentHandler componentHandler;
    PraccyPlugFrame plugFrame;
};

Vst3PluginInstance::Vst3PluginInstance() : m_impl(std::make_unique<Impl>()) {}

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
        PraccyMemoryStream stateStream;
        if (instance->m_component->getState(&stateStream) == Steinberg::kResultOk) {
            stateStream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
            instance->m_controller->setComponentState(&stateStream);
        }
    }

    return instance;
}

Vst3PluginInstance::~Vst3PluginInstance() {
    closeGui();
    if (m_processor) {
        m_processor->setProcessing(false);
    }
    if (m_component) {
        m_component->setActive(false);
        m_component->terminate();
    }
    if (m_controller) {
        m_controller->terminate();
        m_controller->release();
        m_controller = nullptr;
    }
    if (m_processor) {
        m_processor->release();
        m_processor = nullptr;
    }
    if (m_component) {
        m_component->release();
        m_component = nullptr;
    }
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
}

void Vst3PluginInstance::process(audio::AudioProcessContext& ctx) {
    if (!m_processor || ctx.numSamples == 0 || m_faulted.load(std::memory_order_relaxed)) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    const uint32_t numSamples = ctx.numSamples;
    Steinberg::Vst::ProcessData data{};
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = numSamples;

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

    DWORD exCode = 0;
    bool ok = safeCallPluginAudio([&]() {
        m_processor->process(data);
    }, &exCode);

    if (!ok) {
        m_faulted.store(true, std::memory_order_release);
        setFaultReason(getExceptionDescription(exCode));
        ctx.output.copyFrom(ctx.input);
    }
}

void Vst3PluginInstance::reset() {
    if (m_processor) {
        m_processor->setProcessing(false);
        m_processor->setProcessing(true);
    }
}

size_t Vst3PluginInstance::numParameters() const noexcept {
    if (m_controller) {
        return static_cast<size_t>(m_controller->getParameterCount());
    }
    return 0;
}

PluginParameterDesc Vst3PluginInstance::getParameterDesc(size_t index) const {
    PluginParameterDesc desc;
    if (m_controller) {
        Steinberg::Vst::ParameterInfo info{};
        if (m_controller->getParameterInfo(static_cast<int32_t>(index), info) == Steinberg::kResultOk) {
            desc.id = info.id;
            char nameBuf[128] = {0};
            for (int i = 0; i < 127 && info.title[i] != 0; ++i) {
                nameBuf[i] = static_cast<char>(info.title[i]);
            }
            desc.name = nameBuf;
            desc.minValue = 0.0f;
            desc.maxValue = 1.0f;
            desc.defaultValue = static_cast<float>(info.defaultNormalizedValue);
            desc.currentValue = static_cast<float>(m_controller->getParamNormalized(info.id));
        }
    }
    return desc;
}

void Vst3PluginInstance::setParameterValue(uint32_t paramId, float value) {
    if (m_controller && !m_faulted.load(std::memory_order_relaxed)) {
        safeCallPlugin([&]() {
            m_controller->setParamNormalized(paramId, std::clamp(value, 0.0f, 1.0f));
        });
    }
}

float Vst3PluginInstance::getParameterValue(uint32_t paramId) const {
    if (!m_controller || m_faulted.load(std::memory_order_relaxed)) return 0.0f;
    float val = 0.0f;
    safeCallPlugin([&]() {
        val = static_cast<float>(m_controller->getParamNormalized(paramId));
    });
    return val;
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
    if (m_controller && !m_faulted.load(std::memory_order_relaxed)) {
        Steinberg::IPlugView* view = m_plugView;
        bool createdTemp = false;
        if (!view) {
            view = m_controller->createView(Steinberg::Vst::ViewType::kEditor);
            createdTemp = true;
        }
        if (view) {
            Steinberg::ViewRect rect{};
            if (view->getSize(&rect) == Steinberg::kResultOk) {
                int w = rect.right - rect.left;
                int h = rect.bottom - rect.top;
                if (w > 100 && h > 100) {
                    width = w;
                    height = h;
                }
            }
            if (createdTemp) {
                view->release();
            }
        }
    }
}

void Vst3PluginInstance::closeGui() {
    if (m_plugView) {
        safeCallPluginGui([&]() {
            m_plugView->setFrame(nullptr);
            m_plugView->removed();
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
    bool success = false;
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
    return success;
}

} // namespace praccy::plugins
