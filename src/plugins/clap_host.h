#pragma once

#include "plugin_base.h"
#include <clap/clap.h>
#include <memory>
#include <vector>
#include <string>

namespace praccy::plugins {

class ClapPluginInstance : public IPluginInstance {
public:
    static std::unique_ptr<ClapPluginInstance> loadFromFile(const std::string& path, uint32_t pluginIndex = 0);

    ~ClapPluginInstance() override;

    void prepare(double sampleRate, uint32_t maxBlockSize) override;
    void process(audio::AudioProcessContext& ctx) override;
    void reset() override;

    [[nodiscard]] audio::NodeType type() const noexcept override { return audio::NodeType::Plugin; }
    [[nodiscard]] const std::string& name() const noexcept override { return m_name; }
    [[nodiscard]] const std::string& vendor() const noexcept override { return m_vendor; }
    [[nodiscard]] const std::string& version() const noexcept override { return m_version; }

    [[nodiscard]] size_t numParameters() const noexcept override;
    [[nodiscard]] PluginParameterDesc getParameterDesc(size_t index) const override;
    void setParameterValue(uint32_t paramId, float value) override;
    [[nodiscard]] float getParameterValue(uint32_t paramId) const override;

    [[nodiscard]] bool hasCustomGui() const noexcept override;
    bool openGui(HWND parentHwnd) override;
    void closeGui() override;

    [[nodiscard]] std::vector<uint8_t> saveState() const override;
    bool loadState(const std::vector<uint8_t>& state) override;

private:
    ClapPluginInstance() = default;

    static const void* clapHostGetExtension(const clap_host* host, const char* extensionId);
    static void clapHostRequestRestart(const clap_host* host);
    static void clapHostRequestProcess(const clap_host* host);
    static void clapHostRequestCallback(const clap_host* host);

    HMODULE m_libraryModule{nullptr};
    const clap_plugin_entry* m_entry{nullptr};
    const clap_plugin* m_plugin{nullptr};
    clap_host m_host{};

    const clap_plugin_gui* m_guiExt{nullptr};
    const clap_plugin_params* m_paramsExt{nullptr};
    const clap_plugin_state* m_stateExt{nullptr};
    const clap_plugin_latency* m_latencyExt{nullptr};

    std::string m_name{"CLAP Plugin"};
    std::string m_vendor{"Unknown"};
    std::string m_version{"1.0.0"};

    HWND m_guiParentHwnd{nullptr};
    bool m_guiCreated{false};
};

} // namespace praccy::plugins
