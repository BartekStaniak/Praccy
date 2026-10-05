#pragma once

#include "../audio/audio_node.h"
#include <string>
#include <vector>
#include <windows.h>

namespace praccy::plugins {

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
    [[nodiscard]] virtual float getParameterValue(uint32_t paramId) const = 0;

    [[nodiscard]] virtual bool hasCustomGui() const noexcept = 0;
    virtual bool openGui(HWND parentHwnd) = 0;
    virtual void closeGui() = 0;
    virtual void getPreferredSize(int& width, int& height) const {
        width = 850;
        height = 600;
    }

    [[nodiscard]] virtual const std::string& path() const noexcept {
        static const std::string s_empty;
        return s_empty;
    }

    [[nodiscard]] virtual std::vector<uint8_t> saveState() const = 0;
    virtual bool loadState(const std::vector<uint8_t>& state) = 0;
};

} // namespace praccy::plugins
