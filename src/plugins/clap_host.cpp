#include "clap_host.h"
#include "crash_isolation.h"
#include <iostream>
#include <sstream>

namespace praccy::plugins {

const void* ClapPluginInstance::clapHostGetExtension(const clap_host* host, const char* extensionId) {
    return nullptr;
}

void ClapPluginInstance::clapHostRequestRestart(const clap_host* host) {}
void ClapPluginInstance::clapHostRequestProcess(const clap_host* host) {}
void ClapPluginInstance::clapHostRequestCallback(const clap_host* host) {}

std::unique_ptr<ClapPluginInstance> ClapPluginInstance::loadFromFile(const std::string& path, uint32_t pluginIndex) {
    HMODULE hLib = LoadLibraryA(path.c_str());
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

    auto instance = std::unique_ptr<ClapPluginInstance>(new ClapPluginInstance());
    instance->m_libraryModule = hLib;
    instance->m_entry = entry;
    instance->m_path = path;
    instance->m_name = desc->name ? desc->name : "CLAP Plugin";
    instance->m_vendor = desc->vendor ? desc->vendor : "Unknown";
    instance->m_version = desc->version ? desc->version : "1.0.0";

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

    if (instance->m_latencyExt) {
        instance->m_latencySamples = instance->m_latencyExt->get(instance->m_plugin);
    }

    return instance;
}

ClapPluginInstance::~ClapPluginInstance() {
    closeGui();
    if (m_plugin) {
        m_plugin->deactivate(m_plugin);
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
    if (m_plugin) {
        m_plugin->activate(m_plugin, sampleRate, 1, maxBlockSize);
        if (m_latencyExt) {
            m_latencySamples = m_latencyExt->get(m_plugin);
        }
    }
}

void ClapPluginInstance::process(audio::AudioProcessContext& ctx) {
    if (!m_plugin || ctx.numSamples == 0 || m_faulted.load(std::memory_order_relaxed)) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    const uint32_t numSamples = ctx.numSamples;
    const uint32_t inCh = ctx.input.numChannels();
    const uint32_t outCh = ctx.output.numChannels();

    clap_audio_buffer inBuf{};
    inBuf.data32 = const_cast<float**>(ctx.input.rawChannels());
    inBuf.channel_count = inCh;
    inBuf.latency = 0;
    inBuf.constant_mask = 0;

    clap_audio_buffer outBuf{};
    outBuf.data32 = const_cast<float**>(ctx.output.rawChannels());
    outBuf.channel_count = outCh;
    outBuf.latency = 0;
    outBuf.constant_mask = 0;

    clap_process processData{};
    processData.steady_time = -1;
    processData.frames_count = numSamples;
    processData.transport = nullptr;
    processData.audio_inputs = &inBuf;
    processData.audio_outputs = &outBuf;
    processData.audio_inputs_count = 1;
    processData.audio_outputs_count = 1;
    processData.in_events = nullptr;
    processData.out_events = nullptr;

    DWORD exCode = 0;
    bool ok = safeCallPluginAudio([&]() {
        m_plugin->process(m_plugin, &processData);
    }, &exCode);

    if (!ok) {
        m_faulted.store(true, std::memory_order_release);
        setFaultReason(getExceptionDescription(exCode));
        ctx.output.copyFrom(ctx.input);
    }
}

void ClapPluginInstance::reset() {
    if (m_plugin && !m_faulted.load(std::memory_order_relaxed)) {
        safeCallPlugin([&]() {
            m_plugin->reset(m_plugin);
        });
    }
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
                double val = 0.0;
                if (m_paramsExt->get_value(m_plugin, info.id, &val)) {
                    desc.currentValue = static_cast<float>(val);
                }
            }
        });
    }
    return desc;
}

void ClapPluginInstance::setParameterValue(uint32_t paramId, float value) {
    // CLAP parameters in realtime are modulated via events or direct call
}

float ClapPluginInstance::getParameterValue(uint32_t paramId) const {
    if (m_paramsExt && m_plugin && !m_faulted.load(std::memory_order_relaxed)) {
        float result = 0.0f;
        safeCallPlugin([&]() {
            double val = 0.0;
            if (m_paramsExt->get_value(m_plugin, paramId, &val)) {
                result = static_cast<float>(val);
            }
        });
        return result;
    }
    return 0.0f;
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
        setFaultReason(getExceptionDescription(exCode));
        return false;
    }

    return opened;
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
    if (m_guiExt && m_plugin && !m_faulted.load(std::memory_order_relaxed)) {
        safeCallPlugin([&]() {
            uint32_t w = 850, h = 600;
            if (m_guiExt->get_size(m_plugin, &w, &h)) {
                if (w > 100 && h > 100) {
                    width = static_cast<int>(w);
                    height = static_cast<int>(h);
                }
            }
        });
    }
}

std::vector<uint8_t> ClapPluginInstance::saveState() const {
    return {};
}

bool ClapPluginInstance::loadState(const std::vector<uint8_t>& state) {
    return true;
}

} // namespace praccy::plugins
