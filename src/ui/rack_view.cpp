#include "rack_view.h"
#include "../plugins/builtin_dsp.h"
#include "../plugins/clap_host.h"
#include "../plugins/vst3_host.h"
#include "../plugins/plugin_window.h"
#include "../state/app_config.h"
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
                   state::SceneManager& scenes,
                   plugins::PluginScanner& scanner)
    : m_graph(graph),
      m_asio(asio),
      m_tuner(tuner),
      m_metronome(metronome),
      m_midi(midi),
      m_scenes(scenes),
      m_scanner(scanner) {
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
                                  ImGuiWindowFlags_MenuBar |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus;

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

        if (m_showPluginBrowser) {
            renderPluginBrowserModal();
        }

        renderDspTweakModal();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void RackView::renderHeaderBar() {
    if (ImGui::BeginMenuBar()) {
        ImGui::TextColored(ImVec4(0.98f, 0.60f, 0.20f, 1.0f), "PRACCY");
        ImGui::SameLine();
        ImGui::TextDisabled("v1.0.0");
        ImGui::SameLine(0, 20);

        // ASIO Device Selector
        ImGui::Text("ASIO Device:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180);

        std::string previewName = m_cachedDrivers.empty() ? "No Driver Found" :
            (m_selectedDriverIdx < static_cast<int>(m_cachedDrivers.size()) ? m_cachedDrivers[m_selectedDriverIdx].name : "Select");

        if (ImGui::BeginCombo("##DriverCombo", previewName.c_str())) {
            for (int i = 0; i < static_cast<int>(m_cachedDrivers.size()); ++i) {
                const bool isSelected = (m_selectedDriverIdx == i);
                if (ImGui::Selectable(m_cachedDrivers[i].name.c_str(), isSelected)) {
                    m_selectedDriverIdx = i;
                    if (m_asio.isLoaded()) m_asio.unloadDriver();
                    m_asio.loadDriver(m_cachedDrivers[i]);
                    m_asio.start();

                    state::AppConfig cfg;
                    cfg.load();
                    cfg.lastAsioDriver = m_cachedDrivers[i].name;
                    cfg.save();
                }
                if (isSelected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine(0, 16);

        // Dynamic Status Pill (Guaranteed zero overlapping & perfectly centered vertically)
        if (m_asio.isRunning()) {
            const auto& info = m_asio.driverInfo();
            const double latencyMs = (2.0 * info.currentBufferSize / info.sampleRate) * 1000.0;
            renderStatusPill(true, info.sampleRate, info.currentBufferSize, latencyMs);
        } else {
            renderStatusPill(false, 0.0, 0, 0.0);
        }

        ImGui::SameLine(0, 14);
        if (ImGui::Button("ASIO Settings")) {
            m_asio.openControlPanel();
        }

        ImGui::SameLine(0, 10);
        if (ImGui::Button("Plugins...")) {
            m_showPluginBrowser = !m_showPluginBrowser;
            if (m_showPluginBrowser) {
                m_insertTargetBlockIndex = -1;
                m_insertTargetBranchIndex = -1;
                m_focusPluginBrowser = true;
            }
        }

        ImGui::EndMenuBar();
    }
}

void RackView::renderStatusPill(bool isRunning, double sampleRate, int bufferSize, double latencyMs) {
    char statusText[128];
    if (isRunning) {
        std::snprintf(statusText, sizeof(statusText), "ACTIVE  |  %.0f kHz  |  %d spls (%.1f ms)", sampleRate / 1000.0, bufferSize, latencyMs);
    } else {
        std::snprintf(statusText, sizeof(statusText), "STOPPED");
    }

    const ImVec2 textSize = ImGui::CalcTextSize(statusText);
    const float pillW = textSize.x + 36.0f;
    const float pillH = 22.0f;
    const float frameH = ImGui::GetFrameHeight();
    const float offsetY = std::max(0.0f, (frameH - pillH) * 0.5f);

    const ImVec2 screenPos = ImGui::GetCursorScreenPos();
    const ImVec2 pos(screenPos.x, screenPos.y + offsetY);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background capsule pill
    ImU32 bgCol = isRunning ? IM_COL32(20, 36, 26, 255) : IM_COL32(40, 24, 24, 255);
    ImU32 borderCol = isRunning ? IM_COL32(45, 120, 60, 255) : IM_COL32(120, 45, 45, 255);
    dl->AddRectFilled(pos, ImVec2(pos.x + pillW, pos.y + pillH), bgCol, 11.0f);
    dl->AddRect(pos, ImVec2(pos.x + pillW, pos.y + pillH), borderCol, 11.0f);

    // Glowing LED Dot
    ImVec2 dotCenter(pos.x + 12.0f, pos.y + (pillH * 0.5f));
    if (isRunning) {
        dl->AddCircleFilled(dotCenter, 6.0f, IM_COL32(40, 240, 80, 80));
        dl->AddCircleFilled(dotCenter, 3.5f, IM_COL32(50, 255, 90, 255));
    } else {
        dl->AddCircleFilled(dotCenter, 3.5f, IM_COL32(230, 60, 60, 255));
    }

    // Text Label inside pill
    ImVec2 textPos(pos.x + 24.0f, pos.y + ((pillH - textSize.y) * 0.5f));
    ImU32 textCol = isRunning ? IM_COL32(210, 245, 220, 255) : IM_COL32(245, 180, 180, 255);
    dl->AddText(textPos, textCol, statusText);

    ImGui::Dummy(ImVec2(pillW, frameH));
}

void RackView::renderPracticeRibbon() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.13f, 0.16f, 1.0f));
    ImGui::BeginChild("PracticeRibbon", ImVec2(0, 82), false, ImGuiWindowFlags_NoScrollbar);

    // Responsive 4-column layout: guaranteed dynamic scaling without overlapping!
    if (ImGui::BeginTable("PracticeRibbonTable", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("TunerCol", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("MetroCol", ImGuiTableColumnFlags_WidthStretch, 0.26f);
        ImGui::TableSetupColumn("GateCol",  ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("MasterCol",ImGuiTableColumnFlags_WidthStretch, 0.24f);

        // ----------------------------------------------------
        // Column 1: Strobe Tuner
        // ----------------------------------------------------
        ImGui::TableNextColumn();
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

        // Strobe Needle Gauge
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float gaugeW = std::clamp(ImGui::GetContentRegionAvail().x - 10.0f, 140.0f, 220.0f);
            const float gaugeH = 18.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            dl->AddRectFilled(pos, ImVec2(pos.x + gaugeW, pos.y + gaugeH), IM_COL32(24, 26, 32, 255), 4.0f);
            dl->AddRect(pos, ImVec2(pos.x + gaugeW, pos.y + gaugeH), IM_COL32(50, 54, 66, 255), 4.0f);

            const float midX = pos.x + (gaugeW * 0.5f);
            dl->AddLine(ImVec2(midX, pos.y + 2), ImVec2(midX, pos.y + gaugeH - 2), IM_COL32(40, 200, 80, 220), 2.0f);
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
                dl->AddCircleFilled(ImVec2(midX, pos.y + gaugeH * 0.5f), 3.0f, IM_COL32(90, 95, 110, 180));
            }
            ImGui::Dummy(ImVec2(gaugeW, gaugeH));
        }

        // ----------------------------------------------------
        // Column 2: Metronome
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 1.0f), "METRONOME");
        bool metroPlaying = m_metronome.isPlaying();
        if (ImGui::Button(metroPlaying ? " STOP " : " PLAY ")) {
            m_metronome.setPlaying(!metroPlaying);
        }
        ImGui::SameLine(0, 8);

        float bpm = m_metronome.bpm();
        ImGui::SetNextItemWidth(75);
        if (ImGui::DragFloat("##BPM", &bpm, 1.0f, 40.0f, 260.0f, "%.0f BPM")) {
            m_metronome.setBpm(bpm);
        }
        ImGui::SameLine(0, 6);

        // Time Signature selector (4/4, 3/4, 2/4, 6/8)
        const char* timeSigOptions[] = { "4/4", "3/4", "2/4", "6/8" };
        const int timeSigBeats[] = { 4, 3, 2, 6 };
        int currentSigIdx = 0;
        int currentBeats = m_metronome.beatsPerBar();
        for (int i = 0; i < 4; ++i) {
            if (timeSigBeats[i] == currentBeats) {
                currentSigIdx = i;
                break;
            }
        }
        ImGui::SetNextItemWidth(55);
        if (ImGui::Combo("##TimeSig", &currentSigIdx, timeSigOptions, 4)) {
            m_metronome.setBeatsPerBar(timeSigBeats[currentSigIdx]);
        }
        ImGui::SameLine(0, 6);

        // Vector Beat LEDs
        {
            const int totalBeats = m_metronome.beatsPerBar();
            const int currentBeat = m_metronome.currentBeat();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 curPos = ImGui::GetCursorScreenPos();
            const float dotRadius = 4.5f;
            const float dotSpacing = 12.0f;

            for (int b = 0; b < totalBeats; ++b) {
                float cx = curPos.x + 5.0f + (b * dotSpacing);
                float cy = curPos.y + 11.0f;
                bool active = metroPlaying && (b == currentBeat);

                if (active) {
                    ImU32 haloColor = (b == 0) ? IM_COL32(255, 60, 60, 80) : IM_COL32(250, 160, 30, 80);
                    dl->AddCircleFilled(ImVec2(cx, cy), dotRadius + 3.0f, haloColor);
                    ImU32 ledColor = (b == 0) ? IM_COL32(255, 70, 70, 255) : IM_COL32(255, 180, 40, 255);
                    dl->AddCircleFilled(ImVec2(cx, cy), dotRadius, ledColor);
                } else {
                    dl->AddCircleFilled(ImVec2(cx, cy), dotRadius, IM_COL32(36, 40, 48, 255));
                    dl->AddCircle(ImVec2(cx, cy), dotRadius, IM_COL32(55, 60, 72, 255), 0, 1.0f);
                }
            }
            ImGui::Dummy(ImVec2(totalBeats * dotSpacing + 8.0f, 22.0f));
        }

        // ----------------------------------------------------
        // Column 3: Noise Gate
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 1.0f), "NOISE GATE");
        auto& gate = m_graph.inputNoiseGate();
        bool gateOn = gate.isEnabled();
        if (ImGui::Checkbox("Active##Gate", &gateOn)) {
            gate.setEnabled(gateOn);
        }
        ImGui::SameLine(0, 8);
        float thresh = gate.thresholdDb();
        ImGui::SetNextItemWidth(80);
        if (ImGui::SliderFloat("##GateThresh", &thresh, -80.0f, -20.0f, "%.0f dB")) {
            gate.setThresholdDb(thresh);
        }

        // ----------------------------------------------------
        // Column 4: Master Output
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::TextColored(ImVec4(0.80f, 0.82f, 0.88f, 1.0f), "MASTER OUTPUT");
        float masterVol = m_graph.masterVolumeDb();
        ImGui::SetNextItemWidth(100);
        if (ImGui::SliderFloat("##MasterVol", &masterVol, -36.0f, +6.0f, "%.1f dB")) {
            m_graph.setMasterVolumeDb(masterVol);
        }
        ImGui::SameLine(0, 6);
        renderMeter("OutMeterL", m_graph.outputMeter().peakLeft(), 10, 30);
        ImGui::SameLine(0, 3);
        renderMeter("OutMeterR", m_graph.outputMeter().peakRight(), 10, 30);

        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void RackView::renderSceneBar() {
    ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.85f, 1.0f), "SCENE PRESETS:");
    ImGui::SameLine(0, 12);

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
            m_sceneFeedbackMsg = "Loaded " + sc->name;
            m_sceneFeedbackTimer = 2.5f;
        }

        if (isActive) {
            ImGui::PopStyleColor(2);
        }
        ImGui::SameLine(0, 8);
    }

    if (ImGui::Button("[Save Scene]")) {
        m_scenes.captureCurrentScene(m_scenes.activeSceneIndex(), m_graph);
        const auto* cur = m_scenes.getScene(m_scenes.activeSceneIndex());
        m_sceneFeedbackMsg = "Saved to " + (cur ? cur->name : "Scene");
        m_sceneFeedbackTimer = 2.5f;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Save current plugin chain into active scene preset");
    }

    ImGui::SameLine(0, 10);
    if (ImGui::Button("[Save Preset As...]")) {
        m_showSavePresetModal = true;
        m_presetNameBuffer[0] = '\0';
    }

    auto userPresets = m_scenes.savedPresetNames();
    if (!userPresets.empty()) {
        ImGui::SameLine(0, 8);
        ImGui::SetNextItemWidth(140);
        if (ImGui::BeginCombo("##UserPresetsCombo", "Load Preset...")) {
            for (const auto& pName : userPresets) {
                if (ImGui::Selectable(pName.c_str())) {
                    m_scenes.loadPresetChain(pName, m_graph);
                    m_sceneFeedbackMsg = "Loaded preset: " + pName;
                    m_sceneFeedbackTimer = 2.5f;
                }
            }
            ImGui::EndCombo();
        }
    }

    if (m_sceneFeedbackTimer > 0.0f) {
        m_sceneFeedbackTimer -= ImGui::GetIO().DeltaTime;
        ImGui::SameLine(0, 16);
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetTextLineHeight();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddCircleFilled(ImVec2(p.x + 5.0f, p.y + h * 0.5f), 4.0f, IM_COL32(50, 220, 100, 255));
        dl->AddCircle(ImVec2(p.x + 5.0f, p.y + h * 0.5f), 6.5f, IM_COL32(50, 220, 100, 90));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
        ImGui::TextColored(ImVec4(0.35f, 0.95f, 0.55f, 1.0f), "%s", m_sceneFeedbackMsg.c_str());
    }

    // Modal for Save Preset As...
    if (m_showSavePresetModal) {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(360, 150), ImGuiCond_Appearing);
        if (ImGui::Begin("Save Preset Chain##Modal", &m_showSavePresetModal, ImGuiWindowFlags_NoCollapse)) {
            ImGui::Text("Enter preset name:");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##NewPresetNameInput", m_presetNameBuffer, sizeof(m_presetNameBuffer));
            ImGui::Spacing();
            if (ImGui::Button("Save", ImVec2(100, 24))) {
                if (m_presetNameBuffer[0] != '\0') {
                    m_scenes.savePresetChain(m_presetNameBuffer, m_graph);
                    m_sceneFeedbackMsg = std::string("Preset '") + m_presetNameBuffer + "' saved!";
                    m_sceneFeedbackTimer = 2.5f;
                    m_showSavePresetModal = false;
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(80, 24))) {
                m_showSavePresetModal = false;
            }
            ImGui::End();
        }
    }
}

void RackView::renderSignalRack() {
    ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.80f, 1.0f), "SIGNAL CHAIN (RACK):");

    ImGui::BeginChild("RackScrollArea", ImVec2(0, -42), true, ImGuiWindowFlags_HorizontalScrollbar);

    // 1. Input Node Card with Channel Selector
    renderInputCard();

    // 2. Hardware Signal Cable & Arrow
    renderSignalCable(42.0f);

    // 3. Render Serial Nodes in Graph
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

        renderSignalCable(42.0f);
    }

    // 3.5. Serial Rack Insertion Slot Card (+ Add Plugin)
    {
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.15f, 0.19f, 0.6f));
        ImGui::BeginChild("InsertSerialCard", ImVec2(92, 230), true);
        ImGui::SetCursorPosY(85);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.26f, 0.36f, 0.85f));
        if (ImGui::Button("+ Add\nPlugin", ImVec2(76, 55))) {
            m_insertTargetBlockIndex = -1;
            m_insertTargetBranchIndex = -1;
            m_showPluginBrowser = true;
            m_focusPluginBrowser = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Insert a new plugin into the serial rack");
        }
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        renderSignalCable(42.0f);
    }

    // 4. Output Node Card
    {
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
        ImGui::BeginChild("OutputDestNode", ImVec2(100, 230), true);
        ImGui::TextColored(ImVec4(0.35f, 0.80f, 1.0f, 1.0f), "[ OUTPUT ]");
        ImGui::TextDisabled("To ASIO");
        ImGui::Spacing();
        renderMeter("FinalL", m_graph.outputMeter().peakLeft(), 14, 135);
        ImGui::SameLine(0, 6);
        renderMeter("FinalR", m_graph.outputMeter().peakRight(), 14, 135);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::EndGroup();
    }

    ImGui::EndChild();
}

void RackView::renderSignalCable(float width) {
    ImGui::SameLine(0, 0);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float cardH = 230.0f;
    const float centerY = pos.y + (cardH * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Sleek patch cable body
    dl->AddLine(ImVec2(pos.x + 2.0f, centerY), ImVec2(pos.x + width - 8.0f, centerY), IM_COL32(32, 36, 46, 255), 5.0f);
    dl->AddLine(ImVec2(pos.x + 2.0f, centerY), ImVec2(pos.x + width - 8.0f, centerY), IM_COL32(85, 105, 140, 255), 2.5f);

    // Glowing Arrowhead at right tip
    const float arrowSize = 6.0f;
    const float tipX = pos.x + width - 2.0f;
    dl->AddTriangleFilled(
        ImVec2(tipX, centerY),
        ImVec2(tipX - arrowSize - 2.0f, centerY - arrowSize),
        ImVec2(tipX - arrowSize - 2.0f, centerY + arrowSize),
        IM_COL32(110, 140, 195, 255)
    );

    ImGui::Dummy(ImVec2(width, cardH));
    ImGui::SameLine(0, 0);
}

void RackView::renderInputCard() {
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
    ImGui::BeginChild("InputGainNode", ImVec2(180, 230), true);
    ImGui::TextColored(ImVec4(0.35f, 0.80f, 1.0f, 1.0f), "[ INPUT ]");

    // Input Channel Routing Selector (Mono/Stereo)
    auto inputConfig = m_graph.inputRouting();
    const char* routingLabels[] = {
        "Mono: In 1 (L+R)",
        "Mono: In 2 (L+R)",
        "Stereo: In 1+2"
    };

    int currentModeIdx = 0;
    if (inputConfig.mode == audio::InputRoutingMode::MonoLeft) currentModeIdx = 0;
    else if (inputConfig.mode == audio::InputRoutingMode::MonoRight) currentModeIdx = 1;
    else if (inputConfig.mode == audio::InputRoutingMode::Stereo) currentModeIdx = 2;

    ImGui::SetNextItemWidth(155);
    if (ImGui::Combo("##InRoute", &currentModeIdx, routingLabels, 3)) {
        if (currentModeIdx == 0) inputConfig.mode = audio::InputRoutingMode::MonoLeft;
        else if (currentModeIdx == 1) inputConfig.mode = audio::InputRoutingMode::MonoRight;
        else if (currentModeIdx == 2) inputConfig.mode = audio::InputRoutingMode::Stereo;
        m_graph.setInputRouting(inputConfig);

        state::AppConfig cfg;
        cfg.load();
        cfg.inputMode = inputConfig.mode;
        cfg.save();
    }

    ImGui::Spacing();
    float inGain = m_graph.inputGainDb();
    if (ImGui::VSliderFloat("##InGain", ImVec2(24, 120), &inGain, -24.0f, +24.0f, "")) {
        m_graph.setInputGainDb(inGain);
    }
    ImGui::SameLine(0, 8);
    renderMeter("InL", m_graph.inputMeter().peakLeft(), 10, 120);
    ImGui::SameLine(0, 4);
    renderMeter("InR", m_graph.inputMeter().peakRight(), 10, 120);

    ImGui::SameLine(0, 8);
    ImGui::BeginGroup();
    ImGui::Text("%.0fdB", inGain);
    ImGui::TextDisabled("Gain");
    ImGui::EndGroup();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

void RackView::renderPluginSlot(audio::PluginSlot* slot, int slotIndex, int branchIndex) {
    if (!slot) return;

    ImGui::BeginGroup();
    const bool bypassed = slot->isBypassed();
    ImVec4 cardBg = bypassed ? ImVec4(0.12f, 0.13f, 0.16f, 0.95f) : ImVec4(0.16f, 0.18f, 0.23f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg);
    char childId[64];
    std::snprintf(childId, sizeof(childId), "SlotCard_%d_%d", slotIndex, branchIndex);

    const float cardWidth = 240.0f;
    const float cardHeight = 230.0f;
    ImGui::BeginChild(childId, ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar);

    auto* pluginInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    bool isWindowOpen = pluginInst ? plugins::PluginWindowManager::instance().isWindowOpen(pluginInst) : false;

    // ----------------------------------------------------
    // Row 1: Header (Plugin Name + [|| Split] + [X] Delete)
    // ----------------------------------------------------
    {
        std::string displayName = slot->name();
        if (displayName.length() > 16) {
            displayName = displayName.substr(0, 15) + "..";
        }

        ImGui::TextColored(bypassed ? ImVec4(0.55f, 0.58f, 0.65f, 1.0f) : ImVec4(0.98f, 0.98f, 1.0f, 1.0f),
                           "%s", displayName.c_str());

        if (branchIndex == -1) {
            // Split into Parallel button
            ImGui::SameLine(cardWidth - 66.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.32f, 0.44f, 0.85f));
            char splitBtnId[32];
            std::snprintf(splitBtnId, sizeof(splitBtnId), "||##Sp_%d", slotIndex);
            if (ImGui::Button(splitBtnId, ImVec2(24, 20))) {
                m_graph.splitSerialNodeIntoParallel(slotIndex);
                ImGui::PopStyleColor();
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                return;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split into Parallel A/B Branches");
            ImGui::PopStyleColor();

            // Delete slot button
            ImGui::SameLine(0, 4);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.52f, 0.20f, 0.20f, 0.85f));
            char delBtnId[32];
            std::snprintf(delBtnId, sizeof(delBtnId), "X##Del_%d", slotIndex);
            if (ImGui::Button(delBtnId, ImVec2(22, 20))) {
                if (pluginInst) {
                    plugins::PluginWindowManager::instance().closePluginWindow(pluginInst);
                }
                m_graph.removeSerialNode(slotIndex);
                ImGui::PopStyleColor();
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                return;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete Plugin Slot");
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();

    // ----------------------------------------------------
    // Row 2: Rich Hardware Faceplate & GUI Preview Screen
    // ----------------------------------------------------
    {
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
        const float previewW = cardWidth - 16.0f;
        const float previewH = 96.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Faceplate outer chassis
        ImU32 faceplateBg = bypassed ? IM_COL32(18, 20, 26, 255) : IM_COL32(23, 26, 35, 255);
        ImU32 faceplateBorder = isWindowOpen ? IM_COL32(40, 210, 80, 255) : IM_COL32(45, 52, 68, 255);
        dl->AddRectFilled(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), faceplateBg, 5.0f);
        dl->AddRect(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), faceplateBorder, 5.0f, 0, isWindowOpen ? 1.5f : 1.0f);

        // Rack screw rivets at top corners
        dl->AddCircleFilled(ImVec2(previewPos.x + 6.0f, previewPos.y + 6.0f), 2.0f, IM_COL32(65, 70, 85, 255));
        dl->AddCircleFilled(ImVec2(previewPos.x + previewW - 6.0f, previewPos.y + 6.0f), 2.0f, IM_COL32(65, 70, 85, 255));

        // Format tag & Vendor header
        std::string formatTag = "[DSP]";
        ImU32 tagColor = IM_COL32(245, 170, 45, 255);
        std::string vendorText = "Built-In";

        if (pluginInst) {
            if (dynamic_cast<plugins::Vst3PluginInstance*>(pluginInst)) {
                formatTag = "[VST3]";
                tagColor = IM_COL32(65, 185, 255, 255);
            } else {
                formatTag = "[CLAP]";
                tagColor = IM_COL32(220, 110, 240, 255);
            }
            vendorText = pluginInst->vendor();
            if (vendorText.length() > 14) vendorText = vendorText.substr(0, 13) + "..";
        }

        dl->AddText(ImVec2(previewPos.x + 12.0f, previewPos.y + 6.0f), tagColor, formatTag.c_str());
        dl->AddText(ImVec2(previewPos.x + 58.0f, previewPos.y + 6.0f), IM_COL32(130, 138, 155, 255), vendorText.c_str());

        // Top right of faceplate: GUI Button / Status Lamp
        const float guiBtnW = 68.0f;
        const float guiBtnH = 18.0f;
        const ImVec2 guiBtnPos(previewPos.x + previewW - guiBtnW - 8.0f, previewPos.y + 5.0f);

        // Clickable region for OPEN GUI button
        ImGui::SetCursorScreenPos(guiBtnPos);
        char guiBtnId[32];
        std::snprintf(guiBtnId, sizeof(guiBtnId), "##GuiBtn_%d_%d", slotIndex, branchIndex);
        if (ImGui::InvisibleButton(guiBtnId, ImVec2(guiBtnW, guiBtnH))) {
            if (pluginInst) {
                plugins::PluginWindowManager::instance().openPluginWindow(pluginInst);
            } else {
                m_dspTweakSlot = slot;
            }
        }
        bool isGuiHovered = ImGui::IsItemHovered();
        if (isGuiHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip(pluginInst ? "Open / Focus %s Native GUI" : "Open DSP Parameters", slot->name().c_str());
        }

        // Render GUI button pill with glowing LED
        ImU32 pillBg = isWindowOpen ? IM_COL32(24, 60, 36, 255) : (isGuiHovered ? IM_COL32(40, 50, 70, 255) : IM_COL32(28, 34, 46, 255));
        ImU32 pillBorder = isWindowOpen ? IM_COL32(50, 220, 100, 255) : (isGuiHovered ? IM_COL32(90, 130, 190, 255) : IM_COL32(55, 62, 80, 255));
        dl->AddRectFilled(guiBtnPos, ImVec2(guiBtnPos.x + guiBtnW, guiBtnPos.y + guiBtnH), pillBg, 3.0f);
        dl->AddRect(guiBtnPos, ImVec2(guiBtnPos.x + guiBtnW, guiBtnPos.y + guiBtnH), pillBorder, 3.0f);

        if (isWindowOpen) {
            dl->AddCircleFilled(ImVec2(guiBtnPos.x + 8.0f, guiBtnPos.y + 9.0f), 3.0f, IM_COL32(50, 230, 90, 255));
            dl->AddText(ImVec2(guiBtnPos.x + 16.0f, guiBtnPos.y + 3.0f), IM_COL32(90, 245, 130, 255), "WINDOW");
        } else {
            dl->AddCircleFilled(ImVec2(guiBtnPos.x + 8.0f, guiBtnPos.y + 9.0f), 2.5f, IM_COL32(120, 130, 150, 255));
            dl->AddText(ImVec2(guiBtnPos.x + 16.0f, guiBtnPos.y + 3.0f), isGuiHovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(160, 170, 190, 255), "OPEN GUI");
        }

        // Inner Divider
        dl->AddLine(ImVec2(previewPos.x + 8.0f, previewPos.y + 26.0f),
                    ImVec2(previewPos.x + previewW - 8.0f, previewPos.y + 26.0f),
                    IM_COL32(36, 42, 56, 255), 1.0f);

        // Center / Main section: Rotary Parameter Knobs
        size_t numParams = pluginInst ? pluginInst->numParameters() : 0;
        if (numParams > 0) {
            size_t displayCount = std::min(numParams, size_t(3));
            float knobRadius = 13.0f;
            float stepX = (previewW - 16.0f) / static_cast<float>(displayCount);

            for (size_t p = 0; p < displayCount; ++p) {
                auto desc = pluginInst->getParameterDesc(p);
                float val = pluginInst->getParameterValue(desc.id);
                float kCenterX = previewPos.x + 8.0f + (p + 0.5f) * stepX;
                float kCenterY = previewPos.y + 52.0f;

                ImGui::SetCursorScreenPos(ImVec2(kCenterX - knobRadius - 4.0f, kCenterY - knobRadius - 4.0f));
                char knobId[64];
                std::snprintf(knobId, sizeof(knobId), "##Knob_%d_%d_%zu", slotIndex, branchIndex, p);

                float minV = desc.minValue;
                float maxV = (desc.maxValue > desc.minValue) ? desc.maxValue : desc.minValue + 1.0f;

                float knobVal = val;
                if (renderRotaryKnob(knobId, &knobVal, minV, maxV, knobRadius)) {
                    pluginInst->setParameterValue(desc.id, knobVal);
                }

                // Param label below knob
                std::string pName = desc.name;
                if (pName.length() > 6) pName = pName.substr(0, 5) + ".";
                ImVec2 tSz = ImGui::CalcTextSize(pName.c_str());
                dl->AddText(ImVec2(kCenterX - tSz.x * 0.5f, previewPos.y + 76.0f),
                            IM_COL32(160, 168, 185, 255), pName.c_str());
            }
        } else {
            // Live dynamic oscilloscope waveform & EQ curve
            float leftPeak = slot->meter().peakLeft();
            float rightPeak = slot->meter().peakRight();
            float activity = std::max(leftPeak, rightPeak);

            const int numPoints = 24;
            float step = (previewW - 24.0f) / static_cast<float>(numPoints - 1);
            float startX = previewPos.x + 12.0f;
            float midY = previewPos.y + 58.0f;
            float amp = std::min(20.0f, 5.0f + activity * 35.0f);

            static float s_phase = 0.0f;
            s_phase += 0.04f;

            ImVec2 lastPt(startX, midY);
            for (int pt = 0; pt < numPoints; ++pt) {
                float px = startX + pt * step;
                float py = midY + std::sin(s_phase + pt * 0.45f) * amp * (0.3f + 0.7f * std::sin((pt / (float)numPoints) * 3.14159f));
                if (pt > 0) {
                    ImU32 waveCol = bypassed ? IM_COL32(70, 75, 90, 180) : IM_COL32(40, 200, 230, 220);
                    dl->AddLine(lastPt, ImVec2(px, py), waveCol, 1.8f);
                }
                lastPt = ImVec2(px, py);
            }

            // Click faceplate prompt
            ImVec2 pPrompt = ImVec2(previewPos.x + previewW * 0.5f - 40.0f, previewPos.y + 76.0f);
            dl->AddText(pPrompt, IM_COL32(140, 150, 175, 200), "[ CLICK TO EDIT ]");

            // Invisible button covering the area so clicking opens GUI
            ImGui::SetCursorScreenPos(ImVec2(startX, previewPos.y + 28.0f));
            char waveBtnId[32];
            std::snprintf(waveBtnId, sizeof(waveBtnId), "##WaveClick_%d_%d", slotIndex, branchIndex);
            if (ImGui::InvisibleButton(waveBtnId, ImVec2(previewW - 24.0f, 58.0f))) {
                if (pluginInst) plugins::PluginWindowManager::instance().openPluginWindow(pluginInst);
                else m_dspTweakSlot = slot;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }

        // Return cursor below the faceplate preview
        ImGui::SetCursorScreenPos(ImVec2(previewPos.x, previewPos.y + previewH + 4.0f));
    }

    ImGui::Spacing();

    // ----------------------------------------------------
    // Row 3: Stompbox Active / Bypass Button & Dry/Wet Slider
    // ----------------------------------------------------
    {
        bool active = !bypassed;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(35, 75, 45, 255));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 250, 150, 255));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 54, 65, 255));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 155, 170, 255));
        }

        char btnId[32];
        std::snprintf(btnId, sizeof(btnId), "%s##Btn_%d_%d", active ? "ACTIVE" : "BYPASS", slotIndex, branchIndex);
        if (ImGui::Button(btnId, ImVec2(64, 22))) {
            slot->setBypassed(active);
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 8);

        // Dry / Wet Slider
        float mix = slot->dryWet() * 100.0f;
        char mixLabel[32];
        std::snprintf(mixLabel, sizeof(mixLabel), "##Mix_%d_%d", slotIndex, branchIndex);
        ImGui::SetNextItemWidth(96);
        if (ImGui::SliderFloat(mixLabel, &mix, 0.0f, 100.0f, "Mix: %.0f%%")) {
            slot->setDryWet(mix / 100.0f);
        }
        ImGui::SameLine(0, 6);
        renderMeter("SlotMtr", slot->meter().peakLeft(), 8, 22);
    }

    // ----------------------------------------------------
    // Row 4: Output Trim Slider
    // ----------------------------------------------------
    {
        float outTrim = slot->outputGainDb();
        char trimLabel[32];
        std::snprintf(trimLabel, sizeof(trimLabel), "##Trim_%d_%d", slotIndex, branchIndex);
        ImGui::SetNextItemWidth(cardWidth - 20.0f);
        if (ImGui::SliderFloat(trimLabel, &outTrim, -24.0f, +12.0f, "Trim: %+.1f dB")) {
            slot->setOutputGainDb(outTrim);
        }
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
        if (branchIndex == -1 && ImGui::MenuItem("Delete Slot")) {
            if (pluginInst) plugins::PluginWindowManager::instance().closePluginWindow(pluginInst);
            m_graph.removeSerialNode(slotIndex);
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

void RackView::renderMiniHardwareSlot(audio::PluginSlot* slot, int blockIndex, size_t branchIndex, size_t slotIndex) {
    if (!slot) return;

    auto* pInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    bool isBypassed = slot->isBypassed();
    bool isOpen = pInst ? plugins::PluginWindowManager::instance().isWindowOpen(pInst) : false;

    const float slotW = 145.0f;
    const float slotH = 50.0f;

    char subId[64];
    std::snprintf(subId, sizeof(subId), "BSlot_%d_%zu_%zu", blockIndex, branchIndex, slotIndex);

    ImVec4 bgCol = isBypassed ? ImVec4(0.12f, 0.13f, 0.16f, 0.8f) : ImVec4(0.20f, 0.24f, 0.32f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bgCol);
    ImGui::BeginChild(subId, ImVec2(slotW, slotH), true, ImGuiWindowFlags_NoScrollbar);

    // Row 1: Name and Delete button
    std::string sName = slot->name();
    if (sName.length() > 11) sName = sName.substr(0, 10) + "..";
    ImGui::TextColored(isBypassed ? ImVec4(0.6f, 0.6f, 0.6f, 1.0f) : ImVec4(0.95f, 0.95f, 1.0f, 1.0f),
                       "%s", sName.c_str());

    ImGui::SameLine(slotW - 30.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.48f, 0.18f, 0.18f, 0.85f));
    char rmBtnId[32];
    std::snprintf(rmBtnId, sizeof(rmBtnId), "x##rm_%d_%zu_%zu", blockIndex, branchIndex, slotIndex);
    if (ImGui::Button(rmBtnId, ImVec2(18, 16))) {
        if (pInst) plugins::PluginWindowManager::instance().closePluginWindow(pInst);
        auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(blockIndex));
        if (block) {
            auto* branch = block->getBranch(branchIndex);
            if (branch) branch->removeSlot(slotIndex);
        }
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }
    ImGui::PopStyleColor();

    // Row 2: [ACTIVE/BYP] and [GUI] buttons
    char bypId[32];
    std::snprintf(bypId, sizeof(bypId), "%s##byp_%d_%zu_%zu", isBypassed ? "BYP" : "ON", blockIndex, branchIndex, slotIndex);
    if (isBypassed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.28f, 0.33f, 1.0f));
    else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.25f, 1.0f));

    if (ImGui::Button(bypId, ImVec2(32, 18))) {
        slot->setBypassed(!isBypassed);
    }
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 4);
    char guiId[32];
    std::snprintf(guiId, sizeof(guiId), "%s##gui_%d_%zu_%zu", isOpen ? "WINDOW" : "GUI", blockIndex, branchIndex, slotIndex);
    if (isOpen) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.45f, 0.25f, 1.0f));
    else ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.30f, 0.42f, 1.0f));

    if (ImGui::Button(guiId, ImVec2(52, 18))) {
        if (pInst) plugins::PluginWindowManager::instance().openPluginWindow(pInst);
        else m_dspTweakSlot = slot;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s (Click to open GUI)", slot->name().c_str());
    }
    ImGui::PopStyleColor();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void RackView::renderParallelBlock(audio::ParallelSplitMergeBlock* block, int blockIndex) {
    if (!block) return;

    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.15f, 0.19f, 1.0f));
    char blockChildId[64];
    std::snprintf(blockChildId, sizeof(blockChildId), "ParBlock_%d", blockIndex);

    // Dynamic width based on slots in branches
    size_t maxBranchSlots = 0;
    for (size_t b = 0; b < block->numBranches(); ++b) {
        auto* br = block->getBranch(b);
        if (br && br->numSlots() > maxBranchSlots) maxBranchSlots = br->numSlots();
    }
    const float blockW = std::max(560.0f, 420.0f + static_cast<float>(maxBranchSlots) * 155.0f);
    const float blockH = 230.0f;

    ImGui::BeginChild(blockChildId, ImVec2(blockW, blockH), true, ImGuiWindowFlags_NoScrollbar);

    // ----------------------------------------------------
    // Header: Title + [X Remove Split Block]
    // ----------------------------------------------------
    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1.0f), "// PARALLEL SPLIT (A / B)");
    ImGui::SameLine(0, 16);
    ImGui::TextDisabled("| Dual-Path Audio Routing (Summed to Stereo)");

    ImGui::SameLine(blockW - 145.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f, 0.20f, 0.20f, 0.85f));
    char remBlockId[32];
    std::snprintf(remBlockId, sizeof(remBlockId), "[X Remove Split]##%d", blockIndex);
    if (ImGui::Button(remBlockId)) {
        for (size_t b = 0; b < block->numBranches(); ++b) {
            auto* br = block->getBranch(b);
            if (br) {
                for (size_t s = 0; s < br->numSlots(); ++s) {
                    auto* slot = br->getSlot(s);
                    if (slot) {
                        auto* pInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
                        if (pInst) plugins::PluginWindowManager::instance().closePluginWindow(pInst);
                    }
                }
            }
        }
        m_graph.removeSerialNode(blockIndex);
        ImGui::PopStyleColor(2);
        ImGui::EndChild();
        ImGui::EndGroup();
        return;
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();

    // ----------------------------------------------------
    // Branch Lanes (Branch A, Branch B)
    // ----------------------------------------------------
    for (size_t b = 0; b < block->numBranches(); ++b) {
        auto* branch = block->getBranch(b);
        if (!branch) continue;

        ImGui::PushID(static_cast<int>(b));

        // Dedicated sub-container for this branch
        char laneChildId[64];
        std::snprintf(laneChildId, sizeof(laneChildId), "Lane_%d_%zu", blockIndex, b);
        ImVec4 laneBg = (b == 0) ? ImVec4(0.16f, 0.19f, 0.25f, 0.6f) : ImVec4(0.19f, 0.17f, 0.23f, 0.6f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, laneBg);
        ImGui::BeginChild(laneChildId, ImVec2(blockW - 18.0f, 92.0f), true, ImGuiWindowFlags_NoScrollbar);

        // --- Row 1: Branch Mixer Strip ---
        ImVec4 titleCol = (b == 0) ? ImVec4(0.40f, 0.85f, 1.0f, 1.0f) : ImVec4(1.0f, 0.75f, 0.35f, 1.0f);
        ImGui::TextColored(titleCol, "[ %s ]", branch->name().c_str());
        ImGui::SameLine(0, 10);

        // Mute button
        bool muted = branch->isMuted();
        char muteLabel[32];
        std::snprintf(muteLabel, sizeof(muteLabel), "M##%d_%zu", blockIndex, b);
        if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button(muteLabel, ImVec2(22, 20))) {
            branch->setMuted(!muted);
        }
        if (muted) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mute %s", branch->name().c_str());

        // Solo button
        ImGui::SameLine(0, 4);
        bool solo = branch->isSolo();
        char soloLabel[32];
        std::snprintf(soloLabel, sizeof(soloLabel), "S##%d_%zu", blockIndex, b);
        if (solo) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.7f, 0.1f, 1.0f));
        if (ImGui::Button(soloLabel, ImVec2(22, 20))) {
            branch->setSolo(!solo);
        }
        if (solo) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Solo %s", branch->name().c_str());

        // Phase Invert button (Ø)
        ImGui::SameLine(0, 4);
        bool phaseInv = branch->isPhaseInvert();
        char phaseLabel[32];
        std::snprintf(phaseLabel, sizeof(phaseLabel), "O##%d_%zu", blockIndex, b);
        if (phaseInv) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.15f, 1.0f));
        if (ImGui::Button(phaseLabel, ImVec2(22, 20))) {
            branch->setPhaseInvert(!phaseInv);
        }
        if (phaseInv) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Phase Invert (180 deg polarity flip)");

        // Pan Slider
        ImGui::SameLine(0, 10);
        float pan = branch->pan();
        char panLabel[32];
        std::snprintf(panLabel, sizeof(panLabel), "##Pan_%d_%zu", blockIndex, b);
        ImGui::SetNextItemWidth(75);
        if (ImGui::SliderFloat(panLabel, &pan, -1.0f, +1.0f, "Pan: %+.2f")) {
            branch->setPan(pan);
        }

        // Gain Slider
        ImGui::SameLine(0, 6);
        float gain = branch->gainDb();
        char gainLabel[32];
        std::snprintf(gainLabel, sizeof(gainLabel), "##Gain_%d_%zu", blockIndex, b);
        ImGui::SetNextItemWidth(75);
        if (ImGui::SliderFloat(gainLabel, &gain, -36.0f, +12.0f, "%+.1f dB")) {
            branch->setGainDb(gain);
        }

        // --- Row 2: Slots Chain Inside This Branch ---
        ImGui::Spacing();
        if (branch->numSlots() == 0) {
            ImGui::TextDisabled("Dry pass-through");
            ImGui::SameLine(0, 12);
        } else {
            for (size_t s = 0; s < branch->numSlots(); ++s) {
                auto* bSlot = branch->getSlot(s);
                if (!bSlot) continue;

                renderMiniHardwareSlot(bSlot, blockIndex, b, s);
                ImGui::SameLine(0, 6);
                ImGui::TextColored(ImVec4(0.4f, 0.5f, 0.65f, 1.0f), "->");
                ImGui::SameLine(0, 6);
            }
        }

        // [+ Add Plugin] Button
        char addBtnId[32];
        std::snprintf(addBtnId, sizeof(addBtnId), "+ Add Plugin##br_%d_%zu", blockIndex, b);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.28f, 0.38f, 0.85f));
        if (ImGui::Button(addBtnId, ImVec2(92, 22))) {
            m_insertTargetBlockIndex = blockIndex;
            m_insertTargetBranchIndex = static_cast<int>(b);
            m_showPluginBrowser = true;
            m_focusPluginBrowser = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Insert plugin into %s", branch->name().c_str());
        }
        ImGui::PopStyleColor();

        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::PopID();
        if (b + 1 < block->numBranches()) {
            ImGui::Spacing();
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

bool RackView::renderRotaryKnob(const char* label, float* value, float minVal, float maxVal, float radius, const char* format) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();

    float diameter = radius * 2.0f;
    ImVec2 center(pos.x + radius + 4.0f, pos.y + radius + 4.0f);

    // Invisible button to capture drag & clicks
    ImGui::InvisibleButton(label, ImVec2(diameter + 8.0f, diameter + 8.0f));
    bool isHovered = ImGui::IsItemHovered();
    bool isActive = ImGui::IsItemActive();

    bool valueChanged = false;
    if (isActive) {
        float speed = 0.005f * (maxVal - minVal);
        float delta = -io.MouseDelta.y * speed;
        if (delta != 0.0f) {
            *value = std::clamp(*value + delta, minVal, maxVal);
            valueChanged = true;
        }
    }

    if (isHovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        char tip[64];
        std::snprintf(tip, sizeof(tip), format, *value);
        ImGui::SetTooltip("%s", tip);
    }

    // Normalized value (0.0 to 1.0)
    float norm = (*value - minVal) / (maxVal - minVal);
    norm = std::clamp(norm, 0.0f, 1.0f);

    // Knob Outer Shadow / Ring
    dl->AddCircleFilled(center, radius + 2.0f, IM_COL32(16, 18, 24, 255));
    dl->AddCircle(center, radius + 2.0f, isActive ? IM_COL32(245, 150, 40, 255) : (isHovered ? IM_COL32(100, 140, 200, 255) : IM_COL32(40, 45, 58, 255)), 0, 1.5f);

    // Dark brushed aluminum knob cap
    dl->AddCircleFilled(center, radius, IM_COL32(32, 36, 46, 255));

    // Outer Indicator Arc (135° to 405° = 270° sweep)
    const float startAngle = 2.35619449f; // 135 deg in radians (3*pi/4)
    const int arcSegments = 16;
    for (int s = 0; s < arcSegments; ++s) {
        float f1 = s / static_cast<float>(arcSegments);
        float f2 = (s + 1) / static_cast<float>(arcSegments);
        if (f1 > norm) break;
        float a1 = startAngle + f1 * 4.71238898f;
        float a2 = startAngle + std::min(norm, f2) * 4.71238898f;
        dl->PathLineTo(ImVec2(center.x + std::cos(a1) * (radius - 1.0f), center.y + std::sin(a1) * (radius - 1.0f)));
        dl->PathLineTo(ImVec2(center.x + std::cos(a2) * (radius - 1.0f), center.y + std::sin(a2) * (radius - 1.0f)));
        dl->PathStroke(isActive ? IM_COL32(255, 165, 45, 255) : IM_COL32(65, 185, 255, 220), 0, 2.0f);
    }

    // Pointer notch / needle on the knob
    float needleAngle = startAngle + norm * 4.71238898f;
    ImVec2 needleInner(center.x + std::cos(needleAngle) * (radius * 0.35f), center.y + std::sin(needleAngle) * (radius * 0.35f));
    ImVec2 needleOuter(center.x + std::cos(needleAngle) * (radius - 2.0f), center.y + std::sin(needleAngle) * (radius - 2.0f));
    dl->AddLine(needleInner, needleOuter, IM_COL32(255, 255, 255, 255), 2.0f);

    return valueChanged;
}

void RackView::renderBottomBar() {
    ImGui::Separator();
    if (m_midi.isLearning()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[*] MIDI LEARN ACTIVE: Move any knob, slider, or press a footswitch to bind...");
        ImGui::SameLine(0, 12);
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

    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(26, 28, 34, 255), 2.0f);

    float db = audio::DspUtils::gainToDb(level);
    float norm = (db + 60.0f) / 60.0f;
    norm = std::clamp(norm, 0.0f, 1.0f);

    float fillHeight = height * norm;
    ImU32 meterColor = (db > -0.5f) ? IM_COL32(245, 50, 50, 255) :
                       (db > -6.0f) ? IM_COL32(245, 175, 35, 255) :
                                      IM_COL32(40, 210, 80, 255);

    drawList->AddRectFilled(ImVec2(pos.x, pos.y + height - fillHeight),
                            ImVec2(pos.x + width, pos.y + height),
                            meterColor, 2.0f);

    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), IM_COL32(50, 55, 65, 255), 2.0f);
    ImGui::Dummy(ImVec2(width, height));
}

void RackView::renderPluginBrowserModal() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(880, 620), ImGuiCond_Appearing);
    if (m_focusPluginBrowser) {
        ImGui::SetNextWindowFocus();
        m_focusPluginBrowser = false;
    }
    if (ImGui::Begin("Plugin Manager & Scanner", &m_showPluginBrowser, ImGuiWindowFlags_NoCollapse)) {
        // TOP FILTER & SEARCH BAR
        ImGui::TextColored(ImVec4(0.98f, 0.60f, 0.20f, 1.0f), "Filter:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260);
        ImGui::InputTextWithHint("##PluginFilter", "Search plugins by name or vendor...", m_pluginSearchQuery, sizeof(m_pluginSearchQuery));
        if (m_pluginSearchQuery[0] != '\0') {
            ImGui::SameLine(0, 4);
            if (ImGui::Button("X##ClearSearch")) {
                m_pluginSearchQuery[0] = '\0';
            }
        }

        ImGui::SameLine(0, 16);
        ImGui::Text("Sort:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170);
        const char* sortOptions[] = { "Name (A -> Z)", "Name (Z -> A)", "Developer (A -> Z)", "Format (VST3/CLAP)" };
        ImGui::Combo("##SortCombo", &m_pluginSortMode, sortOptions, IM_ARRAYSIZE(sortOptions));

        ImGui::SameLine(0, 16);
        if (ImGui::Button("Rescan All")) {
            m_scanner.scanAll();
        }
        ImGui::Separator();

        float leftWidth = 230.0f;
        float contentHeight = ImGui::GetContentRegionAvail().y - 36.0f;

        // LEFT PANE: Folder / Developer Tree View
        ImGui::BeginChild("CategoryTreeChild", ImVec2(leftWidth, contentHeight), true);
        {
            ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.85f, 1.0f), "CATEGORIES");
            ImGui::Separator();

            bool isAll = (m_selectedDeveloperFilter == "All" && m_selectedFormatFilter == "All");
            char allLabel[64];
            std::snprintf(allLabel, sizeof(allLabel), "All Plugins (%zu)", m_scanner.numPlugins());
            if (ImGui::Selectable(allLabel, isAll)) {
                m_selectedDeveloperFilter = "All";
                m_selectedFormatFilter = "All";
            }

            ImGui::Spacing();
            if (ImGui::TreeNodeEx("Formats", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool isVst3 = (m_selectedFormatFilter == "VST3" && m_selectedDeveloperFilter == "All");
                if (ImGui::Selectable("VST3", isVst3)) {
                    m_selectedFormatFilter = "VST3";
                    m_selectedDeveloperFilter = "All";
                }
                bool isClap = (m_selectedFormatFilter == "CLAP" && m_selectedDeveloperFilter == "All");
                if (ImGui::Selectable("CLAP", isClap)) {
                    m_selectedFormatFilter = "CLAP";
                    m_selectedDeveloperFilter = "All";
                }
                bool isBuiltin = (m_selectedFormatFilter == "Built-In" && m_selectedDeveloperFilter == "All");
                if (ImGui::Selectable("Built-In", isBuiltin)) {
                    m_selectedFormatFilter = "Built-In";
                    m_selectedDeveloperFilter = "All";
                }
                ImGui::TreePop();
            }

            ImGui::Spacing();
            if (ImGui::TreeNodeEx("Developers", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto devs = m_scanner.getDevelopers();
                for (const auto& dev : devs) {
                    bool isDev = (m_selectedDeveloperFilter == dev);
                    if (ImGui::Selectable(dev.c_str(), isDev)) {
                        m_selectedDeveloperFilter = dev;
                        m_selectedFormatFilter = "All";
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // RIGHT PANE: Filtered Plugin Table
        ImGui::BeginChild("FilteredPluginsChild", ImVec2(0, contentHeight), true);
        {
            auto filtered = m_scanner.getFilteredPlugins(
                m_pluginSearchQuery,
                m_selectedDeveloperFilter,
                m_selectedFormatFilter,
                static_cast<plugins::PluginSortMode>(m_pluginSortMode)
            );

            if (m_insertTargetBlockIndex >= 0 && m_insertTargetBranchIndex >= 0) {
                ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1.0f),
                                   "Target: Parallel Block %d, Branch %d  |  %zu plugins matching",
                                   m_insertTargetBlockIndex + 1, m_insertTargetBranchIndex + 1, filtered.size());
            } else {
                ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, 1.0f),
                                   "Viewing: %s %s  |  %zu plugins matching",
                                   (m_selectedDeveloperFilter != "All" ? ("[" + m_selectedDeveloperFilter + "]").c_str() : ""),
                                   (m_selectedFormatFilter != "All" ? ("[" + m_selectedFormatFilter + "]").c_str() : "All"),
                                   filtered.size());
            }
            ImGui::Separator();

            if (ImGui::BeginTable("PluginsTable", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 75.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.50f);
                ImGui::TableSetupColumn("Developer", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 84.0f);
                ImGui::TableHeadersRow();

                for (size_t p = 0; p < filtered.size(); ++p) {
                    const auto& desc = filtered[p];
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImVec4 badgeCol = (desc.type == plugins::PluginType::VST3) ? ImVec4(0.3f, 0.7f, 1.0f, 1.0f) :
                                      (desc.type == plugins::PluginType::CLAP) ? ImVec4(0.9f, 0.5f, 0.9f, 1.0f) :
                                                                                 ImVec4(0.98f, 0.60f, 0.20f, 1.0f);
                    ImGui::TextColored(badgeCol, "%s", desc.typeString().c_str());

                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    char rowSelectId[128];
                    std::snprintf(rowSelectId, sizeof(rowSelectId), "%s##row_%zu", desc.name.c_str(), p);
                    bool doubleClicked = false;
                    if (ImGui::Selectable(rowSelectId, false, ImGuiSelectableFlags_AllowDoubleClick)) {
                        if (ImGui::IsMouseDoubleClicked(0)) {
                            doubleClicked = true;
                        }
                    }

                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", desc.vendor.c_str());

                    ImGui::TableNextColumn();
                    float availW = ImGui::GetContentRegionAvail().x;
                    if (availW > 74.0f) {
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - 74.0f) * 0.5f);
                    }
                    char btnLabel[32];
                    std::snprintf(btnLabel, sizeof(btnLabel), "+ Insert##%zu", p);
                    bool insertClicked = ImGui::Button(btnLabel, ImVec2(74.0f, 0));

                    if (insertClicked || doubleClicked) {
                        std::unique_ptr<audio::PluginSlot> newSlot;

                        if (desc.type == plugins::PluginType::CLAP) {
                            auto clapInst = plugins::ClapPluginInstance::loadFromFile(desc.path);
                            if (clapInst) {
                                newSlot = std::make_unique<audio::PluginSlot>(std::move(clapInst));
                            }
                        } else if (desc.type == plugins::PluginType::VST3) {
                            auto vst3Inst = plugins::Vst3PluginInstance::loadFromFile(desc.path);
                            if (vst3Inst) {
                                newSlot = std::make_unique<audio::PluginSlot>(std::move(vst3Inst));
                            }
                        } else if (desc.path == "builtin://drive") {
                            newSlot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>());
                        } else if (desc.path == "builtin://amp") {
                            newSlot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::TubeAmpEffect>());
                        } else if (desc.path == "builtin://delay") {
                            newSlot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::StereoDelayEffect>());
                        }

                        if (newSlot) {
                            if (m_insertTargetBlockIndex >= 0 && m_insertTargetBranchIndex >= 0) {
                                auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(m_insertTargetBlockIndex));
                                if (block) {
                                    auto* branch = block->getBranch(m_insertTargetBranchIndex);
                                    if (branch) {
                                        newSlot->prepare(m_graph.sampleRate(), m_graph.maxBlockSize());
                                        branch->addSlot(std::move(newSlot));
                                    }
                                }
                            } else {
                                newSlot->prepare(m_graph.sampleRate(), m_graph.maxBlockSize());
                                m_graph.addSerialNode(std::move(newSlot));
                            }
                            m_showPluginBrowser = false;
                        }
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        // BOTTOM BAR: Manage Search Paths toggle & Close button
        if (ImGui::Button("Search Paths...")) {
            ImGui::OpenPopup("ManageSearchPathsPopup");
        }
        ImGui::SameLine(0, 8);
        ImGui::TextDisabled("(%zu search locations registered)", m_scanner.searchPaths().size());

        ImGui::SameLine(ImGui::GetWindowWidth() - 90);
        if (ImGui::Button("Close", ImVec2(75, 24))) {
            m_showPluginBrowser = false;
        }

        if (ImGui::BeginPopup("ManageSearchPathsPopup")) {
            ImGui::TextColored(ImVec4(0.98f, 0.60f, 0.20f, 1.0f), "Plugin Search Paths");
            ImGui::Separator();
            const auto& paths = m_scanner.searchPaths();
            for (size_t i = 0; i < paths.size(); ++i) {
                ImGui::TextDisabled("[%zu]", i + 1);
                ImGui::SameLine(0, 8);
                ImGui::Text("%s", paths[i].c_str());
                ImGui::SameLine(0, 16);
                char rmLabel[32];
                std::snprintf(rmLabel, sizeof(rmLabel), "Remove##%zu", i);
                if (ImGui::Button(rmLabel)) {
                    m_scanner.removeCustomSearchPath(i);
                    m_scanner.scanAll();
                    state::AppConfig cfg;
                    cfg.load();
                    cfg.customPluginPaths = m_scanner.searchPaths();
                    cfg.save();
                }
            }
            ImGui::Spacing();
            ImGui::SetNextItemWidth(320);
            ImGui::InputTextWithHint("##NewPath", "e.g. D:\\AudioPlugins", m_newPathBuffer, sizeof(m_newPathBuffer));
            ImGui::SameLine(0, 8);
            if (ImGui::Button("+ Add Path")) {
                if (m_newPathBuffer[0] != '\0') {
                    m_scanner.addCustomSearchPath(m_newPathBuffer);
                    m_scanner.scanAll();
                    state::AppConfig cfg;
                    cfg.load();
                    cfg.customPluginPaths = m_scanner.searchPaths();
                    cfg.save();
                    m_newPathBuffer[0] = '\0';
                }
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

void RackView::renderDspTweakModal() {
    if (!m_dspTweakSlot) return;

    auto* pInst = dynamic_cast<plugins::IPluginInstance*>(m_dspTweakSlot->innerNode());
    if (!pInst) {
        m_dspTweakSlot = nullptr;
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(380, 280), ImGuiCond_Appearing);
    bool open = true;
    char titleBuf[64];
    std::snprintf(titleBuf, sizeof(titleBuf), "%s Parameters##DspTweak", m_dspTweakSlot->name().c_str());

    if (ImGui::Begin(titleBuf, &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(0.98f, 0.60f, 0.20f, 1.0f), "%s", m_dspTweakSlot->name().c_str());
        ImGui::TextDisabled("Vendor: %s | Version: %s", pInst->vendor().c_str(), pInst->version().c_str());
        ImGui::Separator();

        const size_t numParams = pInst->numParameters();
        if (numParams == 0) {
            ImGui::TextDisabled("No modifiable parameters available.");
        } else {
            for (size_t i = 0; i < numParams; ++i) {
                auto desc = pInst->getParameterDesc(i);
                float val = pInst->getParameterValue(desc.id);
                char label[64];
                std::snprintf(label, sizeof(label), "%s##p_%zu", desc.name.c_str(), i);
                if (ImGui::SliderFloat(label, &val, desc.minValue, desc.maxValue, "%.2f")) {
                    pInst->setParameterValue(desc.id, val);
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(80, 24))) {
            open = false;
        }
    }
    ImGui::End();

    if (!open) {
        m_dspTweakSlot = nullptr;
    }
}

} // namespace praccy::ui

