#pragma once

#include "plugin_base.h"
#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/gui/iplugview.h>

#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace praccy::plugins {

class Vst3PluginInstance : public IPluginInstance {
public:
    static std::unique_ptr<Vst3PluginInstance> loadFromFile(const std::string& path);

    ~Vst3PluginInstance() override;

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

    [[nodiscard]] Steinberg::IPlugView* plugView() const noexcept { return m_plugView; }

private:
    Vst3PluginInstance() = default;

    HMODULE m_module{nullptr};
    Steinberg::IPluginFactory* m_factory{nullptr};
    Steinberg::Vst::IComponent* m_component{nullptr};
    Steinberg::Vst::IAudioProcessor* m_processor{nullptr};
    Steinberg::Vst::IEditController* m_controller{nullptr};
    Steinberg::IPlugView* m_plugView{nullptr};

    std::string m_name{"VST3 Plugin"};
    std::string m_vendor{"Steinberg VST3"};
    std::string m_version{"1.0.0"};

    HWND m_guiParentHwnd{nullptr};
    double m_sampleRate{48000.0};
    uint32_t m_maxBlockSize{256};
};

} // namespace praccy::plugins
