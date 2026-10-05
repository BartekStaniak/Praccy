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
            m_insertTargetBlockIndex = -1;
            m_insertTargetBranchIndex = -1;
            m_showPluginBrowser = true;
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
        ImGui::SameLine(0, 8);

        // Vector Beat LEDs
        {
            const int currentBeat = m_metronome.currentBeat();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 curPos = ImGui::GetCursorScreenPos();
            const float dotRadius = 5.0f;
            const float dotSpacing = 14.0f;

            for (int b = 0; b < 4; ++b) {
                float cx = curPos.x + 6.0f + (b * dotSpacing);
                float cy = curPos.y + 12.0f;
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
            ImGui::Dummy(ImVec2(4 * dotSpacing + 8.0f, 22.0f));
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
        }

        if (isActive) {
            ImGui::PopStyleColor(2);
        }
        ImGui::SameLine(0, 8);
    }

    if (ImGui::Button("[Save Scene]")) {
        m_scenes.captureCurrentScene(m_scenes.activeSceneIndex(), m_graph);
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

    // 4. Output Node Card
    {
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
        ImGui::BeginChild("OutputDestNode", ImVec2(100, 185), true);
        ImGui::TextColored(ImVec4(0.35f, 0.80f, 1.0f, 1.0f), "[ OUTPUT ]");
        ImGui::TextDisabled("To ASIO");
        ImGui::Spacing();
        renderMeter("FinalL", m_graph.outputMeter().peakLeft(), 14, 95);
        ImGui::SameLine(0, 6);
        renderMeter("FinalR", m_graph.outputMeter().peakRight(), 14, 95);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::EndGroup();
    }

    ImGui::EndChild();
}

void RackView::renderSignalCable(float width) {
    ImGui::SameLine(0, 0);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float cardH = 185.0f;
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
    ImGui::BeginChild("InputGainNode", ImVec2(180, 185), true);
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
    if (ImGui::VSliderFloat("##InGain", ImVec2(24, 85), &inGain, -24.0f, +24.0f, "")) {
        m_graph.setInputGainDb(inGain);
    }
    ImGui::SameLine(0, 8);
    renderMeter("InL", m_graph.inputMeter().peakLeft(), 10, 85);
    ImGui::SameLine(0, 4);
    renderMeter("InR", m_graph.inputMeter().peakRight(), 10, 85);

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
    ImVec4 cardBg = bypassed ? ImVec4(0.13f, 0.14f, 0.17f, 0.6f) : ImVec4(0.17f, 0.19f, 0.24f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg);
    char childId[64];
    std::snprintf(childId, sizeof(childId), "SlotCard_%d_%d", slotIndex, branchIndex);

    const float cardWidth = 230.0f;
    const float cardHeight = 185.0f;
    ImGui::BeginChild(childId, ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar);

    auto* pluginInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());

    // ----------------------------------------------------
    // Row 1: Header (Plugin Name + [|| Split] + [X] Delete)
    // ----------------------------------------------------
    {
        std::string displayName = slot->name();
        if (displayName.length() > 14) {
            displayName = displayName.substr(0, 13) + "..";
        }

        ImGui::TextColored(bypassed ? ImVec4(0.55f, 0.58f, 0.65f, 1.0f) : ImVec4(0.98f, 0.98f, 1.0f, 1.0f),
                           "%s", displayName.c_str());

        if (branchIndex == -1) {
            // Split into Parallel button
            ImGui::SameLine(cardWidth - 68.0f);
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
    // Row 2: Interactive Hardware Screen / Mini Faceplate Preview
    // ----------------------------------------------------
    {
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
        const float previewW = cardWidth - 16.0f;
        const float previewH = 46.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        bool isWindowOpen = pluginInst ? plugins::PluginWindowManager::instance().isWindowOpen(pluginInst) : false;

        // Clickable interactive region over the entire preview faceplate
        char clickAreaId[32];
        std::snprintf(clickAreaId, sizeof(clickAreaId), "##PrevClick_%d_%d", slotIndex, branchIndex);
        ImGui::InvisibleButton(clickAreaId, ImVec2(previewW, previewH));
        bool isHovered = ImGui::IsItemHovered();
        bool isClicked = ImGui::IsItemClicked();

        if (isClicked) {
            if (pluginInst) {
                plugins::PluginWindowManager::instance().openPluginWindow(pluginInst);
            } else {
                m_dspTweakSlot = slot;
            }
        }

        if (isHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (pluginInst) {
                ImGui::SetTooltip("Click to open %s native GUI", slot->name().c_str());
            } else {
                ImGui::SetTooltip("Click to adjust DSP parameters");
            }
        }

        // Metallic bevel background
        ImU32 screenBg = IM_COL32(16, 18, 24, 255);
        ImU32 screenBorder = isHovered ? IM_COL32(245, 150, 40, 255) :
                             (isWindowOpen ? IM_COL32(40, 210, 80, 255) : IM_COL32(45, 50, 64, 255));
        dl->AddRectFilled(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), screenBg, 4.0f);
        dl->AddRect(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), screenBorder, 4.0f, 0, isHovered ? 1.5f : 1.0f);

        // Header inside preview: Format badge & Vendor
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
            if (vendorText.length() > 16) vendorText = vendorText.substr(0, 15) + "..";
        }

        dl->AddText(ImVec2(previewPos.x + 8.0f, previewPos.y + 5.0f), tagColor, formatTag.c_str());
        dl->AddText(ImVec2(previewPos.x + 55.0f, previewPos.y + 5.0f), IM_COL32(130, 138, 155, 255), vendorText.c_str());

        // Action prompt inside preview
        if (isWindowOpen) {
            // Glowing Green LED + GUI OPEN
            dl->AddCircleFilled(ImVec2(previewPos.x + 14.0f, previewPos.y + 30.0f), 4.0f, IM_COL32(40, 240, 80, 255));
            dl->AddCircle(ImVec2(previewPos.x + 14.0f, previewPos.y + 30.0f), 6.5f, IM_COL32(40, 240, 80, 100), 0, 1.5f);
            dl->AddText(ImVec2(previewPos.x + 26.0f, previewPos.y + 23.0f), IM_COL32(70, 245, 110, 255), "GUI OPEN (FOCUS)");
        } else if (pluginInst) {
            ImU32 promptColor = isHovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(180, 190, 210, 255);
            dl->AddText(ImVec2(previewPos.x + 8.0f, previewPos.y + 23.0f), promptColor, isHovered ? ">> OPEN GUI <<" : "[ EDIT GUI ]");
        } else {
            ImU32 promptColor = isHovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 190, 120, 255);
            dl->AddText(ImVec2(previewPos.x + 8.0f, previewPos.y + 23.0f), promptColor, isHovered ? ">> EDIT DSP <<" : "[ TWEAK PARAMS ]");
        }
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
        if (ImGui::Button(btnId, ImVec2(62, 22))) {
            slot->setBypassed(active);
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 8);

        // Dry / Wet Slider
        float mix = slot->dryWet() * 100.0f;
        char mixLabel[32];
        std::snprintf(mixLabel, sizeof(mixLabel), "##Mix_%d_%d", slotIndex, branchIndex);
        ImGui::SetNextItemWidth(92);
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

void RackView::renderParallelBlock(audio::ParallelSplitMergeBlock* block, int blockIndex) {
    if (!block) return;

    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.16f, 0.20f, 1.0f));
    char blockChildId[64];
    std::snprintf(blockChildId, sizeof(blockChildId), "ParBlock_%d", blockIndex);

    // Dynamic width based on slots in branches
    size_t maxBranchSlots = 0;
    for (size_t b = 0; b < block->numBranches(); ++b) {
        auto* br = block->getBranch(b);
        if (br && br->numSlots() > maxBranchSlots) maxBranchSlots = br->numSlots();
    }
    const float blockW = std::max(480.0f, 320.0f + static_cast<float>(maxBranchSlots) * 140.0f);
    const float blockH = 185.0f;

    ImGui::BeginChild(blockChildId, ImVec2(blockW, blockH), true, ImGuiWindowFlags_NoScrollbar);

    // Header: Title + [X Remove Split Block]
    ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1.0f), "// PARALLEL SPLIT (A / B)");
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

    ImGui::Separator();

    for (size_t b = 0; b < block->numBranches(); ++b) {
        auto* branch = block->getBranch(b);
        if (!branch) continue;

        ImGui::PushID(static_cast<int>(b));

        // Branch controls row
        ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, 1.0f), "[%s]", branch->name().c_str());
        ImGui::SameLine(0, 8);

        // Mute / Solo Buttons
        bool muted = branch->isMuted();
        char muteLabel[32];
        std::snprintf(muteLabel, sizeof(muteLabel), "M##%d_%zu", blockIndex, b);
        if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button(muteLabel, ImVec2(20, 20))) {
            branch->setMuted(!muted);
        }
        if (muted) ImGui::PopStyleColor();
        ImGui::SameLine(0, 4);

        bool solo = branch->isSolo();
        char soloLabel[32];
        std::snprintf(soloLabel, sizeof(soloLabel), "S##%d_%zu", blockIndex, b);
        if (solo) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.9f, 0.7f, 0.1f, 1.0f));
        if (ImGui::Button(soloLabel, ImVec2(20, 20))) {
            branch->setSolo(!solo);
        }
        if (solo) ImGui::PopStyleColor();
        ImGui::SameLine(0, 8);

        // Branch Pan & Gain Sliders
        float pan = branch->pan();
        char panLabel[32];
        std::snprintf(panLabel, sizeof(panLabel), "##Pan_%d_%zu", blockIndex, b);
        ImGui::SetNextItemWidth(65);
        if (ImGui::SliderFloat(panLabel, &pan, -1.0f, +1.0f, "P:%.2f")) {
            branch->setPan(pan);
        }
        ImGui::SameLine(0, 6);

        float gain = branch->gainDb();
        char gainLabel[32];
        std::snprintf(gainLabel, sizeof(gainLabel), "##Gain_%d_%zu", blockIndex, b);
        ImGui::SetNextItemWidth(65);
        if (ImGui::SliderFloat(gainLabel, &gain, -36.0f, +12.0f, "%.0fdB")) {
            branch->setGainDb(gain);
        }

        ImGui::SameLine(0, 10);

        // Slots inside this branch
        if (branch->numSlots() == 0) {
            ImGui::TextDisabled("(Dry pass-through)");
            ImGui::SameLine(0, 8);
        } else {
            for (size_t s = 0; s < branch->numSlots(); ++s) {
                auto* bSlot = branch->getSlot(s);
                if (!bSlot) continue;

                ImGui::PushID(static_cast<int>(s));
                auto* pInst = dynamic_cast<plugins::IPluginInstance*>(bSlot->innerNode());
                bool isBypassed = bSlot->isBypassed();
                bool isOpen = pInst ? plugins::PluginWindowManager::instance().isWindowOpen(pInst) : false;

                ImVec4 chipCol = isBypassed ? ImVec4(0.18f, 0.20f, 0.24f, 1.0f) : ImVec4(0.22f, 0.28f, 0.36f, 1.0f);
                ImGui::PushStyleColor(ImGuiCol_Button, chipCol);

                std::string btnText = bSlot->name();
                if (btnText.length() > 10) btnText = btnText.substr(0, 9) + "..";
                if (isOpen) btnText += " [GUI]";

                if (ImGui::Button(btnText.c_str())) {
                    if (pInst) {
                        plugins::PluginWindowManager::instance().openPluginWindow(pInst);
                    } else {
                        m_dspTweakSlot = bSlot;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s (Click to open GUI)", bSlot->name().c_str());
                }
                ImGui::PopStyleColor();

                // Delete button for this branch slot
                ImGui::SameLine(0, 2);
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.18f, 0.18f, 0.8f));
                char rmSlotId[32];
                std::snprintf(rmSlotId, sizeof(rmSlotId), "x##rm_%d_%zu_%zu", blockIndex, b, s);
                if (ImGui::Button(rmSlotId, ImVec2(16, 20))) {
                    if (pInst) plugins::PluginWindowManager::instance().closePluginWindow(pInst);
                    branch->removeSlot(s);
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    break;
                }
                ImGui::PopStyleColor();

                ImGui::SameLine(0, 6);
                ImGui::TextDisabled("->");
                ImGui::SameLine(0, 6);

                ImGui::PopID();
            }
        }

        // [+ Add] Plugin to this branch button
        char addBtnId[32];
        std::snprintf(addBtnId, sizeof(addBtnId), "+ Add##br_%d_%zu", blockIndex, b);
        if (ImGui::Button(addBtnId)) {
            m_insertTargetBlockIndex = blockIndex;
            m_insertTargetBranchIndex = static_cast<int>(b);
            m_showPluginBrowser = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Insert plugin into %s", branch->name().c_str());
        }

        ImGui::PopID();
        if (b + 1 < block->numBranches()) {
            ImGui::Separator();
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
    ImGui::SetNextWindowSize(ImVec2(680, 500), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Plugin Manager & Scanner", &m_showPluginBrowser)) {
        ImGui::TextColored(ImVec4(0.98f, 0.60f, 0.20f, 1.0f), "Plugin Search Paths");
        ImGui::Separator();

        // Search Paths List
        const auto& paths = m_scanner.searchPaths();
        ImGui::BeginChild("PathsChild", ImVec2(0, 105), true);
        for (size_t i = 0; i < paths.size(); ++i) {
            ImGui::TextDisabled("[%zu]", i + 1);
            ImGui::SameLine(0, 8);
            ImGui::Text("%s", paths[i].c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 70);
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
        ImGui::EndChild();

        // Add Custom Search Path Input
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 120);
        ImGui::InputTextWithHint("##NewPath", "e.g. D:\\AudioPlugins", m_newPathBuffer, sizeof(m_newPathBuffer));
        ImGui::SameLine(0, 8);
        if (ImGui::Button("[+ Add Path]")) {
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

        ImGui::SameLine(0, 8);
        if (ImGui::Button("Scan / Refresh")) {
            m_scanner.scanAll();
        }

        ImGui::Spacing();
        if (m_insertTargetBlockIndex >= 0 && m_insertTargetBranchIndex >= 0) {
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1.0f), "Inserting into Parallel Block %d, Branch %d",
                               m_insertTargetBlockIndex + 1, m_insertTargetBranchIndex + 1);
        } else {
            ImGui::TextColored(ImVec4(0.98f, 0.60f, 0.20f, 1.0f), "Discovered Plugins (%zu available)", m_scanner.numPlugins());
        }
        ImGui::Separator();

        // Plugins List
        ImGui::BeginChild("PluginsListChild", ImVec2(0, 0), true);
        const auto& plugins = m_scanner.scannedPlugins();
        for (size_t p = 0; p < plugins.size(); ++p) {
            const auto& desc = plugins[p];
            ImGui::PushID(static_cast<int>(p));

            ImVec4 badgeCol = (desc.type == plugins::PluginType::VST3) ? ImVec4(0.3f, 0.7f, 1.0f, 1.0f) :
                              (desc.type == plugins::PluginType::CLAP) ? ImVec4(0.9f, 0.5f, 0.9f, 1.0f) :
                                                                         ImVec4(0.98f, 0.60f, 0.20f, 1.0f);
            ImGui::TextColored(badgeCol, "[%s]", desc.typeString().c_str());
            ImGui::SameLine(0, 8);
            ImGui::Text("%s", desc.name.c_str());
            ImGui::SameLine(0, 8);
            ImGui::TextDisabled("(%s)", desc.vendor.c_str());

            ImGui::SameLine(ImGui::GetWindowWidth() - 110);
            if (ImGui::Button("+ Insert")) {
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
                        m_graph.addSerialNode(std::move(newSlot));
                    }
                }
            }

            ImGui::PopID();
            ImGui::Separator();
        }
        ImGui::EndChild();
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

