#include "practice_tools_modal.h"
#include "../design_tokens.h"
#include "../theme.h"
#include "../ui_helpers.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace praccy::ui {

PracticeToolsModal::PracticeToolsModal(tools::QuickLooper& looper, tools::AudioPlayer& player)
    : m_looper(looper)
    , m_player(player)
{
}

PracticeToolsModal::~PracticeToolsModal() = default;

void PracticeToolsModal::open() {
    m_isOpen = true;
}

void PracticeToolsModal::close() {
    m_isOpen = false;
}

void PracticeToolsModal::renderLooperCircularProgressRing() {
    const auto& tokens = themeTokens();
    const tools::LooperState state = m_looper.state();
    const float progress = std::clamp(m_looper.playheadNormalized(), 0.0f, 1.0f);
    const double loopSec = m_looper.loopLengthSeconds();

    const float widgetSize = 130.0f;
    const float radius = 50.0f;
    const float thickness = 6.0f;

    const ImVec2 screenPos = ImGui::GetCursorScreenPos();
    const ImVec2 center(screenPos.x + widgetSize * 0.5f, screenPos.y + widgetSize * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 1. Determine State Colors & Strings
    ImU32 arcColor = tokens.text.muted;
    const char* stateText = "READY";
    switch (state) {
        case tools::LooperState::Recording:
            arcColor = tokens.signal.recording;
            stateText = "REC";
            break;
        case tools::LooperState::Overdubbing:
            arcColor = tokens.signal.overdubbing;
            stateText = "DUB";
            break;
        case tools::LooperState::Playing:
            arcColor = tokens.signal.playing;
            stateText = "PLAY";
            break;
        case tools::LooperState::Stopped:
            arcColor = tokens.signal.stopped;
            stateText = "STOP";
            break;
        case tools::LooperState::Empty:
        default:
            arcColor = tokens.borders.subtle;
            stateText = "READY";
            break;
    }

    // 2. Background Track Ring
    dl->PathArcTo(center, radius, 0.0f, 2.0f * IM_PI, 64);
    dl->PathStroke(tokens.borders.subtle, 0, thickness);

    // 3. Active Progress Arc
    const float startAngle = -IM_PI * 0.5f; // 12 o'clock
    float endAngle = startAngle;

    if (state == tools::LooperState::Recording) {
        // While recording, show continuous sweep progress
        float recProgress = static_cast<float>(std::fmod(ImGui::GetTime() * 0.8, 1.0));
        endAngle = startAngle + (2.0f * IM_PI * recProgress);
    } else if (progress > 0.001f) {
        endAngle = startAngle + (2.0f * IM_PI * progress);
    }

    if (endAngle > startAngle) {
        dl->PathArcTo(center, radius, startAngle, endAngle, 64);
        dl->PathStroke(arcColor, 0, thickness);

        // Playhead Indicator Dot
        const float dotX = center.x + radius * std::cos(endAngle);
        const float dotY = center.y + radius * std::sin(endAngle);
        dl->AddCircleFilled(ImVec2(dotX, dotY), thickness * 0.75f, arcColor, 16);
        dl->AddCircle(ImVec2(dotX, dotY), thickness * 0.75f, tokens.text.primary, 16, 1.5f);
    }

    // 4. Center Typography
    const ImVec2 stateSize = ImGui::CalcTextSize(stateText);
    dl->AddText(ImVec2(center.x - stateSize.x * 0.5f, center.y - stateSize.y - 3.0f), arcColor, stateText);

    char timeBuf[32];
    if (state == tools::LooperState::Recording) {
        std::snprintf(timeBuf, sizeof(timeBuf), "REC");
    } else if (loopSec > 0.05) {
        std::snprintf(timeBuf, sizeof(timeBuf), "%.1fs", progress * loopSec);
    } else {
        std::snprintf(timeBuf, sizeof(timeBuf), "--.-s");
    }

    if (g_fontMono) ImGui::PushFont(g_fontMono);
    const ImVec2 timeSize = ImGui::CalcTextSize(timeBuf);
    dl->AddText(ImVec2(center.x - timeSize.x * 0.5f, center.y + 4.0f), tokens.text.secondary, timeBuf);
    if (g_fontMono) ImGui::PopFont();

    // 5. Reserve Layout Space
    ImGui::Dummy(ImVec2(widgetSize, widgetSize));
}

void PracticeToolsModal::renderLooperTab() {
    const auto& tokens = themeTokens();
    ImGui::Spacing();
    ImGui::TextColored(tokens.text.accent.vec4, "QUICK PHRASE LOOPER");
    ImGui::Separator();
    ImGui::Spacing();

    const tools::LooperState loopState = m_looper.state();

    ImGui::BeginGroup();
    renderLooperCircularProgressRing();
    ImGui::EndGroup();

    ImGui::SameLine(0, 24.0f);

    ImGui::BeginGroup();
    ImGui::Text("Phrase Looper Controls:");
    ImGui::Spacing();

    // Action button label based on state
    const char* actionLabel = "Record [Action]";
    if (loopState == tools::LooperState::Recording) {
        actionLabel = "Play [Lock Loop]";
    } else if (loopState == tools::LooperState::Playing) {
        actionLabel = "Overdub [Add Layer]";
    } else if (loopState == tools::LooperState::Overdubbing) {
        actionLabel = "Play [Lock Layer]";
    } else if (loopState == tools::LooperState::Stopped) {
        actionLabel = "Record [New Loop]";
    }

    if (CenteredButton(actionLabel, ImVec2(180, 32))) {
        m_looper.triggerAction();
    }

    ImGui::SameLine(0, 10);
    if (CenteredButton("Stop", ImVec2(80, 32))) {
        m_looper.stop();
    }

    ImGui::SameLine(0, 10);
    if (CenteredButton("Clear", ImVec2(80, 32))) {
        m_looper.clear();
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // Volume Slider
    float looperVol = m_looper.volume();
    ImGui::Text("Looper Output Level:");
    ImGui::SetNextItemWidth(360);
    if (ResettableSliderFloat("##LooperVol", &looperVol, 0.0f, 2.0f, 1.0f, "Volume: %.0f%%")) {
        m_looper.setVolume(looperVol);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Quick Looper Playback Volume (Double-click to reset 100%%)");
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Smart 1-Button Cycling: Empty -> Record -> Play -> Overdub -> Play");
    ImGui::TextDisabled("Tip: Drag and drop a .WAV loop file anywhere onto Praccy to load it!");
    ImGui::EndGroup();
}

void PracticeToolsModal::renderBackingTrackTab() {
    const auto& tokens = themeTokens();
    ImGui::Spacing();
    ImGui::TextColored(tokens.text.accent.vec4, "BACKING TRACK AUDIO PLAYER");
    ImGui::Separator();
    ImGui::Spacing();

    // File path loader
    ImGui::Text("Audio Track File (.wav):");
    ImGui::SetNextItemWidth(450);
    ImGui::InputTextWithHint("##AudioTrackPath", "Path to .wav file or drag-and-drop onto Praccy...", m_audioFilePathBuffer, sizeof(m_audioFilePathBuffer));
    ImGui::SameLine(0, 8);
    if (CenteredButton("Load", ImVec2(70, 24))) {
        if (m_audioFilePathBuffer[0] != '\0') {
            m_player.loadWavFile(m_audioFilePathBuffer);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (m_player.isLoaded()) {
        ImGui::TextColored(tokens.text.primary.vec4, "Loaded Track: %s", m_player.fileName().c_str());
        double dur = m_player.durationSeconds();
        double pos = m_player.positionSeconds();

        char timeStr[64];
        std::snprintf(timeStr, sizeof(timeStr), "%02d:%02d / %02d:%02d",
            static_cast<int>(pos) / 60, static_cast<int>(pos) % 60,
            static_cast<int>(dur) / 60, static_cast<int>(dur) % 60);

        if (g_fontMono) ImGui::PushFont(g_fontMono);
        ImGui::TextColored(tokens.text.accent.vec4, "%s", timeStr);
        if (g_fontMono) ImGui::PopFont();

        ImGui::Spacing();

        // Transport Controls
        bool isPlaying = m_player.isPlaying();
        if (isPlaying) {
            if (CenteredButton("Pause##Player", ImVec2(90, 28))) {
                m_player.pause();
            }
        } else {
            if (CenteredButton("Play##Player", ImVec2(90, 28))) {
                m_player.play();
            }
        }

        ImGui::SameLine(0, 10);
        if (CenteredButton("Stop##Player", ImVec2(80, 28))) {
            m_player.stop();
        }

        ImGui::SameLine(0, 10);
        bool looping = m_player.isLooping();
        if (ImGui::Checkbox("Loop Track", &looping)) {
            m_player.setLooping(looping);
        }

        ImGui::Spacing();

        // Seek Bar
        float normPos = m_player.positionNormalized();
        ImGui::SetNextItemWidth(520);
        if (ImGui::SliderFloat("##PlayerSeek", &normPos, 0.0f, 1.0f, "")) {
            m_player.seek(normPos);
        }

        ImGui::Spacing();

        // Player Volume
        float playerVol = m_player.volume();
        ImGui::Text("Track Playback Level:");
        ImGui::SetNextItemWidth(300);
        if (ResettableSliderFloat("##PlayerVol", &playerVol, 0.0f, 2.0f, 1.0f, "Volume: %.0f%%")) {
            m_player.setVolume(playerVol);
        }
    } else {
        ImGui::Spacing();
        ImGui::TextDisabled("(No audio track loaded. Enter a WAV file path above and click Load, or drag-and-drop a WAV file)");
    }
}

void PracticeToolsModal::render() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Practice Tools##Modal");
    ImGui::SetNextWindowSize(ImVec2(680, 480), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Practice Tools##Modal", &m_isOpen, ImGuiWindowFlags_NoResize)) {
        const auto& tokens = themeTokens();
        ImGui::TextColored(tokens.text.accent.vec4, "PRACTICE SUITE");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("PracticeToolsTabs")) {
            if (ImGui::BeginTabItem("Quick Looper")) {
                renderLooperTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Backing Track Player")) {
                renderBackingTrackTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        float bottomY = std::max(ImGui::GetCursorPosY() + 12.0f, 435.0f);
        ImGui::SetCursorPosY(bottomY);
        ImGui::Separator();
        if (CenteredButton("Close", ImVec2(90, 26))) {
            close();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace praccy::ui
