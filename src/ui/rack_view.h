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
    char m_newPathBuffer[260]{0};
};

} // namespace praccy::ui
