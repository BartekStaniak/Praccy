#pragma once

#include "plugin_base.h"
#include "../audio/dsp_utils.h"
#include <vector>
#include <cmath>
#include <numbers>

namespace praccy::plugins {

/**
 * @brief Built-in Overdrive Pedal (TS-style soft clipper with drive and tone controls).
 */
class OverdriveEffect : public IPluginInstance {
public:
    OverdriveEffect();

    void prepare(double sampleRate, uint32_t maxBlockSize) override;
    void process(audio::AudioProcessContext& ctx) override;
    void reset() override;

    [[nodiscard]] audio::NodeType type() const noexcept override { return audio::NodeType::Plugin; }
    [[nodiscard]] const std::string& name() const noexcept override { return m_name; }
    [[nodiscard]] const std::string& vendor() const noexcept override { return m_vendor; }
    [[nodiscard]] const std::string& version() const noexcept override { return m_version; }

    [[nodiscard]] size_t numParameters() const noexcept override { return 3; }
    [[nodiscard]] PluginParameterDesc getParameterDesc(size_t index) const override;
    void setParameterValue(uint32_t paramId, float value) override;
    [[nodiscard]] float getParameterValue(uint32_t paramId) const override;

    [[nodiscard]] bool hasCustomGui() const noexcept override { return false; }
    bool openGui(HWND) override { return false; }
    void closeGui() override {}

    [[nodiscard]] std::vector<uint8_t> saveState() const override { return {}; }
    bool loadState(const std::vector<uint8_t>&) override { return true; }

private:
    std::string m_name{"Praccy Drive"};
    std::string m_vendor{"Praccy Audio"};
    std::string m_version{"1.0.0"};

    float m_drive{4.0f};  // 1.0 to 15.0
    float m_tone{0.5f};   // 0.0 to 1.0
    float m_level{1.0f};  // 0.0 to 2.0

    float m_filterState[2]{0.0f, 0.0f};
};

/**
 * @brief Built-in High-Gain / Clean Amp Emulation.
 */
class TubeAmpEffect : public IPluginInstance {
public:
    TubeAmpEffect();

    void prepare(double sampleRate, uint32_t maxBlockSize) override;
    void process(audio::AudioProcessContext& ctx) override;
    void reset() override;

    [[nodiscard]] audio::NodeType type() const noexcept override { return audio::NodeType::Plugin; }
    [[nodiscard]] const std::string& name() const noexcept override { return m_name; }
    [[nodiscard]] const std::string& vendor() const noexcept override { return m_vendor; }
    [[nodiscard]] const std::string& version() const noexcept override { return m_version; }

    [[nodiscard]] size_t numParameters() const noexcept override { return 4; }
    [[nodiscard]] PluginParameterDesc getParameterDesc(size_t index) const override;
    void setParameterValue(uint32_t paramId, float value) override;
    [[nodiscard]] float getParameterValue(uint32_t paramId) const override;

    [[nodiscard]] bool hasCustomGui() const noexcept override { return false; }
    bool openGui(HWND) override { return false; }
    void closeGui() override {}

    [[nodiscard]] std::vector<uint8_t> saveState() const override { return {}; }
    bool loadState(const std::vector<uint8_t>&) override { return true; }

private:
    std::string m_name{"Praccy Amp Sim"};
    std::string m_vendor{"Praccy Audio"};
    std::string m_version{"1.0.0"};

    float m_gain{6.0f};
    float m_bass{0.5f};
    float m_treble{0.5f};
    float m_master{0.8f};

    float m_cabFilterL{0.0f};
    float m_cabFilterR{0.0f};
};

/**
 * @brief Built-in Stereo Ping-Pong Delay.
 */
class StereoDelayEffect : public IPluginInstance {
public:
    StereoDelayEffect();

    void prepare(double sampleRate, uint32_t maxBlockSize) override;
    void process(audio::AudioProcessContext& ctx) override;
    void reset() override;

    [[nodiscard]] audio::NodeType type() const noexcept override { return audio::NodeType::Plugin; }
    [[nodiscard]] const std::string& name() const noexcept override { return m_name; }
    [[nodiscard]] const std::string& vendor() const noexcept override { return m_vendor; }
    [[nodiscard]] const std::string& version() const noexcept override { return m_version; }

    [[nodiscard]] size_t numParameters() const noexcept override { return 3; }
    [[nodiscard]] PluginParameterDesc getParameterDesc(size_t index) const override;
    void setParameterValue(uint32_t paramId, float value) override;
    [[nodiscard]] float getParameterValue(uint32_t paramId) const override;

    [[nodiscard]] bool hasCustomGui() const noexcept override { return false; }
    bool openGui(HWND) override { return false; }
    void closeGui() override {}

    [[nodiscard]] std::vector<uint8_t> saveState() const override { return {}; }
    bool loadState(const std::vector<uint8_t>&) override { return true; }

private:
    std::string m_name{"Praccy Stereo Delay"};
    std::string m_vendor{"Praccy Audio"};
    std::string m_version{"1.0.0"};

    double m_sampleRate{48000.0};
    float m_timeMs{350.0f};
    float m_feedback{0.45f};
    float m_mix{0.35f};

    std::vector<float> m_delayBufferL;
    std::vector<float> m_delayBufferR;
    uint32_t m_writePos{0};
};

} // namespace praccy::plugins
