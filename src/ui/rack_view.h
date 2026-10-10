#pragma once

#include "../audio/graph_engine.h"
#include "../audio/asio_manager.h"
#include "../tools/tuner.h"
#include "../tools/metronome.h"
#include "../midi/midi_manager.h"
#include "../state/scene_manager.h"
#include "../plugins/plugin_scanner.h"
#include "../tools/audio_player.h"
#include "../tools/quick_looper.h"
#include "modals/plugin_browser_modal.h"
#include "modals/settings_modal.h"
#include "modals/practice_tools_modal.h"
#include <vector>
#include <string>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace praccy::ui {

class RackView {
public:
    RackView(audio::GraphEngine& graph,
             audio::AsioManager& asio,
             tools::InstrumentTuner& tuner,
             tools::Metronome& metronome,
             tools::AudioPlayer& player,
             tools::QuickLooper& looper,
             midi::MidiManager& midi,
             state::SceneManager& scenes,
             plugins::PluginScanner& scanner);

    void render();
    void setDspStats(std::atomic<float>* load, std::atomic<uint32_t>* dropouts) {
        m_dspLoadPercent = load;
        m_dspDropouts = dropouts;
    }
    void resetDspDropouts() {
        if (m_dspDropouts) {
            m_dspDropouts->store(0, std::memory_order_relaxed);
        }
    }

    void triggerHudToast(std::string message);

private:
    void renderPraccyLogo();
    bool renderStatusPill(bool isRunning, const char* statusText);
    void renderPracticeRibbon();
    void renderSignalRack();
    void renderSignalCable(float width = 36.0f);
    void renderInputCard(float cardY = -1.0f);
    void renderPluginSlot(audio::PluginSlot* slot, int slotIndex, int blockIndex = -1, int branchIndex = -1);
    void renderParallelBlock(audio::ParallelSplitMergeBlock* block, int blockIndex, float centerY);
    void renderSceneBar();
    void renderBottomBar();
    void renderMeter(const char* label, float level, float width, float height);
    void renderDspTweakModal();
    bool renderRotaryKnob(const char* label, float* value, float minVal, float maxVal, float radius, const char* format = "%.1f", float defaultVal = 0.0f);
    void renderUpdateModal();
    void renderFloatingHudToast();

    audio::GraphEngine& m_graph;
    audio::AsioManager& m_asio;
    tools::InstrumentTuner& m_tuner;
    tools::Metronome& m_metronome;
    tools::AudioPlayer& m_player;
    tools::QuickLooper& m_looper;
    midi::MidiManager& m_midi;
    state::SceneManager& m_scenes;
    plugins::PluginScanner& m_scanner;

    std::unique_ptr<PluginBrowserModal> m_pluginBrowserModal;
    std::unique_ptr<SettingsModal> m_settingsModal;
    std::unique_ptr<PracticeToolsModal> m_practiceToolsModal;

    std::atomic<float>* m_dspLoadPercent{nullptr};
    std::atomic<uint32_t>* m_dspDropouts{nullptr};

    std::vector<std::chrono::steady_clock::time_point> m_tapTimes;
    std::chrono::steady_clock::time_point m_lastTapFlash{};
    bool m_tempoEditing{false};
    int m_tempoEditValue{120};

    std::unordered_map<std::string, bool> m_branchMinimized;

    bool m_showSavePresetModal{false};
    char m_presetNameBuffer[64]{0};

    std::string m_hudToastText;
    float m_hudToastTimer{0.0f};
    static constexpr float kHudToastDuration = 1.8f;

    audio::PluginSlot* m_dspTweakSlot{nullptr};
    audio::PluginSlot* m_expandedSlot{nullptr};
    float m_expandPulseTimer{0.0f};

    bool m_showUpdateModal{false};
};

} // namespace praccy::ui
