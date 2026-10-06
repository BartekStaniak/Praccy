#pragma once

#include "../audio/graph_engine.h"
#include "../audio/asio_manager.h"
#include "../tools/tuner.h"
#include "../tools/metronome.h"
#include "../midi/midi_manager.h"
#include "../state/scene_manager.h"
#include <vector>
#include <string>

#include "../plugins/plugin_scanner.h"

#include "../tools/audio_player.h"
#include "../tools/quick_looper.h"
#include <chrono>
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
    void renderSettingsModal();
    void renderPracticeToolsModal();
    void renderMeter(const char* label, float level, float width, float height);
    void renderPluginBrowserModal();
    void renderDspTweakModal();
    bool renderRotaryKnob(const char* label, float* value, float minVal, float maxVal, float radius, const char* format = "%.1f", float defaultVal = 0.0f);
    void renderUpdateModal();

    bool m_showSettingsModal{false};

    audio::GraphEngine& m_graph;
    audio::AsioManager& m_asio;
    tools::InstrumentTuner& m_tuner;
    tools::Metronome& m_metronome;
    tools::AudioPlayer& m_player;
    tools::QuickLooper& m_looper;
    midi::MidiManager& m_midi;
    state::SceneManager& m_scenes;
    plugins::PluginScanner& m_scanner;

    std::atomic<float>* m_dspLoadPercent{nullptr};
    std::atomic<uint32_t>* m_dspDropouts{nullptr};

    std::vector<std::chrono::steady_clock::time_point> m_tapTimes;
    std::chrono::steady_clock::time_point m_lastTapFlash{};
    bool m_tempoEditing{false};
    int m_tempoEditValue{120};

    std::unordered_map<std::string, bool> m_branchMinimized;
    bool m_showPracticeToolsModal{false};
    char m_audioFilePathBuffer[260]{0};

    std::vector<audio::AsioDriverDesc> m_cachedDrivers;
    int m_selectedDriverIdx{0};

    bool m_showPluginBrowser{false};
    bool m_focusPluginBrowser{false};
    char m_newPathBuffer[260]{0};

    char m_pluginSearchQuery[128]{0};
    std::string m_selectedDeveloperFilter{"All"};
    std::string m_selectedFormatFilter{"All"};
    bool m_showFavoritesFilter{false};
    int m_pluginSortMode{0}; // 0: Name A-Z, 1: Name Z-A, 2: Dev A-Z, 3: Format A-Z

    bool m_showSavePresetModal{false};
    char m_presetNameBuffer[64]{0};
    std::string m_sceneFeedbackMsg;
    float m_sceneFeedbackTimer{0.0f};

    int m_insertTargetBlockIndex{-1};
    int m_insertTargetBranchIndex{-1};
    audio::PluginSlot* m_dspTweakSlot{nullptr};

    audio::PluginSlot* m_expandedSlot{nullptr};
    float m_expandPulseTimer{0.0f};

    bool m_showUpdateModal{false};
};

} // namespace praccy::ui
