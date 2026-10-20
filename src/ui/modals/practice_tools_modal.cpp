#include "practice_tools_modal.h"
#include "../../utils/wav_reader.h"
#include "../design_tokens.h"
#include "../theme.h"
#include "../ui_helpers.h"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <cstring>
#include <optional>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#endif

namespace praccy::ui {

namespace {
#if defined(_WIN32)
std::optional<std::string> openNativeWavFilePicker() {
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* pDialog = nullptr;
    std::optional<std::string> result;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pDialog)))) {
        COMDLG_FILTERSPEC rgSpec[] = {
            { L"WAV Audio Files (*.wav)", L"*.wav" },
            { L"All Files (*.*)", L"*.*" }
        };
        pDialog->SetFileTypes(ARRAYSIZE(rgSpec), rgSpec);
        pDialog->SetTitle(L"Select Backing Track Audio File (.wav)");
        HWND parent = GetActiveWindow();
        if (SUCCEEDED(pDialog->Show(parent))) {
            IShellItem* pItem = nullptr;
            if (SUCCEEDED(pDialog->GetResult(&pItem)) && pItem) {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0) {
                        std::string pathStr(len - 1, '\0');
                        WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, pathStr.data(), len, nullptr, nullptr);
                        result = pathStr;
                    }
                    CoTaskMemFree(pszPath);
                }
                pItem->Release();
            }
        }
        pDialog->Release();
    }
    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    return result;
}
#endif
} // namespace

PracticeToolsModal::PracticeToolsModal(tools::QuickLooper& looper, tools::AudioPlayer& player)
    : m_looper(looper)
    , m_player(player)
{
}

PracticeToolsModal::~PracticeToolsModal() = default;

void PracticeToolsModal::open() {
    m_isOpen = true;
    m_loopStart = m_player.loopStart();
    m_loopEnd = m_player.loopEnd();
}

void PracticeToolsModal::close() {
    m_isOpen = false;
}

void PracticeToolsModal::renderLooperCircularProgressRing() {
    const auto& tokens = themeTokens();
    const tools::LooperState state = m_looper.state();
    const float progress = std::clamp(m_looper.playheadNormalized(), 0.0f, 1.0f);
    const double loopSec = m_looper.loopLengthSeconds();

    const float widgetSize = ui::px(140.0f);
    const float radius = ui::px(54.0f);
    const float thickness = ui::px(6.0f);

    const ImVec2 screenPos = ImGui::GetCursorScreenPos();
    const ImVec2 center(screenPos.x + widgetSize * 0.5f, screenPos.y + widgetSize * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 1. Determine State Colors & Strings (keep state label >= 4.5:1 WCAG contrast in READY/STOP states)
    ImU32 arcColor = tokens.borders.subtle.u32;
    ImU32 stateLabelColor = tokens.text.looperIdle.u32;
    const char* stateText = "READY";
    switch (state) {
        case tools::LooperState::Recording:
            arcColor = stateLabelColor = tokens.signal.recording.u32;
            stateText = "REC";
            break;
        case tools::LooperState::Overdubbing:
            arcColor = stateLabelColor = tokens.signal.overdubbing.u32;
            stateText = "DUB";
            break;
        case tools::LooperState::Playing:
            arcColor = stateLabelColor = tokens.signal.playing.u32;
            stateText = "PLAY";
            break;
        case tools::LooperState::Stopped:
            arcColor = tokens.signal.stopped.u32;
            stateLabelColor = tokens.text.secondary.u32;
            stateText = "STOP";
            break;
        case tools::LooperState::Empty:
        default:
            arcColor = tokens.borders.subtle.u32;
            stateLabelColor = tokens.text.looperIdle.u32;
            stateText = "READY";
            break;
    }

    // 2. Hardware Stompbox Outer Bezel & Illuminated Halo Ring
    dl->AddCircle(center, radius + 7.0f, tokens.borders.subtle.u32, 64, 1.0f);

    const float curTime = static_cast<float>(ImGui::GetTime());
    if (state == tools::LooperState::Recording) {
        // Pulsing red halo
        float pulse = 0.55f + 0.45f * std::sin(curTime * 8.0f);
        ColorToken haloInner = tokens.signal.recording.withAlpha(pulse * 0.50f);
        ColorToken haloOuter = tokens.signal.recording.withAlpha(pulse * 0.22f);
        dl->AddCircle(center, radius + 2.0f, haloInner.u32, 64, 4.0f);
        dl->AddCircle(center, radius + 5.0f, haloOuter.u32, 64, 6.0f);
    } else if (state == tools::LooperState::Playing) {
        // Solid green halo
        ColorToken haloInner = tokens.signal.playing.withAlpha(0.40f);
        ColorToken haloOuter = tokens.signal.playing.withAlpha(0.18f);
        dl->AddCircle(center, radius + 2.0f, haloInner.u32, 64, 4.0f);
        dl->AddCircle(center, radius + 5.0f, haloOuter.u32, 64, 6.0f);
    } else if (state == tools::LooperState::Overdubbing) {
        // Warm amber halo
        ColorToken haloInner = tokens.signal.overdubbing.withAlpha(0.45f);
        ColorToken haloOuter = tokens.signal.overdubbing.withAlpha(0.20f);
        dl->AddCircle(center, radius + 2.0f, haloInner.u32, 64, 4.0f);
        dl->AddCircle(center, radius + 5.0f, haloOuter.u32, 64, 6.0f);
    }

    // 3. Background Track Ring
    dl->PathArcTo(center, radius, 0.0f, 2.0f * IM_PI, 64);
    dl->PathStroke(tokens.borders.subtle.u32, 0, thickness);

    // 4. Active Progress Arc & Playhead
    const float startAngle = -IM_PI * 0.5f; // 12 o'clock
    float endAngle = startAngle;

    if (state == tools::LooperState::Recording) {
        float recProgress = static_cast<float>(std::fmod(curTime * 0.8, 1.0));
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
        dl->AddCircleFilled(ImVec2(dotX, dotY), thickness * 0.8f, arcColor, 16);
        dl->AddCircle(ImVec2(dotX, dotY), thickness * 0.8f, tokens.text.primary.u32, 16, 1.5f);
    }

    // 5. Central Footswitch Disc (Body)
    const float hubR = radius - 13.0f;
    dl->AddCircleFilled(center, hubR, tokens.surfaces.cardBg.u32, 48);
    dl->AddCircle(center, hubR, tokens.borders.subtle.u32, 48, 1.5f);

    // 6. Center Typography: State, Bar/Beat, Duration
    const ImVec2 stateSize = ImGui::CalcTextSize(stateText);
    dl->AddText(ImVec2(center.x - stateSize.x * 0.5f, center.y - 22.0f), stateLabelColor, stateText);

    // Bar & Beat Counter (avoid redundant "REC / REC... / REC" triple-stack while recording)
    char barBeatBuf[32];
    if (state == tools::LooperState::Recording) {
        std::snprintf(barBeatBuf, sizeof(barBeatBuf), "CAPTURING");
    } else if (loopSec > 0.05) {
        std::snprintf(barBeatBuf, sizeof(barBeatBuf), "BAR %d . %d", m_looper.currentBar(), m_looper.currentBeat());
    } else {
        std::snprintf(barBeatBuf, sizeof(barBeatBuf), "BAR -- . -");
    }

    if (g_fontMono) ImGui::PushFont(g_fontMono);
    const ImVec2 barBeatSize = ImGui::CalcTextSize(barBeatBuf);
    dl->AddText(ImVec2(center.x - barBeatSize.x * 0.5f, center.y - 4.0f), tokens.text.primary.u32, barBeatBuf);

    char timeBuf[32];
    if (state == tools::LooperState::Recording) {
        std::snprintf(timeBuf, sizeof(timeBuf), "LIVE IN");
    } else if (loopSec > 0.05) {
        std::snprintf(timeBuf, sizeof(timeBuf), "%.1fs / %.1fs", progress * loopSec, loopSec);
    } else {
        std::snprintf(timeBuf, sizeof(timeBuf), "--.-s");
    }

    const ImVec2 timeSize = ImGui::CalcTextSize(timeBuf);
    dl->AddText(ImVec2(center.x - timeSize.x * 0.5f, center.y + 11.0f), tokens.text.secondary.u32, timeBuf);
    if (g_fontMono) ImGui::PopFont();

    // 7. Reserve Layout Space
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

    // Action button label based on state (clean labels without raw '[Action]' placeholder)
    const char* actionLabel = "Record Loop";
    if (loopState == tools::LooperState::Recording) {
        actionLabel = "Lock & Play";
    } else if (loopState == tools::LooperState::Playing) {
        actionLabel = "Overdub Layer";
    } else if (loopState == tools::LooperState::Overdubbing) {
        actionLabel = "Finish Overdub";
    } else if (loopState == tools::LooperState::Stopped) {
        actionLabel = "Record New Loop";
    }

    if (CenteredButton(actionLabel, ImVec2(180, Metrics::buttonHeight()))) {
        m_looper.triggerAction();
    }

    ImGui::SameLine(0, 10);
    if (CenteredButton("Stop", ImVec2(80, Metrics::buttonHeight()))) {
        m_looper.stop();
    }

    ImGui::SameLine(0, 10);
    if (CenteredButton("Clear", ImVec2(80, Metrics::buttonHeight()))) {
        m_looper.clear();
    }

    ImGui::Spacing();

    // Undo / Redo Last Layer Controls
    const bool canUndo = m_looper.canUndo();
    const bool canRedo = m_looper.canRedo();

    if (!canUndo) ImGui::BeginDisabled();
    if (CenteredButton("Undo Layer", ImVec2(96, Metrics::buttonHeight()))) {
        m_looper.undo();
    }
    if (!canUndo) ImGui::EndDisabled();

    ImGui::SameLine(0, 8);
    if (!canRedo) ImGui::BeginDisabled();
    if (CenteredButton("Redo Layer", ImVec2(96, Metrics::buttonHeight()))) {
        m_looper.redo();
    }
    if (!canRedo) ImGui::EndDisabled();

    ImGui::SameLine(0, 14);
    if (g_fontMono) ImGui::PushFont(g_fontMono);
    ImGui::AlignTextToFramePadding();
    const int totBars = m_looper.totalBars();
    const int totBeats = m_looper.totalBeats();
    ImGui::TextColored(tokens.text.muted.vec4, "%d %s (%d %s) @ %.0f BPM",
                       totBars, (totBars == 1 ? "Bar" : "Bars"),
                       totBeats, (totBeats == 1 ? "Beat" : "Beats"),
                       m_looper.bpm());
    if (g_fontMono) ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Spacing();

    // Volume Slider (0% to 200%, default unity 100%)
    float looperVolPct = m_looper.volume() * 100.0f;
    ImGui::Text("Looper Output Level:");
    ImGui::SetNextItemWidth(360);
    if (ResettableSliderFloat("##LooperVol", &looperVolPct, 0.0f, 200.0f, 100.0f, "Volume: %.0f%%")) {
        m_looper.setVolume(looperVolPct / 100.0f);
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

    // File path loader with native Browse... dialog
    ImGui::Text("Audio Track File (.wav):");
    const float inputW = std::max(180.0f, ImGui::GetContentRegionAvail().x - 180.0f);
    ImGui::SetNextItemWidth(inputW);
    ImGui::InputTextWithHint("##AudioTrackPath", "Path to .wav file or drag-and-drop onto Praccy...", m_audioFilePathBuffer, sizeof(m_audioFilePathBuffer));
    ImGui::SameLine(0, 8);
    if (CenteredButton("Browse...", ImVec2(88, Metrics::buttonHeight()))) {
#if defined(_WIN32)
        if (auto picked = openNativeWavFilePicker()) {
            std::snprintf(m_audioFilePathBuffer, sizeof(m_audioFilePathBuffer), "%s", picked->c_str());
            m_player.loadWavFile(*picked);
        }
#endif
    }
    ImGui::SameLine(0, 8);
    if (CenteredButton("Load", ImVec2(70, Metrics::buttonHeight()))) {
        if (m_audioFilePathBuffer[0] != '\0') {
            m_player.loadWavFile(m_audioFilePathBuffer);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (m_player.isLoading()) {
        float prog = m_player.loadProgress();
        ImVec2 spinnerPos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float spinR = 12.0f;
        ImVec2 spinCenter(spinnerPos.x + spinR + 4.0f, spinnerPos.y + spinR + 4.0f);
        float spinAngle = static_cast<float>(ImGui::GetTime() * 7.0);
        dl->PathArcTo(spinCenter, spinR, spinAngle, spinAngle + 1.4f * IM_PI, 24);
        dl->PathStroke(tokens.text.accent.u32, 0, 3.0f);

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 40.0f);
        ImGui::TextColored(tokens.text.accent.vec4, "Loading Backing Track in background... (%.0f%%)", prog * 100.0f);
        ImGui::Spacing();
        ImGui::ProgressBar(prog, ImVec2(-1.0f, 6.0f), "");
    } else if (m_player.isLoaded()) {
        ImGui::TextColored(tokens.text.primary.vec4, "Loaded Track: %s", m_player.fileName().c_str());
        double dur = m_player.durationSeconds();
        double pos = m_player.positionSeconds();
        double rem = std::max(0.0, dur - pos);

        char timeStr[96];
        std::snprintf(timeStr, sizeof(timeStr), "Elapsed: %02d:%02d   |   Remaining: -%02d:%02d   |   Total: %02d:%02d",
            static_cast<int>(pos) / 60, static_cast<int>(pos) % 60,
            static_cast<int>(rem) / 60, static_cast<int>(rem) % 60,
            static_cast<int>(dur) / 60, static_cast<int>(dur) % 60);

        if (g_fontMono) ImGui::PushFont(g_fontMono);
        ImGui::TextColored(tokens.text.accent.vec4, "%s", timeStr);
        if (g_fontMono) ImGui::PopFont();

        ImGui::Spacing();

        // Transport Controls
        bool isPlaying = m_player.isPlaying();
        if (isPlaying) {
            if (CenteredButton("Pause##Player", ImVec2(90, Metrics::buttonHeight()))) {
                m_player.pause();
            }
        } else {
            if (CenteredButton("Play##Player", ImVec2(90, Metrics::buttonHeight()))) {
                m_player.play();
            }
        }

        ImGui::SameLine(0, 10);
        if (CenteredButton("Stop##Player", ImVec2(80, Metrics::buttonHeight()))) {
            m_player.stop();
        }

        ImGui::SameLine(0, 10);
        bool looping = m_player.isLooping();
        if (ImGui::Checkbox("Loop Track", &looping)) {
            m_player.setLooping(looping);
        }

        ImGui::Spacing();
        ImGui::Spacing();

        if (m_lastLoadedTrack != m_player.filePath() || (m_waveform800.empty() && !m_player.filePath().empty())) {
            m_lastLoadedTrack = m_player.filePath();
            const auto& peaks = m_player.waveformPeaks800();
            if (!peaks.empty()) {
                m_waveform800.resize(peaks.size());
                for (size_t i = 0; i < peaks.size(); ++i) {
                    m_waveform800[i] = WaveformPeak{peaks[i].first, peaks[i].second};
                }
            }
        }

        ImGui::TextColored(tokens.text.secondary.vec4, "Interactive Overview Waveform (800-Point Strip):");
        const float stripW = std::max(200.0f, ImGui::GetContentRegionAvail().x);
        renderOverviewWaveformStrip(stripW, 64.0f);

        ImGui::Spacing();
        double startSec = m_loopStart * dur;
        double endSec = m_loopEnd * dur;
        if (g_fontMono) ImGui::PushFont(g_fontMono);
        ImGui::TextColored(tokens.text.primary.vec4, "A/B Loop Range: [%02d:%02d (%.1f%%)] -> [%02d:%02d (%.1f%%)]",
                           static_cast<int>(startSec) / 60, static_cast<int>(startSec) % 60, m_loopStart * 100.0f,
                           static_cast<int>(endSec) / 60, static_cast<int>(endSec) % 60, m_loopEnd * 100.0f);
        if (g_fontMono) ImGui::PopFont();

        ImGui::SameLine(0, 16);
        if (CenteredButton("Reset Loop Points", ImVec2(130, Metrics::buttonHeight()))) {
            m_loopStart = 0.0f;
            m_loopEnd = 1.0f;
            m_player.setLoopPoints(0.0f, 1.0f);
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

    praccy::utils::WavAudioData wavData;
    if (!praccy::utils::WavReader::loadWavFile(wavPath, wavData) || wavData.numFrames == 0) {
        for (int i = 0; i < 800; ++i) {
            float t = static_cast<float>(i) / 800.0f;
            float amp = 0.35f + 0.25f * std::sin(t * 18.0f) + 0.15f * std::cos(t * 42.0f);
            amp = std::clamp(amp, 0.05f, 0.95f);
            m_waveform800[i] = WaveformPeak{-amp, amp};
        }
        return;
    }

    const size_t totalFrames = wavData.numFrames;
    const size_t bucketSize = std::max<size_t>(1, totalFrames / 800);

    for (size_t b = 0; b < 800; ++b) {
        size_t startFrame = b * bucketSize;
        size_t endFrame = std::min(totalFrames, (b == 799) ? totalFrames : (startFrame + bucketSize));
        float minVal = 0.0f;
        float maxVal = 0.0f;

        size_t step = std::max<size_t>(1, (endFrame - startFrame) / 128);
        for (size_t f = startFrame; f < endFrame; f += step) {
            float s = (wavData.samplesL[f] + wavData.samplesR[f]) * 0.5f;
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
        ImU32 loopShadeCol = static_cast<ImU32>(tokens.text.accent.withAlpha(0.15f));
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

    // Draggable Loop Start Bracket [A
    dl->AddLine(ImVec2(loopStartX, screenPos.y), ImVec2(loopStartX, stripMax.y), tokens.text.accent.u32, 2.0f);
    dl->AddRectFilled(ImVec2(loopStartX - 8.0f, screenPos.y), ImVec2(loopStartX + 1.0f, screenPos.y + 15.0f),
                      tokens.text.accent.u32, 2.0f);
    dl->AddText(ImVec2(loopStartX - 7.0f, screenPos.y), tokens.surfaces.windowBg.u32, "A");

    // Draggable Loop End Bracket B]
    dl->AddLine(ImVec2(loopEndX, screenPos.y), ImVec2(loopEndX, stripMax.y), tokens.text.accent.u32, 2.0f);
    dl->AddRectFilled(ImVec2(loopEndX - 1.0f, screenPos.y), ImVec2(loopEndX + 8.0f, screenPos.y + 15.0f),
                      tokens.text.accent.u32, 2.0f);
    dl->AddText(ImVec2(loopEndX + 2.0f, screenPos.y), tokens.surfaces.windowBg.u32, "B");

    // Playhead line & needle
    dl->AddLine(ImVec2(playheadX, screenPos.y), ImVec2(playheadX, stripMax.y), tokens.text.primary.u32, 2.0f);
    dl->AddTriangleFilled(ImVec2(playheadX - 4.0f, screenPos.y), ImVec2(playheadX + 4.0f, screenPos.y),
                          ImVec2(playheadX, screenPos.y + 7.0f), tokens.text.primary.u32);

    // InvisibleButton to handle mouse click-to-seek and drag
    char btnId[32];
    std::snprintf(btnId, sizeof(btnId), "##WaveformSeek_%p", static_cast<const void*>(this));
    ImGui::InvisibleButton(btnId, ImVec2(stripWidth, stripHeight));
    bool isHovered = ImGui::IsItemHovered();
    bool isActive = ImGui::IsItemActive();

    if (isActive) {
        float mouseRelX = io.MousePos.x - screenPos.x;
        float normX = std::clamp(mouseRelX / stripWidth, 0.0f, 1.0f);

        if (m_isDraggingLoopStart) {
            m_loopStart = std::clamp(normX, 0.0f, m_loopEnd - 0.02f);
            m_player.setLoopPoints(m_loopStart, m_loopEnd);
        } else if (m_isDraggingLoopEnd) {
            m_loopEnd = std::clamp(normX, m_loopStart + 0.02f, 1.0f);
            m_player.setLoopPoints(m_loopStart, m_loopEnd);
        } else {
            if (std::abs(io.MouseClickedPos[0].x - loopStartX) < 12.0f) {
                m_isDraggingLoopStart = true;
                m_loopStart = std::clamp(normX, 0.0f, m_loopEnd - 0.02f);
                m_player.setLoopPoints(m_loopStart, m_loopEnd);
            } else if (std::abs(io.MouseClickedPos[0].x - loopEndX) < 12.0f) {
                m_isDraggingLoopEnd = true;
                m_loopEnd = std::clamp(normX, m_loopStart + 0.02f, 1.0f);
                m_player.setLoopPoints(m_loopStart, m_loopEnd);
            } else {
                m_player.seek(normX);
            }
        }
    } else {
        m_isDraggingLoopStart = false;
        m_isDraggingLoopEnd = false;
    }

    if (isHovered) {
        if (std::abs(io.MousePos.x - loopStartX) < 12.0f || std::abs(io.MousePos.x - loopEndX) < 12.0f) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }
}

void PracticeToolsModal::render() {
    if (!m_isOpen) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float initW = vp ? std::clamp(vp->Size.x * 0.70f, 740.0f, 1050.0f) : 760.0f;
    const float initH = vp ? std::clamp(vp->Size.y * 0.72f, 540.0f, 760.0f) : 560.0f;

    ImGui::OpenPopup("Practice Tools##Modal");
    ImGui::SetNextWindowSize(ImVec2(initW, initH), ImGuiCond_Appearing);
    if (vp) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(640.0f, 460.0f), ImVec2(vp->Size.x * 0.95f, vp->Size.y * 0.95f));
    }
    ImGui::SetNextWindowPos(vp ? vp->GetCenter() : ImVec2(100.0f, 100.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    const auto& tokens = themeTokens();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tokens.surfaces.popupBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.subtle.vec4);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Spec::kModalCornerRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Spec::kModalWindowPadding, Spec::kModalWindowPadding));

    if (ImGui::BeginPopupModal("Practice Tools##Modal", &m_isOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const float footerHeight = Metrics::kFooterHeight;
        const float contentHeight = ImGui::GetContentRegionAvail().y - footerHeight;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ColorToken::transparent().vec4);
        if (ImGui::BeginChild("PracticeToolsContentRegion", ImVec2(0, contentHeight), false)) {
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
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Separator();
        ImGui::Spacing();
        if (CenteredButton("Close", ImVec2(90, Metrics::buttonHeight())) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            close();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace praccy::ui
