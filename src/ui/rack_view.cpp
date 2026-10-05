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
    if (m_asio.isLoaded()) {
        const std::string& loadedName = m_asio.driverInfo().name;
        for (int i = 0; i < static_cast<int>(m_cachedDrivers.size()); ++i) {
            if (m_cachedDrivers[i].name == loadedName) {
                m_selectedDriverIdx = i;
                break;
            }
        }
    }
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
        ImGui::SetNextItemWidth(230);

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

        ImGui::SameLine(400);
        if (m_asio.isRunning()) {
            const auto& info = m_asio.driverInfo();
            const double latencyMs = (2.0 * info.currentBufferSize / info.sampleRate) * 1000.0;
            ImGui::TextColored(ImVec4(0.30f, 0.95f, 0.45f, 1.0f), "[ACTIVE]");
            ImGui::SameLine();
            ImGui::Text("%.0f kHz | %d spls (%.1f ms)", info.sampleRate / 1000.0, info.currentBufferSize, latencyMs);
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "[STOPPED]");
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
    ImGui::BeginChild("PracticeRibbon", ImVec2(0, 80), false, ImGuiWindowFlags_NoScrollbar);

    // ==========================================
    // 1. Tuner Section (Custom Strobe Needle)
    // ==========================================
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 1.0f), "TUNER");
    auto result = m_tuner.currentResult();

    if (result.confidence) {
        bool inTune = std::abs(result.centDeviation) <= 3.0f;
        ImVec4 noteColor = inTune ? ImVec4(0.25f, 0.95f, 0.40f, 1.0f) : ImVec4(0.98f, 0.82f, 0.25f, 1.0f);

        ImGui::TextColored(noteColor, "%s", result.noteName.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%.1f Hz)", result.frequencyHz);
    } else {
        ImGui::TextDisabled("-- (Listening)");
    }

    // Custom Strobe Needle Gauge
    {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float gaugeW = 190.0f;
        const float gaugeH = 18.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background groove
        dl->AddRectFilled(pos, ImVec2(pos.x + gaugeW, pos.y + gaugeH), IM_COL32(24, 26, 32, 255), 4.0f);
        dl->AddRect(pos, ImVec2(pos.x + gaugeW, pos.y + gaugeH), IM_COL32(50, 54, 66, 255), 4.0f);

        // Center zero line (green)
        const float midX = pos.x + (gaugeW * 0.5f);
        dl->AddLine(ImVec2(midX, pos.y + 2), ImVec2(midX, pos.y + gaugeH - 2), IM_COL32(40, 200, 80, 220), 2.0f);

        // Sub-ticks at -25 and +25 cents
        dl->AddLine(ImVec2(pos.x + gaugeW * 0.25f, pos.y + 5), ImVec2(pos.x + gaugeW * 0.25f, pos.y + gaugeH - 5), IM_COL32(70, 75, 90, 200), 1.0f);
        dl->AddLine(ImVec2(pos.x + gaugeW * 0.75f, pos.y + 5), ImVec2(pos.x + gaugeW * 0.75f, pos.y + gaugeH - 5), IM_COL32(70, 75, 90, 200), 1.0f);

        if (result.confidence) {
            bool inTune = std::abs(result.centDeviation) <= 3.0f;
            float norm = (result.centDeviation + 50.0f) / 100.0f;
            norm = std::clamp(norm, 0.0f, 1.0f);
            float needleX = pos.x + (norm * gaugeW);

            ImU32 needleColor = inTune ? IM_COL32(40, 240, 80, 255) : IM_COL32(250, 180, 30, 255);
            dl->AddRectFilled(ImVec2(needleX - 2.0f, pos.y + 1), ImVec2(needleX + 2.0f, pos.y + gaugeH - 1), needleColor, 2.0f);
        } else {
            // Idle indicator
            dl->AddCircleFilled(ImVec2(midX, pos.y + gaugeH * 0.5f), 3.0f, IM_COL32(90, 95, 110, 180));
        }
        ImGui::Dummy(ImVec2(gaugeW, gaugeH));
    }
    ImGui::EndGroup();

    // ==========================================
    // 2. Metronome Section (Custom Vector LEDs)
    // ==========================================
    ImGui::SameLine(250);
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 1.0f), "METRONOME");

    bool metroPlaying = m_metronome.isPlaying();
    if (ImGui::Button(metroPlaying ? " STOP " : " PLAY ")) {
        m_metronome.setPlaying(!metroPlaying);
    }
    ImGui::SameLine();

    float bpm = m_metronome.bpm();
    ImGui::SetNextItemWidth(90);
    if (ImGui::DragFloat("##BPM", &bpm, 1.0f, 40.0f, 260.0f, "%.0f BPM")) {
        m_metronome.setBpm(bpm);
    }
    ImGui::SameLine();

    // Custom Vector Beat LEDs (Circles drawn via ImDrawList)
    {
        const int currentBeat = m_metronome.currentBeat();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 curPos = ImGui::GetCursorScreenPos();
        const float dotRadius = 5.5f;
        const float dotSpacing = 16.0f;

        for (int b = 0; b < 4; ++b) {
            float cx = curPos.x + 8.0f + (b * dotSpacing);
            float cy = curPos.y + 12.0f;
            bool active = metroPlaying && (b == currentBeat);

            if (active) {
                // Glow outer halo
                ImU32 haloColor = (b == 0) ? IM_COL32(255, 60, 60, 80) : IM_COL32(250, 160, 30, 80);
                dl->AddCircleFilled(ImVec2(cx, cy), dotRadius + 3.0f, haloColor);

                // Bright center
                ImU32 ledColor = (b == 0) ? IM_COL32(255, 70, 70, 255) : IM_COL32(255, 180, 40, 255);
                dl->AddCircleFilled(ImVec2(cx, cy), dotRadius, ledColor);
            } else {
                // Inactive bezel
                dl->AddCircleFilled(ImVec2(cx, cy), dotRadius, IM_COL32(36, 40, 48, 255));
                dl->AddCircle(ImVec2(cx, cy), dotRadius, IM_COL32(55, 60, 72, 255), 0, 1.0f);
            }
        }
        ImGui::Dummy(ImVec2(4 * dotSpacing + 10.0f, 24.0f));
    }
    ImGui::EndGroup();

    // ==========================================
    // 3. Noise Gate Section
    // ==========================================
    ImGui::SameLine(510);
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 1.0f), "NOISE GATE");
    auto& gate = m_graph.inputNoiseGate();
    bool gateOn = gate.isEnabled();
    if (ImGui::Checkbox("Active##Gate", &gateOn)) {
        gate.setEnabled(gateOn);
    }
    ImGui::SameLine();
    float thresh = gate.thresholdDb();
    ImGui::SetNextItemWidth(90);
    if (ImGui::SliderFloat("##GateThresh", &thresh, -80.0f, -20.0f, "%.0f dB")) {
        gate.setThresholdDb(thresh);
    }
    ImGui::EndGroup();

    // ==========================================
    // 4. Master Output Section
    // ==========================================
    ImGui::SameLine(ImGui::GetWindowWidth() - 240);
    ImGui::BeginGroup();
    ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 1.0f), "MASTER OUTPUT");
    float masterVol = m_graph.masterVolumeDb();
    ImGui::SetNextItemWidth(110);
    if (ImGui::SliderFloat("##MasterVol", &masterVol, -36.0f, +6.0f, "%.1f dB")) {
        m_graph.setMasterVolumeDb(masterVol);
    }
    ImGui::SameLine();
    renderMeter("OutMeterL", m_graph.outputMeter().peakLeft(), 12, 34);
    ImGui::SameLine();
    renderMeter("OutMeterR", m_graph.outputMeter().peakRight(), 12, 34);
    ImGui::EndGroup();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void RackView::renderSceneBar() {
    ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.85f, 1.0f), "SCENE PRESETS:");
    ImGui::SameLine();

    for (size_t s = 0; s < m_scenes.numScenes(); ++s) {
        const auto* sc = m_scenes.getScene(s);
        if (!sc) continue;

        bool isActive = (static_cast<int>(s) == m_scenes.activeSceneIndex());
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.98f, 0.60f, 0.20f, 0.95f));
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

    if (ImGui::Button("[Save Scene]")) {
        m_scenes.captureCurrentScene(m_scenes.activeSceneIndex(), m_graph);
    }
}

void RackView::renderSignalRack() {
    ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.80f, 1.0f), "SIGNAL CHAIN (RACK):");

    ImGui::BeginChild("RackScrollArea", ImVec2(0, -42), true, ImGuiWindowFlags_HorizontalScrollbar);

    // ==========================================
    // Master Input Node Card
    // ==========================================
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
    ImGui::BeginChild("InputGainNode", ImVec2(100, 155), true);
    ImGui::TextColored(ImVec4(0.35f, 0.80f, 1.0f, 1.0f), "[ INPUT ]");
    float inGain = m_graph.inputGainDb();
    if (ImGui::VSliderFloat("##InGain", ImVec2(24, 78), &inGain, -24.0f, +24.0f, "")) {
        m_graph.setInputGainDb(inGain);
    }
    ImGui::SameLine();
    renderMeter("InL", m_graph.inputMeter().peakLeft(), 10, 78);
    ImGui::SameLine();
    renderMeter("InR", m_graph.inputMeter().peakRight(), 10, 78);
    ImGui::Text("%.0fdB", inGain);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    // Signal connection line & arrow
    ImGui::SameLine();
    ImGui::TextDisabled("-->");
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
        ImGui::TextDisabled("-->");
        ImGui::SameLine();
    }

    // ==========================================
    // Master Output Node Card
    // ==========================================
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
    ImGui::BeginChild("OutputDestNode", ImVec2(100, 155), true);
    ImGui::TextColored(ImVec4(0.35f, 0.80f, 1.0f, 1.0f), "[ OUTPUT ]");
    ImGui::TextDisabled("To ASIO");
    renderMeter("FinalL", m_graph.outputMeter().peakLeft(), 14, 75);
    ImGui::SameLine();
    renderMeter("FinalR", m_graph.outputMeter().peakRight(), 14, 75);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    ImGui::EndChild();
}

void RackView::renderPluginSlot(audio::PluginSlot* slot, int slotIndex, int branchIndex) {
    if (!slot) return;

    ImGui::BeginGroup();
    const bool bypassed = slot->isBypassed();
    ImVec4 cardBg = bypassed ? ImVec4(0.13f, 0.14f, 0.17f, 0.6f) : ImVec4(0.17f, 0.19f, 0.24f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg);
    char childId[64];
    std::snprintf(childId, sizeof(childId), "SlotCard_%d_%d", slotIndex, branchIndex);

    // Card dimensions: 215px width ensures no title truncation!
    ImGui::BeginChild(childId, ImVec2(215, 155), true);

    // Title
    ImGui::TextColored(bypassed ? ImVec4(0.55f, 0.58f, 0.65f, 1.0f) : ImVec4(0.98f, 0.98f, 1.0f, 1.0f),
                       "%s", slot->name().c_str());

    // Hardware-style Bypass / Active Button with LED
    {
        bool active = !bypassed;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.35f, 0.25f, 1.0f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.24f, 0.28f, 1.0f));
        }

        char btnId[32];
        std::snprintf(btnId, sizeof(btnId), "%s##Btn_%d_%d", active ? "[ACTIVE]" : "[BYPASS]", slotIndex, branchIndex);
        if (ImGui::Button(btnId)) {
            slot->setBypassed(active);
        }
        ImGui::PopStyleColor();
    }

    // Dry / Wet mix slider
    float mix = slot->dryWet() * 100.0f;
    char mixLabel[32];
    std::snprintf(mixLabel, sizeof(mixLabel), "##Mix_%d_%d", slotIndex, branchIndex);
    ImGui::TextDisabled("Dry/Wet:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::SliderFloat(mixLabel, &mix, 0.0f, 100.0f, "%.0f%%")) {
        slot->setDryWet(mix / 100.0f);
    }
    ImGui::SameLine();
    renderMeter("SlotMtr", slot->meter().peakLeft(), 8, 22);

    // Output trim slider
    float outTrim = slot->outputGainDb();
    char trimLabel[32];
    std::snprintf(trimLabel, sizeof(trimLabel), "##Trim_%d_%d", slotIndex, branchIndex);
    ImGui::TextDisabled("Trim:    ");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::SliderFloat(trimLabel, &outTrim, -24.0f, +12.0f, "%.1fdB")) {
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
    ImGui::BeginChild(blockChildId, ImVec2(420, 155), true);

    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1.0f), "// PARALLEL SPLIT (A / B)");

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
        if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button(muteLabel)) {
            branch->setMuted(!muted);
        }
        if (muted) ImGui::PopStyleColor();
        ImGui::SameLine();

        bool solo = branch->isSolo();
        char soloLabel[32];
        std::snprintf(soloLabel, sizeof(soloLabel), "S##%d_%zu", blockIndex, b);
        if (solo) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.7f, 0.1f, 1.0f));
        if (ImGui::Button(soloLabel)) {
            branch->setSolo(!solo);
        }
        if (solo) ImGui::PopStyleColor();
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
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[*] MIDI LEARN ACTIVE: Move any knob, slider, or press a footswitch to bind...");
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

    // Background groove
    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(26, 28, 34, 255), 2.0f);

    // Meter Fill (logarithmic scaled)
    float db = audio::DspUtils::gainToDb(level);
    float norm = (db + 60.0f) / 60.0f; // -60dB to 0dB range
    norm = std::clamp(norm, 0.0f, 1.0f);

    float fillHeight = height * norm;
    ImU32 meterColor = (db > -0.5f) ? IM_COL32(245, 50, 50, 255) : // Red (clipping)
                       (db > -6.0f) ? IM_COL32(245, 175, 35, 255) : // Amber
                                      IM_COL32(40, 210, 80, 255);   // Green

    drawList->AddRectFilled(ImVec2(pos.x, pos.y + height - fillHeight),
                            ImVec2(pos.x + width, pos.y + height),
                            meterColor, 2.0f);

    // Border
    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(50, 55, 65, 255), 2.0f);
    ImGui::Dummy(ImVec2(width, height));
}

} // namespace praccy::ui
