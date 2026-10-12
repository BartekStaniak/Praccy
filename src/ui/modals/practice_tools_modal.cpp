#include "practice_tools_modal.h"
#include "../design_tokens.h"
#include "../theme.h"
#include "../ui_helpers.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <cstring>

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

        ImGui::Spacing();

        if (m_lastLoadedTrack != m_player.filePath()) {
            m_lastLoadedTrack = m_player.filePath();
            generateWaveform800(m_lastLoadedTrack);
        }

        ImGui::TextColored(tokens.text.secondary.vec4, "Interactive Overview Waveform (800-Point Strip):");
        renderOverviewWaveformStrip(520.0f, 54.0f);

        ImGui::Spacing();
        ImGui::Text("Loop Region: %.1f%% - %.1f%%", m_loopStart * 100.0f, m_loopEnd * 100.0f);
        ImGui::SameLine(0, 16);
        if (CenteredButton("Reset Loop Points", ImVec2(120, 20))) {
            m_loopStart = 0.0f;
            m_loopEnd = 1.0f;
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

void PracticeToolsModal::generateWaveform800(const std::string& wavPath) {
    m_waveform800.clear();
    m_waveform800.resize(800, WaveformPeak{-0.05f, 0.05f});

    if (wavPath.empty()) return;

    std::ifstream file(wavPath, std::ios::binary);
    if (!file.is_open()) {
        for (int i = 0; i < 800; ++i) {
            float t = static_cast<float>(i) / 800.0f;
            float amp = 0.35f + 0.25f * std::sin(t * 18.0f) + 0.15f * std::cos(t * 42.0f);
            amp = std::clamp(amp, 0.05f, 0.95f);
            m_waveform800[i] = WaveformPeak{-amp, amp};
        }
        return;
    }

    char riffTag[4]{};
    uint32_t fileSize = 0;
    char waveTag[4]{};
    file.read(riffTag, 4);
    file.read(reinterpret_cast<char*>(&fileSize), 4);
    file.read(waveTag, 4);

    if (std::memcmp(riffTag, "RIFF", 4) != 0 || std::memcmp(waveTag, "WAVE", 4) != 0) {
        for (int i = 0; i < 800; ++i) {
            float t = static_cast<float>(i) / 800.0f;
            float amp = 0.3f + 0.2f * std::sin(t * 20.0f);
            m_waveform800[i] = WaveformPeak{-amp, amp};
        }
        return;
    }

    uint16_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint16_t audioFormat = 1;
    bool foundFmt = false;
    std::vector<uint8_t> rawData;

    while (file) {
        char chunkTag[4]{};
        uint32_t chunkSize = 0;
        file.read(chunkTag, 4);
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (file.gcount() < 8) break;

        if (std::memcmp(chunkTag, "fmt ", 4) == 0) {
            file.read(reinterpret_cast<char*>(&audioFormat), 2);
            file.read(reinterpret_cast<char*>(&numChannels), 2);
            file.read(reinterpret_cast<char*>(&sampleRate), 4);
            uint32_t byteRate = 0;
            uint16_t blockAlign = 0;
            file.read(reinterpret_cast<char*>(&byteRate), 4);
            file.read(reinterpret_cast<char*>(&blockAlign), 2);
            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            if (chunkSize > 16) {
                file.seekg(chunkSize - 16, std::ios::cur);
            }
            foundFmt = true;
        } else if (std::memcmp(chunkTag, "data", 4) == 0) {
            rawData.resize(chunkSize);
            file.read(reinterpret_cast<char*>(rawData.data()), chunkSize);
            break;
        } else {
            file.seekg(chunkSize, std::ios::cur);
        }
    }

    if (!foundFmt || rawData.empty() || numChannels == 0 || bitsPerSample == 0) {
        for (int i = 0; i < 800; ++i) {
            float t = static_cast<float>(i) / 800.0f;
            float amp = 0.3f + 0.2f * std::sin(t * 20.0f);
            m_waveform800[i] = WaveformPeak{-amp, amp};
        }
        return;
    }

    const size_t bytesPerSample = bitsPerSample / 8;
    const size_t frameSize = numChannels * bytesPerSample;
    const size_t totalFrames = rawData.size() / frameSize;
    if (totalFrames == 0) return;

    const size_t bucketSize = std::max<size_t>(1, totalFrames / 800);
    const uint8_t* ptr = rawData.data();

    for (size_t b = 0; b < 800; ++b) {
        size_t startFrame = b * bucketSize;
        size_t endFrame = std::min(totalFrames, (b == 799) ? totalFrames : (startFrame + bucketSize));
        float minVal = 0.0f;
        float maxVal = 0.0f;

        size_t step = std::max<size_t>(1, (endFrame - startFrame) / 128);
        for (size_t f = startFrame; f < endFrame; f += step) {
            const uint8_t* framePtr = ptr + f * frameSize;
            float s = 0.0f;
            if (audioFormat == 1) { // PCM
                if (bitsPerSample == 16) {
                    s = reinterpret_cast<const int16_t*>(framePtr)[0] / 32768.0f;
                } else if (bitsPerSample == 24) {
                    int32_t val = (framePtr[0] << 8) | (framePtr[1] << 16) | (framePtr[2] << 24);
                    s = (val >> 8) / 8388608.0f;
                } else if (bitsPerSample == 32) {
                    s = reinterpret_cast<const int32_t*>(framePtr)[0] / 2147483648.0f;
                }
            } else if (audioFormat == 3 && bitsPerSample == 32) { // IEEE float
                s = reinterpret_cast<const float*>(framePtr)[0];
            }
            if (std::isnan(s)) s = 0.0f;
            if (s < minVal) minVal = s;
            if (s > maxVal) maxVal = s;
        }

        if (minVal == 0.0f && maxVal == 0.0f) {
            minVal = -0.02f;
            maxVal = 0.02f;
        }
        m_waveform800[b] = WaveformPeak{minVal, maxVal};
    }
}

void PracticeToolsModal::renderOverviewWaveformStrip(float stripWidth, float stripHeight) {
    const auto& tokens = themeTokens();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 screenPos = ImGui::GetCursorScreenPos();
    ImVec2 stripMax(screenPos.x + stripWidth, screenPos.y + stripHeight);

    // Frame background and border
    dl->AddRectFilled(screenPos, stripMax, tokens.surfaces.frameBg, 4.0f);
    dl->AddRect(screenPos, stripMax, tokens.borders.subtle, 4.0f);

    const float midY = screenPos.y + stripHeight * 0.5f;
    dl->AddLine(ImVec2(screenPos.x, midY), ImVec2(stripMax.x, midY), tokens.borders.subtle, 1.0f);

    const float curPlayhead = std::clamp(m_player.positionNormalized(), 0.0f, 1.0f);
    const float playheadX = screenPos.x + stripWidth * curPlayhead;

    const float loopStartX = screenPos.x + stripWidth * m_loopStart;
    const float loopEndX = screenPos.x + stripWidth * m_loopEnd;

    // Loop region shaded background
    if (m_loopEnd > m_loopStart) {
        ImU32 loopShadeCol = ColorToken(tokens.text.accent.r, tokens.text.accent.g, tokens.text.accent.b, 0.15f);
        dl->AddRectFilled(ImVec2(loopStartX, screenPos.y), ImVec2(loopEndX, stripMax.y), loopShadeCol, 2.0f);
    }

    // Draw 800 peak points mapped to stripWidth
    const size_t numPoints = m_waveform800.size();
    if (numPoints > 0) {
        for (int px = 0; px < static_cast<int>(stripWidth); ++px) {
            float frac = static_cast<float>(px) / stripWidth;
            size_t idx = std::min(numPoints - 1, static_cast<size_t>(frac * static_cast<float>(numPoints)));
            const auto& pt = m_waveform800[idx];

            float x = screenPos.x + static_cast<float>(px);
            float yTop = midY - std::clamp(pt.maxVal, 0.0f, 1.0f) * (stripHeight * 0.44f);
            float yBot = midY - std::clamp(pt.minVal, -1.0f, 0.0f) * (stripHeight * 0.44f);
            if (yBot < yTop + 1.0f) yBot = yTop + 1.0f;

            ImU32 barCol = (x <= playheadX) ? tokens.signal.active.u32 : tokens.text.secondary.u32;
            if (x >= loopStartX && x <= loopEndX) {
                barCol = (x <= playheadX) ? tokens.signal.active.u32 : tokens.text.accent.u32;
            }

            dl->AddLine(ImVec2(x, yTop), ImVec2(x, yBot), barCol, 1.0f);
        }
    }

    // Draggable Loop Start Handle [L
    dl->AddLine(ImVec2(loopStartX, screenPos.y), ImVec2(loopStartX, stripMax.y), tokens.text.accent.u32, 2.0f);
    dl->AddRectFilled(ImVec2(loopStartX - 7.0f, screenPos.y), ImVec2(loopStartX + 1.0f, screenPos.y + 14.0f),
                      tokens.text.accent.u32, 2.0f);
    dl->AddText(ImVec2(loopStartX - 6.0f, screenPos.y), tokens.surfaces.windowBg.u32, "L");

    // Draggable Loop End Handle R]
    dl->AddLine(ImVec2(loopEndX, screenPos.y), ImVec2(loopEndX, stripMax.y), tokens.text.accent.u32, 2.0f);
    dl->AddRectFilled(ImVec2(loopEndX - 1.0f, screenPos.y), ImVec2(loopEndX + 7.0f, screenPos.y + 14.0f),
                      tokens.text.accent.u32, 2.0f);
    dl->AddText(ImVec2(loopEndX + 1.0f, screenPos.y), tokens.surfaces.windowBg.u32, "R");

    // Playhead line & needle
    dl->AddLine(ImVec2(playheadX, screenPos.y), ImVec2(playheadX, stripMax.y), tokens.text.primary.u32, 2.0f);
    dl->AddTriangleFilled(ImVec2(playheadX - 4.0f, screenPos.y), ImVec2(playheadX + 4.0f, screenPos.y),
                          ImVec2(playheadX, screenPos.y + 7.0f), tokens.text.primary.u32);

    // InvisibleButton to handle mouse click-to-seek and drag
    char btnId[32];
    std::snprintf(btnId, sizeof(btnId), "##WaveformSeek_%p", this);
    ImGui::InvisibleButton(btnId, ImVec2(stripWidth, stripHeight));
    bool isHovered = ImGui::IsItemHovered();
    bool isActive = ImGui::IsItemActive();

    if (isActive) {
        float mouseRelX = io.MousePos.x - screenPos.x;
        float normX = std::clamp(mouseRelX / stripWidth, 0.0f, 1.0f);

        if (m_isDraggingLoopStart) {
            m_loopStart = std::clamp(normX, 0.0f, m_loopEnd - 0.02f);
        } else if (m_isDraggingLoopEnd) {
            m_loopEnd = std::clamp(normX, m_loopStart + 0.02f, 1.0f);
        } else {
            if (std::abs(io.MouseClickedPos[0].x - loopStartX) < 10.0f) {
                m_isDraggingLoopStart = true;
                m_loopStart = std::clamp(normX, 0.0f, m_loopEnd - 0.02f);
            } else if (std::abs(io.MouseClickedPos[0].x - loopEndX) < 10.0f) {
                m_isDraggingLoopEnd = true;
                m_loopEnd = std::clamp(normX, m_loopStart + 0.02f, 1.0f);
            } else {
                m_player.seek(normX);
            }
        }
    } else {
        m_isDraggingLoopStart = false;
        m_isDraggingLoopEnd = false;
    }

    if (isHovered) {
        if (std::abs(io.MousePos.x - loopStartX) < 10.0f || std::abs(io.MousePos.x - loopEndX) < 10.0f) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    // Auto-looping within custom loop boundaries
    if (m_player.isPlaying() && m_player.isLooping()) {
        if (curPlayhead >= m_loopEnd || curPlayhead < m_loopStart) {
            m_player.seek(m_loopStart);
        }
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
