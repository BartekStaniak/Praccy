#include "vst3_host.h"
#include <filesystem>
#include <iostream>

namespace Steinberg {
DEF_CLASS_IID (IPlugView)
namespace Vst {
DEF_CLASS_IID (IComponent)
DEF_CLASS_IID (IAudioProcessor)
DEF_CLASS_IID (IEditController)
}
}

namespace praccy::plugins {

using InitDllProc = bool (PLUGIN_API *)();
using ExitDllProc = bool (PLUGIN_API *)();
using GetFactoryProc = Steinberg::IPluginFactory* (PLUGIN_API *)();

std::unique_ptr<Vst3PluginInstance> Vst3PluginInstance::loadFromFile(const std::string& path) {
    std::filesystem::path p(path);
    std::filesystem::path dllPath = p;

    // Check if it's a VST3 bundle directory (Windows 10/11 VST3 standard layout)
    if (std::filesystem::is_directory(p)) {
        auto candidate = p / "Contents" / "x86_64-win" / (p.stem().string() + ".vst3");
        if (std::filesystem::exists(candidate)) {
            dllPath = candidate;
        } else {
            // Check for any .vst3 or .dll inside Contents/x86_64-win
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

    auto* initDll = reinterpret_cast<InitDllProc>(GetProcAddress(hLib, "InitDll"));
    if (initDll) {
        initDll();
    }

    auto* getFactory = reinterpret_cast<GetFactoryProc>(GetProcAddress(hLib, "GetPluginFactory"));
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
    instance->m_name = classInfo.name;
    instance->m_vendor = "VST3 Plugin";

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
    if (instance->m_component->getControllerClassId(controllerCid) == Steinberg::kResultOk && controllerCid[0] != 0) {
        factory->createInstance(controllerCid, Steinberg::Vst::IEditController::iid, reinterpret_cast<void**>(&instance->m_controller));
    }
    if (!instance->m_controller) {
        instance->m_component->queryInterface(Steinberg::Vst::IEditController::iid, reinterpret_cast<void**>(&instance->m_controller));
    }

    if (instance->m_controller) {
        instance->m_controller->initialize(nullptr);
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
        auto* exitDll = reinterpret_cast<ExitDllProc>(GetProcAddress(m_module, "ExitDll"));
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
    if (!m_processor || ctx.numSamples == 0) {
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

    m_processor->process(data);
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
    if (m_controller) {
        m_controller->setParamNormalized(paramId, std::clamp(value, 0.0f, 1.0f));
    }
}

float Vst3PluginInstance::getParameterValue(uint32_t paramId) const {
    if (m_controller) {
        return static_cast<float>(m_controller->getParamNormalized(paramId));
    }
    return 0.0f;
}

bool Vst3PluginInstance::hasCustomGui() const noexcept {
    return m_controller != nullptr;
}

bool Vst3PluginInstance::openGui(HWND parentHwnd) {
    if (!m_controller || !parentHwnd) return false;

    if (!m_plugView) {
        m_plugView = m_controller->createView(Steinberg::Vst::ViewType::kEditor);
    }

    if (!m_plugView) return false;

    if (m_plugView->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != Steinberg::kResultOk) {
        return false;
    }

    m_guiParentHwnd = parentHwnd;
    return (m_plugView->attached(reinterpret_cast<void*>(parentHwnd), Steinberg::kPlatformTypeHWND) == Steinberg::kResultOk);
}

void Vst3PluginInstance::closeGui() {
    if (m_plugView) {
        m_plugView->removed();
        m_plugView->release();
        m_plugView = nullptr;
        m_guiParentHwnd = nullptr;
    }
}

std::vector<uint8_t> Vst3PluginInstance::saveState() const {
    return {};
}

bool Vst3PluginInstance::loadState(const std::vector<uint8_t>& state) {
    return true;
}

} // namespace praccy::plugins
