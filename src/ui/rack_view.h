#pragma once

#include "../audio/graph_engine.h"
#include "../audio/asio_manager.h"
#include "../tools/tuner.h"
#include "../tools/metronome.h"
#include "../midi/midi_manager.h"
#include "../state/scene_manager.h"
#include <vector>
#include <string>

namespace praccy::ui {

class RackView {
public:
    RackView(audio::GraphEngine& graph,
             audio::AsioManager& asio,
             tools::InstrumentTuner& tuner,
             tools::Metronome& metronome,
             midi::MidiManager& midi,
             state::SceneManager& scenes);

    void render();

private:
    void renderHeaderBar();
    void renderPracticeRibbon();
    void renderSignalRack();
    void renderPluginSlot(audio::PluginSlot* slot, int slotIndex, int branchIndex = -1);
    void renderParallelBlock(audio::ParallelSplitMergeBlock* block, int blockIndex);
    void renderSceneBar();
    void renderBottomBar();
    void renderMeter(const char* label, float level, float width, float height);

    audio::GraphEngine& m_graph;
    audio::AsioManager& m_asio;
    tools::InstrumentTuner& m_tuner;
    tools::Metronome& m_metronome;
    midi::MidiManager& m_midi;
    state::SceneManager& m_scenes;

    std::vector<audio::AsioDriverDesc> m_cachedDrivers;
    int m_selectedDriverIdx{0};
};

} // namespace praccy::ui
