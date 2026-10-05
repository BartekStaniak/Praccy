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

namespace praccy::ui {

class RackView {
public:
    RackView(audio::GraphEngine& graph,
             audio::AsioManager& asio,
             tools::InstrumentTuner& tuner,
             tools::Metronome& metronome,
             midi::MidiManager& midi,
             state::SceneManager& scenes,
             plugins::PluginScanner& scanner);

    void render();

private:
    void renderHeaderBar();
    void renderStatusPill(bool isRunning, double sampleRate, int bufferSize, double latencyMs);
    void renderPracticeRibbon();
    void renderSignalRack();
    void renderSignalCable(float width = 36.0f);
    void renderInputCard();
    void renderPluginSlot(audio::PluginSlot* slot, int slotIndex, int branchIndex = -1);
    void renderParallelBlock(audio::ParallelSplitMergeBlock* block, int blockIndex);
    void renderSceneBar();
    void renderBottomBar();
    void renderMeter(const char* label, float level, float width, float height);
    void renderPluginBrowserModal();
    void renderDspTweakModal();
    bool renderRotaryKnob(const char* label, float* value, float minVal, float maxVal, float radius, const char* format = "%.1f");
    void renderMiniHardwareSlot(audio::PluginSlot* slot, int blockIndex, size_t branchIndex, size_t slotIndex);
    void renderUpdateModal();

    audio::GraphEngine& m_graph;
    audio::AsioManager& m_asio;
    tools::InstrumentTuner& m_tuner;
    tools::Metronome& m_metronome;
    midi::MidiManager& m_midi;
    state::SceneManager& m_scenes;
    plugins::PluginScanner& m_scanner;

    std::vector<audio::AsioDriverDesc> m_cachedDrivers;
    int m_selectedDriverIdx{0};

    bool m_showPluginBrowser{false};
    bool m_focusPluginBrowser{false};
    char m_newPathBuffer[260]{0};

    char m_pluginSearchQuery[128]{0};
    std::string m_selectedDeveloperFilter{"All"};
    std::string m_selectedFormatFilter{"All"};
    int m_pluginSortMode{0}; // 0: Name A-Z, 1: Name Z-A, 2: Dev A-Z, 3: Format A-Z

    bool m_showSavePresetModal{false};
    char m_presetNameBuffer[64]{0};
    std::string m_sceneFeedbackMsg;
    float m_sceneFeedbackTimer{0.0f};

    int m_insertTargetBlockIndex{-1};
    int m_insertTargetBranchIndex{-1};
    audio::PluginSlot* m_dspTweakSlot{nullptr};

    int m_expandedSlotIndex{-1};
    float m_expandPulseTimer{0.0f};

    bool m_showUpdateModal{false};
};

} // namespace praccy::ui
