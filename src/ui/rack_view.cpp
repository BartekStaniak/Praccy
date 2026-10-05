#include "rack_view.h"
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace praccy::ui {

RackView::RackView(audio::GraphEngine& graph,
                   audio::AsioManager& asio,
                   tools::InstrumentTuner& tuner,
                   tools::Metronome& metronome,
                   midi::MidiManager& midi,
                   state::SceneManager& scenes)
    : m_graph(graph),
      m_asio(asio),
      m_tuner(tuner),
      m_metronome(metronome),
      m_midi(midi),
      m_scenes(scenes) {
    m_cachedDrivers = audio::AsioManager::enumerateDrivers();
}

void RackView::render() {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("PraccyMainWindow", nullptr, windowFlags)) {
        renderHeaderBar();
        ImGui::Separator();
        renderPracticeRibbon();
        ImGui::Separator();
        renderSceneBar();
        ImGui::Separator();
        renderSignalRack();
        renderBottomBar();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void RackView::renderHeaderBar() {
    if (ImGui::BeginMenuBar()) {
        ImGui::TextColored(ImVec4(0.98f, 0.60f, 0.20f, 1.0f), "PRACCY");
        ImGui::SameLine();
        ImGui::TextDisabled("v1.0.0");
        ImGui::SameLine(100);

        // Driver selection
        ImGui::Text("ASIO Device:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220);

        std::string previewName = m_cachedDrivers.empty() ? "No ASIO Driver Found" :
            (m_selectedDriverIdx < static_cast<int>(m_cachedDrivers.size()) ? m_cachedDrivers[m_selectedDriverIdx].name : "Select");

        if (ImGui::BeginCombo("##DriverCombo", previewName.c_str())) {
            for (int i = 0; i < static_cast<int>(m_cachedDrivers.size()); ++i) {
                const bool isSelected = (m_selectedDriverIdx == i);
                if (ImGui::Selectable(m_cachedDrivers[i].name.c_str(), isSelected)) {
                    m_selectedDriverIdx = i;
                    if (m_asio.isLoaded()) m_asio.unloadDriver();
                    m_asio.loadDriver(m_cachedDrivers[i]);
                    m_asio.start();
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine(380);
        if (m_asio.isRunning()) {
            const auto& info = m_asio.driverInfo();
            const double latencyMs = (2.0 * info.currentBufferSize / info.sampleRate) * 1000.0;
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "[RUNNING]");
            ImGui::SameLine();
            ImGui::Text("%.0f kHz | %d spls (%.1f ms)", info.sampleRate / 1000.0, info.currentBufferSize, latencyMs);
        } else {
            ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "[STOPPED]");
        }

        ImGui::SameLine();
        if (ImGui::Button("ASIO Settings")) {
            m_asio.openControlPanel();
        }

        ImGui::EndMenuBar();
    }
}

void RackView::renderPracticeRibbon() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.16f, 1.0f));
    ImGui::BeginChild("PracticeRibbon", ImVec2(0, 75), false, ImGuiWindowFlags_NoScrollbar);

    // ==========================================
    // 1. Tuner Section
    // ==========================================
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.9f, 1.0f), "TUNER");
    auto result = m_tuner.currentResult();
    if (result.confidence) {
        bool inTune = std::abs(result.centDeviation) <= 3.0f;
        ImVec4 noteColor = inTune ? ImVec4(0.2f, 0.95f, 0.3f, 1.0f) : ImVec4(0.95f, 0.85f, 0.3f, 1.0f);

        ImGui::TextColored(noteColor, "%s", result.noteName.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%.1f Hz)", result.frequencyHz);

        // Needle gauge [-50, +50 cents]
        float needlePos = (result.centDeviation + 50.0f) / 100.0f;
        needlePos = std::clamp(needlePos, 0.0f, 1.0f);
        ImGui::ProgressBar(needlePos, ImVec2(180, 12), inTune ? "IN TUNE" : "");
    } else {
        ImGui::TextDisabled("--");
        ImGui::ProgressBar(0.5f, ImVec2(180, 12), "Listening...");
    }
    ImGui::EndGroup();

    // ==========================================
    // 2. Metronome Section
    // ==========================================
    ImGui::SameLine(250);
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.9f, 1.0f), "METRONOME");

    bool metroPlaying = m_metronome.isPlaying();
    if (ImGui::Button(metroPlaying ? " [■ Stop] " : " [▶ Play] ")) {
        m_metronome.setPlaying(!metroPlaying);
    }
    ImGui::SameLine();

    float bpm = m_metronome.bpm();
    ImGui::SetNextItemWidth(90);
    if (ImGui::DragFloat("##BPM", &bpm, 1.0f, 40.0f, 260.0f, "%.0f BPM")) {
        m_metronome.setBpm(bpm);
    }
    ImGui::SameLine();

    // Visual beat indicators (4 dots)
    const int currentBeat = m_metronome.currentBeat();
    for (int b = 0; b < 4; ++b) {
        bool active = metroPlaying && (b == currentBeat);
        ImVec4 dotColor = active ? (b == 0 ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) : ImVec4(0.98f, 0.60f, 0.20f, 1.0f))
                                 : ImVec4(0.25f, 0.28f, 0.33f, 1.0f);
        ImGui::SameLine();
        ImGui::TextColored(dotColor, "●");
    }
    ImGui::EndGroup();

    // ==========================================
    // 3. Noise Gate Section
    // ==========================================
    ImGui::SameLine(500);
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.9f, 1.0f), "NOISE GATE");
    auto& gate = m_graph.inputNoiseGate();
    bool gateOn = gate.isEnabled();
    if (ImGui::Checkbox("Active##Gate", &gateOn)) {
        gate.setEnabled(gateOn);
    }
    ImGui::SameLine();
    float thresh = gate.thresholdDb();
    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderFloat("##GateThresh", &thresh, -80.0f, -20.0f, "%.0f dB")) {
        gate.setThresholdDb(thresh);
    }
    ImGui::EndGroup();

    // ==========================================
    // 4. Master Output & Safety Limiter
    // ==========================================
    ImGui::SameLine(ImGui::GetWindowWidth() - 220);
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.9f, 1.0f), "MASTER OUTPUT");
    float masterVol = m_graph.masterVolumeDb();
    ImGui::SetNextItemWidth(110);
    if (ImGui::SliderFloat("##MasterVol", &masterVol, -36.0f, +6.0f, "%.1f dB")) {
        m_graph.setMasterVolumeDb(masterVol);
    }
    ImGui::SameLine();
    renderMeter("OutMeter", m_graph.outputMeter().peakLeft(), 14, 30);
    ImGui::SameLine();
    renderMeter("OutMeterR", m_graph.outputMeter().peakRight(), 14, 30);
    ImGui::EndGroup();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void RackView::renderSceneBar() {
    ImGui::Text("SCENE / PRESET SNAPSHOT:");
    ImGui::SameLine();

    for (size_t s = 0; s < m_scenes.numScenes(); ++s) {
        const auto* sc = m_scenes.getScene(s);
        if (!sc) continue;

        bool isActive = (static_cast<int>(s) == m_scenes.activeSceneIndex());
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.98f, 0.60f, 0.20f, 0.9f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));
        }

        if (ImGui::Button(sc->name.c_str())) {
            m_scenes.applyScene(static_cast<int>(s), m_graph);
        }

        if (isActive) {
            ImGui::PopStyleColor(2);
        }
        ImGui::SameLine();
    }

    if (ImGui::Button("[Save Current to Scene]")) {
        m_scenes.captureCurrentScene(m_scenes.activeSceneIndex(), m_graph);
    }
}

void RackView::renderSignalRack() {
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.8f, 1.0f), "SIGNAL CHAIN (RACK):");

    ImGui::BeginChild("RackScrollArea", ImVec2(0, -40), true, ImGuiWindowFlags_HorizontalScrollbar);

    // Master Input Node
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.20f, 0.24f, 1.0f));
    ImGui::BeginChild("InputGainNode", ImVec2(100, 140), true);
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[ INPUT ]");
    float inGain = m_graph.inputGainDb();
    ImGui::SetNextItemWidth(75);
    if (ImGui::VSliderFloat("##InGain", ImVec2(24, 75), &inGain, -24.0f, +24.0f, "")) {
        m_graph.setInputGainDb(inGain);
    }
    ImGui::SameLine();
    renderMeter("InL", m_graph.inputMeter().peakLeft(), 10, 75);
    ImGui::SameLine();
    renderMeter("InR", m_graph.inputMeter().peakRight(), 10, 75);
    ImGui::Text("%.0fdB", inGain);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    // Signal Flow Arrow
    ImGui::SameLine();
    ImGui::Text("►");
    ImGui::SameLine();

    // Render nodes in the graph
    const size_t numNodes = m_graph.numNodes();
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = m_graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            renderPluginSlot(slot, static_cast<int>(i));
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            renderParallelBlock(block, static_cast<int>(i));
        }

        ImGui::SameLine();
        ImGui::Text("►");
        ImGui::SameLine();
    }

    // Output Destination Node
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.20f, 0.24f, 1.0f));
    ImGui::BeginChild("OutputDestNode", ImVec2(90, 140), true);
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[ OUTPUT ]");
    ImGui::Text("To ASIO");
    renderMeter("FinalL", m_graph.outputMeter().peakLeft(), 14, 60);
    ImGui::SameLine();
    renderMeter("FinalR", m_graph.outputMeter().peakRight(), 14, 60);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    ImGui::EndChild();
}

void RackView::renderPluginSlot(audio::PluginSlot* slot, int slotIndex, int branchIndex) {
    if (!slot) return;

    ImGui::BeginGroup();
    const bool bypassed = slot->isBypassed();
    ImVec4 cardBg = bypassed ? ImVec4(0.13f, 0.14f, 0.16f, 0.6f) : ImVec4(0.17f, 0.19f, 0.24f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg);
    char childId[64];
    std::snprintf(childId, sizeof(childId), "SlotCard_%d_%d", slotIndex, branchIndex);
    ImGui::BeginChild(childId, ImVec2(170, 140), true);

    // Title & Bypass toggle
    bool active = !bypassed;
    char pwrLabel[32];
    std::snprintf(pwrLabel, sizeof(pwrLabel), "##Pwr_%d_%d", slotIndex, branchIndex);
    if (ImGui::Checkbox(pwrLabel, &active)) {
        slot->setBypassed(!active);
    }
    ImGui::SameLine();
    ImGui::TextColored(active ? ImVec4(0.98f, 0.60f, 0.20f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", slot->name().c_str());

    // Dry / Wet knob/slider
    float mix = slot->dryWet() * 100.0f;
    char mixLabel[32];
    std::snprintf(mixLabel, sizeof(mixLabel), "##Mix_%d_%d", slotIndex, branchIndex);
    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderFloat(mixLabel, &mix, 0.0f, 100.0f, "Wet: %.0f%%")) {
        slot->setDryWet(mix / 100.0f);
    }
    ImGui::SameLine();
    renderMeter("SlotMtr", slot->meter().peakLeft(), 8, 20);

    // Output trim
    float outTrim = slot->outputGainDb();
    char trimLabel[32];
    std::snprintf(trimLabel, sizeof(trimLabel), "##Trim_%d_%d", slotIndex, branchIndex);
    ImGui::SetNextItemWidth(100);
    if (ImGui::SliderFloat(trimLabel, &outTrim, -24.0f, +12.0f, "Trim: %.1fdB")) {
        slot->setOutputGainDb(outTrim);
    }

    // Context Menu for MIDI Learn
    char popupId[64];
    std::snprintf(popupId, sizeof(popupId), "SlotCtx_%d_%d", slotIndex, branchIndex);
    if (ImGui::BeginPopupContextItem(popupId)) {
        if (ImGui::MenuItem("MIDI Learn: Bypass Toggle")) {
            m_midi.startLearning(midi::BindingTargetType::SlotBypass, slotIndex, branchIndex);
        }
        if (ImGui::MenuItem("MIDI Learn: Dry/Wet Mix")) {
            m_midi.startLearning(midi::BindingTargetType::SlotDryWet, slotIndex, branchIndex);
        }
        if (ImGui::MenuItem("MIDI Learn: Output Trim")) {
            m_midi.startLearning(midi::BindingTargetType::SlotOutputGain, slotIndex, branchIndex);
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

void RackView::renderParallelBlock(audio::ParallelSplitMergeBlock* block, int blockIndex) {
    if (!block) return;

    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.16f, 0.20f, 1.0f));
    char blockChildId[64];
    std::snprintf(blockChildId, sizeof(blockChildId), "ParBlock_%d", blockIndex);
    ImGui::BeginChild(blockChildId, ImVec2(400, 160), true);

    ImGui::TextColored(ImVec4(0.3f, 0.8f, 0.9f, 1.0f), "⤹ PARALLEL SPLIT");

    for (size_t b = 0; b < block->numBranches(); ++b) {
        auto* branch = block->getBranch(b);
        if (!branch) continue;

        ImGui::Separator();
        ImGui::Text("[%s]", branch->name().c_str());
        ImGui::SameLine();

        // Mute / Solo
        bool muted = branch->isMuted();
        char muteLabel[32];
        std::snprintf(muteLabel, sizeof(muteLabel), "M##%d_%zu", blockIndex, b);
        if (ImGui::Checkbox(muteLabel, &muted)) {
            branch->setMuted(muted);
        }
        ImGui::SameLine();

        bool solo = branch->isSolo();
        char soloLabel[32];
        std::snprintf(soloLabel, sizeof(soloLabel), "S##%d_%zu", blockIndex, b);
        if (ImGui::Checkbox(soloLabel, &solo)) {
            branch->setSolo(solo);
        }
        ImGui::SameLine();

        // Branch Pan & Gain
        float pan = branch->pan();
        char panLabel[32];
        std::snprintf(panLabel, sizeof(panLabel), "##Pan_%d_%zu", blockIndex, b);
        ImGui::SetNextItemWidth(75);
        if (ImGui::SliderFloat(panLabel, &pan, -1.0f, +1.0f, "Pan: %.2f")) {
            branch->setPan(pan);
        }
        ImGui::SameLine();

        float gain = branch->gainDb();
        char gainLabel[32];
        std::snprintf(gainLabel, sizeof(gainLabel), "##Gain_%d_%zu", blockIndex, b);
        ImGui::SetNextItemWidth(75);
        if (ImGui::SliderFloat(gainLabel, &gain, -36.0f, +12.0f, "%.1fdB")) {
            branch->setGainDb(gain);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

void RackView::renderBottomBar() {
    ImGui::Separator();
    if (m_midi.isLearning()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "★ MIDI LEARN ACTIVE: Move any knob, slider, or press a footswitch to bind...");
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            m_midi.cancelLearning();
        }
    } else {
        ImGui::TextDisabled("MIDI: %s", m_midi.lastActivityDescription().c_str());
    }
}

void RackView::renderMeter(const char* label, float level, float width, float height) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background
    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(30, 32, 38, 255));

    // Meter Fill (logarithmic scaled)
    float db = audio::DspUtils::gainToDb(level);
    float norm = (db + 60.0f) / 60.0f; // -60dB to 0dB range
    norm = std::clamp(norm, 0.0f, 1.0f);

    float fillHeight = height * norm;
    ImU32 meterColor = (db > -0.5f) ? IM_COL32(240, 50, 50, 255) : // Red (clipping)
                       (db > -6.0f) ? IM_COL32(240, 180, 40, 255) : // Amber
                                      IM_COL32(40, 210, 80, 255);   // Green

    drawList->AddRectFilled(ImVec2(pos.x, pos.y + height - fillHeight),
                            ImVec2(pos.x + width, pos.y + height),
                            meterColor);

    // Border
    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(60, 65, 75, 255));
    ImGui::Dummy(ImVec2(width, height));
}

} // namespace praccy::ui
