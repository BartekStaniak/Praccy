#include "rack_view.h"
#include "thumbnail_manager.h"
#include "update_checker.h"
#include "design_tokens.h"
#include "theme.h"
#include "ui_helpers.h"
#include "../plugins/builtin_dsp.h"
#include "../plugins/clap_host.h"
#include "../plugins/vst3_host.h"
#include "../plugins/plugin_window.h"
#include "../state/app_config.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace praccy::ui {

static inline ImVec2 evaluateCubicBezier(ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, float u) {
    const float u1 = 1.0f - u;
    const float w0 = u1 * u1 * u1;
    const float w1 = 3.0f * u1 * u1 * u;
    const float w2 = 3.0f * u1 * u * u;
    const float w3 = u * u * u;
    return ImVec2(
        w0 * p0.x + w1 * c0.x + w2 * c1.x + w3 * p1.x,
        w0 * p0.y + w1 * c0.y + w2 * c1.y + w3 * p1.y
    );
}

static void drawCubicHermiteCable(ImDrawList* dl, ImVec2 p0, ImVec2 p1,
                                 float signalPeak = 0.0f, bool isBypassed = false,
                                 float animTime = 0.0f) {
    const float dx = p1.x - p0.x;
    const float dy = p1.y - p0.y;

    // Distance-adaptive tangent magnitude with short-dx safety clamp
    float tMag = std::max(36.0f, 0.55f * dx + 0.35f * std::abs(dy));
    if (dx < 36.0f && dx > 0.0f) {
        tMag = std::min(tMag, std::max(12.0f, dx * 1.2f));
    }

    // Hermite to Cubic Bezier control points
    const float inv3 = 1.0f / 3.0f;
    const ImVec2 c0(p0.x + tMag * inv3, p0.y);
    const ImVec2 c1(p1.x - tMag * inv3, p1.y);

    const auto& tokens = themeTokens();
    const ImU32 shadowCol = isBypassed ? static_cast<ImU32>(ColorToken(0, 0, 0, 15)) : static_cast<ImU32>(ColorToken(0, 0, 0, 48));
    const ImU32 sleeveCol = isBypassed ? static_cast<ImU32>(tokens.cables.bypassedSleeve) : static_cast<ImU32>(tokens.cables.sleeve);
    const ImU32 coreCol   = isBypassed ? static_cast<ImU32>(tokens.cables.bypassedCore)   : static_cast<ImU32>(tokens.cables.core);
    const ImU32 socketCol = static_cast<ImU32>(tokens.cables.socketRing);
    const ImU32 pinCol    = static_cast<ImU32>(tokens.cables.socketPin);

    // 1. Layer 1: Drop Shadow (+2.5px Y offset, alpha 48)
    const ImVec2 shOffset(0.0f, 2.5f);
    dl->AddBezierCubic(
        ImVec2(p0.x + shOffset.x, p0.y + shOffset.y),
        ImVec2(c0.x + shOffset.x, c0.y + shOffset.y),
        ImVec2(c1.x + shOffset.x, c1.y + shOffset.y),
        ImVec2(p1.x + shOffset.x, p1.y + shOffset.y),
        shadowCol, 5.5f, 24
    );

    // 2. Layer 2: Outer Sleeve (~3.5px gauge)
    dl->AddBezierCubic(p0, c0, c1, p1, sleeveCol, 3.5f, 24);

    // 3. Layer 3: Core Wire (~1.8px)
    dl->AddBezierCubic(p0, c0, c1, p1, coreCol, 1.8f, 24);

    // 4. Layer 4: Socket Endpoint Pins
    dl->AddCircleFilled(p0, 4.5f, socketCol);
    dl->AddCircleFilled(p0, 2.5f, pinCol);
    dl->AddCircleFilled(p1, 4.5f, socketCol);
    dl->AddCircleFilled(p1, 2.5f, pinCol);

    // 5. Layer 5: Audio-Reactive Signal Pulse Dots
    if (!isBypassed) {
        const float safePeak = (!std::isfinite(signalPeak) || signalPeak < 0.0f) ? 0.0f : std::clamp(signalPeak, 0.0f, 1.0f);
        const float safeTime = (!std::isfinite(animTime)) ? 0.0f : animTime;
        const float normPeak = safePeak;
        const float dotSpeed = 0.75f;
        float u0 = std::fmod(safeTime * dotSpeed, 1.0f);
        if (u0 < 0.0f) {
            u0 += 1.0f;
        }

        const float baseR = 1.8f + 2.4f * std::sqrt(normPeak);
        const float alphaFactor = 0.35f + 0.65f * normPeak;

        auto drawDot = [&](float uVal) {
            ImVec2 dotPos = evaluateCubicBezier(p0, c0, c1, p1, uVal);

            // Halo glow
            dl->AddCircleFilled(dotPos, baseR * 2.2f,
                static_cast<ImU32>(ColorToken(
                    static_cast<uint8_t>(tokens.cables.pulseGlow.r * 255.0f),
                    static_cast<uint8_t>(tokens.cables.pulseGlow.g * 255.0f),
                    static_cast<uint8_t>(tokens.cables.pulseGlow.b * 255.0f),
                    static_cast<uint8_t>(std::clamp(56.0f * alphaFactor, 0.0f, 255.0f))
                )));

            // Main dot body
            dl->AddCircleFilled(dotPos, baseR,
                static_cast<ImU32>(ColorToken(
                    static_cast<uint8_t>(tokens.cables.pulseDot.r * 255.0f),
                    static_cast<uint8_t>(tokens.cables.pulseDot.g * 255.0f),
                    static_cast<uint8_t>(tokens.cables.pulseDot.b * 255.0f),
                    static_cast<uint8_t>(std::clamp(230.0f * alphaFactor, 0.0f, 255.0f))
                )));

            // Specular center
            dl->AddCircleFilled(dotPos, baseR * 0.45f, static_cast<ImU32>(tokens.text.primary));
        };

        drawDot(u0);

        if (dx > 100.0f) {
            const float u1 = std::fmod(u0 + 0.5f, 1.0f);
            drawDot(u1);
        }
    }
}

static void drawRoutingWire(ImDrawList* dl, ImVec2 p1, ImVec2 p2, bool /*withArrow*/ = false) {
    drawCubicHermiteCable(dl, p1, p2, 0.3f, false, static_cast<float>(ImGui::GetTime()));
}

static bool renderCenteredPlusButton(const char* id, const ImVec2& size = ImVec2(52, 52), float iconRadius = 11.0f) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();

    ImU32 bgCol;
    if (held)         bgCol = tokens.surfaces.buttonBgActive;
    else if (hovered) bgCol = tokens.surfaces.buttonBgHovered;
    else              bgCol = tokens.surfaces.buttonBg;

    ImU32 borderCol = hovered ? static_cast<ImU32>(tokens.borders.focus) : static_cast<ImU32>(tokens.borders.subtle);
    ImU32 iconCol   = hovered ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.secondary);

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 8.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 8.0f, 0, hovered ? 1.5f : 1.0f);

    float cx = std::floor(pos.x + size.x * 0.5f);
    float cy = std::floor(pos.y + size.y * 0.5f);

    const float halfBar = iconRadius;
    const float thick = 1.5f;
    dl->AddRectFilled(ImVec2(cx - halfBar, cy - thick), ImVec2(cx + halfBar, cy + thick), iconCol, 1.0f);
    dl->AddRectFilled(ImVec2(cx - thick, cy - halfBar), ImVec2(cx + thick, cy + halfBar), iconCol, 1.0f);

    return clicked;
}

RackView::RackView(audio::GraphEngine& graph,
                   audio::AsioManager& asio,
                   tools::InstrumentTuner& tuner,
                   tools::Metronome& metronome,
                   tools::AudioPlayer& player,
                   tools::QuickLooper& looper,
                   midi::MidiManager& midi,
                   state::SceneManager& scenes,
                   plugins::PluginScanner& scanner)
    : m_graph(graph),
      m_asio(asio),
      m_tuner(tuner),
      m_metronome(metronome),
      m_player(player),
      m_looper(looper),
      m_midi(midi),
      m_scenes(scenes),
      m_scanner(scanner),
      m_pluginBrowserModal(std::make_unique<PluginBrowserModal>(scanner, graph)),
      m_settingsModal(std::make_unique<SettingsModal>(asio, scanner, [this]() { m_pluginBrowserModal->open(); })),
      m_practiceToolsModal(std::make_unique<PracticeToolsModal>(looper, player)) {
}

void RackView::render() {
    ThumbnailManager::instance().update();
    if (m_expandPulseTimer > 0.0f) {
        m_expandPulseTimer -= ImGui::GetIO().DeltaTime * 2.8f;
        if (m_expandPulseTimer <= 0.0f) {
            m_expandPulseTimer = 0.0f;
            m_expandedSlot = nullptr;
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("PraccyMainWindow", nullptr, windowFlags)) {
        renderPracticeRibbon();
        ImGui::Separator();
        renderSceneBar();
        ImGui::Separator();
        renderSignalRack();
        renderBottomBar();

        // Keyboard shortcuts
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
                m_pluginBrowserModal->open();
            }

            for (int k = 0; k < 8 && k < static_cast<int>(m_scenes.numScenes()); ++k) {
                if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + k), false)) {
                    if (m_scenes.applyScene(k, m_graph)) {
                        triggerHudToast("PRESET " + std::to_string(k + 1) + " RECALLED");
                    }
                }
            }
        }

        m_pluginBrowserModal->render();
        renderDspTweakModal();
        renderUpdateModal();
        m_settingsModal->render();
        m_practiceToolsModal->render();
        renderFloatingHudToast();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void RackView::renderPraccyLogo() {
    Thumbnail* logoThumb = ThumbnailManager::instance().getThumbnail("__praccy_logo__");
    if (!logoThumb || !logoThumb->srv) {
        // Resolve paths relative to the executable so we find the file
        // regardless of what working directory the app was launched from.
        wchar_t exePathW[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
        std::filesystem::path exeDir = std::filesystem::path(exePathW).parent_path();

        std::vector<std::filesystem::path> searchPaths = {
            exeDir / "resources" / "icon_512.png",
            std::filesystem::path("resources") / "icon_512.png",
        };

        for (const auto& p : searchPaths) {
            if (std::filesystem::exists(p)) {
                ThumbnailManager::instance().loadFromFile("__praccy_logo__", p.string().c_str());
                logoThumb = ThumbnailManager::instance().getThumbnail("__praccy_logo__");
                if (logoThumb && logoThumb->srv) break;
            }
        }
    }

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();

    // Square card to match the square icon aspect ratio
    const float boxW = 74.0f;
    const float boxH = 74.0f;

    // Stylized logo card
    dl->AddRectFilled(pos, ImVec2(pos.x + boxW, pos.y + boxH), tokens.surfaces.headerBg, 6.0f);
    dl->AddRect(pos, ImVec2(pos.x + boxW, pos.y + boxH), tokens.borders.subtle, 6.0f);

    if (logoThumb && logoThumb->srv) {
        // Fill the full card area with the icon
        dl->AddImage((ImTextureID)logoThumb->srv, pos, ImVec2(pos.x + boxW, pos.y + boxH),
                     ImVec2(0, 0), ImVec2(1, 1));
    } else {
        // Fallback procedural medallion
        const float cx = pos.x + 24.0f;
        const float cy = pos.y + (boxH * 0.5f);

        dl->AddCircleFilled(ImVec2(cx, cy), 16.0f, static_cast<ImU32>(ColorToken(
            static_cast<uint8_t>(tokens.signal.active.r * 255.0f),
            static_cast<uint8_t>(tokens.signal.active.g * 255.0f),
            static_cast<uint8_t>(tokens.signal.active.b * 255.0f),
            35
        )));
        dl->AddCircleFilled(ImVec2(cx, cy), 12.0f, tokens.surfaces.cardBg);
        dl->AddCircle(ImVec2(cx, cy), 12.0f, tokens.text.accent, 0, 1.5f);

        dl->AddLine(ImVec2(cx - 5.0f, cy - 4.0f), ImVec2(cx - 5.0f, cy + 4.0f), tokens.text.accent, 1.5f);
        dl->AddLine(ImVec2(cx - 1.5f, cy - 8.0f), ImVec2(cx - 1.5f, cy + 8.0f), tokens.text.primary, 2.0f);
        dl->AddLine(ImVec2(cx + 2.0f, cy - 6.0f), ImVec2(cx + 2.0f, cy + 6.0f), tokens.text.accent, 1.8f);
        dl->AddLine(ImVec2(cx + 5.5f, cy - 3.0f), ImVec2(cx + 5.5f, cy + 3.0f), tokens.text.accent, 1.5f);

        // No logo fallback - nothing displayed
    }

    ImGui::Dummy(ImVec2(boxW, boxH));
}

bool RackView::renderStatusPill(bool isRunning, const char* statusText) {
    if (!statusText) return false;

    const ImVec2 textSize = ImGui::CalcTextSize(statusText);
    const float pillW = textSize.x + 36.0f;
    const float pillH = 22.0f;
    const float frameH = ImGui::GetFrameHeight();
    const float offsetY = std::max(0.0f, (frameH - pillH) * 0.5f);

    const ImVec2 screenPos = ImGui::GetCursorScreenPos();
    const ImVec2 pos(screenPos.x, screenPos.y + offsetY);

    bool clicked = ImGui::InvisibleButton("##DspStatusPill", ImVec2(pillW, frameH));
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    if (clicked) {
        resetDspDropouts();
    }

    uint32_t drops = m_dspDropouts ? m_dspDropouts->load(std::memory_order_relaxed) : 0;

    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (drops > 0) {
            ImGui::SetTooltip("Real-time DSP Load %% & Dropout Meter\nDropouts: %u\nClick to reset dropout counter", drops);
        } else {
            ImGui::SetTooltip("Real-time DSP Load %% & Dropout Meter\nClick to reset dropout counter");
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();

    // Background capsule pill
    ImU32 bgCol;
    ImU32 borderCol;
    if (isRunning) {
        if (drops > 0) {
            bgCol = held ? static_cast<ImU32>(tokens.surfaces.frameBgActive) : (hovered ? static_cast<ImU32>(tokens.surfaces.frameBgHovered) : static_cast<ImU32>(tokens.surfaces.frameBg));
            borderCol = held ? static_cast<ImU32>(tokens.borders.focus) : (hovered ? static_cast<ImU32>(tokens.borders.cardGlowActive) : static_cast<ImU32>(tokens.borders.strong));
        } else {
            bgCol = held ? static_cast<ImU32>(tokens.surfaces.frameBgActive) : (hovered ? static_cast<ImU32>(tokens.surfaces.frameBgHovered) : static_cast<ImU32>(tokens.surfaces.frameBg));
            borderCol = held ? static_cast<ImU32>(tokens.borders.cardGlowOpen) : (hovered ? static_cast<ImU32>(tokens.borders.focus) : static_cast<ImU32>(tokens.borders.subtle));
        }
    } else {
        bgCol = tokens.surfaces.cardBgFaulted;
        borderCol = tokens.borders.cardFaulted;
    }

    dl->AddRectFilled(pos, ImVec2(pos.x + pillW, pos.y + pillH), bgCol, 11.0f);
    dl->AddRect(pos, ImVec2(pos.x + pillW, pos.y + pillH), borderCol, 11.0f);

    // Glowing LED Dot
    ImVec2 dotCenter(pos.x + 12.0f, pos.y + (pillH * 0.5f));
    if (isRunning) {
        if (drops > 0) {
            dl->AddCircleFilled(dotCenter, 6.0f, static_cast<ImU32>(ColorToken(
                static_cast<uint8_t>(tokens.signal.meterWarning.r * 255.0f),
                static_cast<uint8_t>(tokens.signal.meterWarning.g * 255.0f),
                static_cast<uint8_t>(tokens.signal.meterWarning.b * 255.0f),
                80
            )));
            dl->AddCircleFilled(dotCenter, 3.5f, tokens.signal.meterWarning);
        } else {
            dl->AddCircleFilled(dotCenter, 6.0f, static_cast<ImU32>(ColorToken(
                static_cast<uint8_t>(tokens.signal.active.r * 255.0f),
                static_cast<uint8_t>(tokens.signal.active.g * 255.0f),
                static_cast<uint8_t>(tokens.signal.active.b * 255.0f),
                80
            )));
            dl->AddCircleFilled(dotCenter, 3.5f, tokens.signal.active);
        }
    } else {
        dl->AddCircleFilled(dotCenter, 3.5f, tokens.signal.faulted);
    }

    // Text Label inside pill
    ImVec2 textPos(pos.x + 24.0f, pos.y + ((pillH - textSize.y) * 0.5f));
    ImU32 textCol;
    if (isRunning) {
        if (drops > 0) {
            textCol = hovered ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.warning);
        } else {
            textCol = hovered ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.success);
        }
    } else {
        textCol = hovered ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.error);
    }
    dl->AddText(textPos, textCol, statusText);

    return clicked;
}

void RackView::renderPracticeRibbon() {
    const auto& tokens = themeTokens();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.panelBg.vec4);
    ImGui::BeginChild("PracticeRibbon", ImVec2(0, 88.0f), false, ImGuiWindowFlags_NoScrollbar);

    // Responsive 4-column layout: Tuner, Metro, Gate, Master — all stretch
    if (ImGui::BeginTable("PracticeRibbonTable", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("TunerCol",  ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("MetroCol",  ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("GateCol",   ImGuiTableColumnFlags_WidthStretch, 0.20f);
        ImGui::TableSetupColumn("MasterCol", ImGuiTableColumnFlags_WidthStretch, 0.20f);

        const float modH = 74.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.panelBg.vec4);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));



        // ----------------------------------------------------
        // Column 1: Strobe Tuner Module
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::BeginChild("Mod_Tuner", ImVec2(0, modH), true, ImGuiWindowFlags_NoScrollbar);
        {
            auto result = m_tuner.currentResult();
            ImGui::TextColored(tokens.text.muted.vec4, "TUNER");
            ImGui::SameLine();
            if (result.confidence) {
                bool inTune = std::abs(result.centDeviation) <= 3.0f;
                ImVec4 noteColor = inTune ? tokens.signal.tunerInTune.vec4 : tokens.signal.tunerFlat.vec4;
                ImGui::TextColored(noteColor, "%s (%.1f Hz)", result.noteName.c_str(), result.frequencyHz);
            } else {
                ImGui::TextDisabled("(Listening)");
            }

            ImGui::SetCursorPosY(38.0f);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float gaugeW = std::max(60.0f, ImGui::GetContentRegionAvail().x - 4.0f);
            const float gaugeH = 22.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            const auto& tokens = themeTokens();
            dl->AddRectFilled(pos, ImVec2(pos.x + gaugeW, pos.y + gaugeH), tokens.surfaces.frameBg, 4.0f);
            dl->AddRect(pos, ImVec2(pos.x + gaugeW, pos.y + gaugeH), tokens.borders.subtle, 4.0f);

            const float midX = pos.x + (gaugeW * 0.5f);
            dl->AddLine(ImVec2(midX, pos.y + 2), ImVec2(midX, pos.y + gaugeH - 2), tokens.signal.tunerInTune, 2.0f);
            dl->AddLine(ImVec2(pos.x + gaugeW * 0.25f, pos.y + 4), ImVec2(pos.x + gaugeW * 0.25f, pos.y + gaugeH - 4), tokens.borders.separator, 1.0f);
            dl->AddLine(ImVec2(pos.x + gaugeW * 0.75f, pos.y + 4), ImVec2(pos.x + gaugeW * 0.75f, pos.y + gaugeH - 4), tokens.borders.separator, 1.0f);

            if (result.confidence) {
                bool inTune = std::abs(result.centDeviation) <= 3.0f;
                float norm = (result.centDeviation + 50.0f) / 100.0f;
                norm = std::clamp(norm, 0.0f, 1.0f);
                float needleX = pos.x + (norm * gaugeW);
                ImU32 needleColor = inTune ? static_cast<ImU32>(tokens.signal.tunerInTune) : static_cast<ImU32>(tokens.signal.tunerSharp);
                dl->AddRectFilled(ImVec2(needleX - 2.0f, pos.y + 1), ImVec2(needleX + 2.0f, pos.y + gaugeH - 1), needleColor, 2.0f);
            } else {
                dl->AddCircleFilled(ImVec2(midX, pos.y + gaugeH * 0.5f), 3.0f, tokens.text.muted);
            }
            ImGui::Dummy(ImVec2(gaugeW, gaugeH));
        }
        ImGui::EndChild();

        // ----------------------------------------------------
        // Column 2: Metronome Module
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::BeginChild("Mod_Metro", ImVec2(0, modH), true, ImGuiWindowFlags_NoScrollbar);
        {
            ImGui::TextColored(tokens.text.muted.vec4, "METRONOME");
            ImGui::SameLine(0, 8);
            bool metroPlaying = m_metronome.isPlaying();

            // Beat LEDs beside title
            {
                const int totalBeats = m_metronome.beatsPerBar();
                const int currentBeat = m_metronome.currentBeat();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const auto& tokens = themeTokens();
                ImVec2 curPos = ImGui::GetCursorScreenPos();
                const float dotRadius = 4.0f;
                const float dotSpacing = 11.0f;
                float fontH = ImGui::GetFontSize();
                float cy = curPos.y + (fontH * 0.5f);

                for (int b = 0; b < totalBeats; ++b) {
                    float cx = curPos.x + 4.0f + (b * dotSpacing);
                    bool active = metroPlaying && (b == currentBeat);
                    if (active) {
                        ImU32 haloColor = (b == 0) ?
                            static_cast<ImU32>(ColorToken(
                                static_cast<uint8_t>(tokens.signal.faulted.r * 255.0f),
                                static_cast<uint8_t>(tokens.signal.faulted.g * 255.0f),
                                static_cast<uint8_t>(tokens.signal.faulted.b * 255.0f),
                                80)) :
                            static_cast<ImU32>(ColorToken(
                                static_cast<uint8_t>(tokens.signal.meterWarning.r * 255.0f),
                                static_cast<uint8_t>(tokens.signal.meterWarning.g * 255.0f),
                                static_cast<uint8_t>(tokens.signal.meterWarning.b * 255.0f),
                                80));
                        dl->AddCircleFilled(ImVec2(cx, cy), dotRadius + 2.5f, haloColor);
                        ImU32 ledColor = (b == 0) ? static_cast<ImU32>(tokens.signal.faulted) : static_cast<ImU32>(tokens.signal.meterWarning);
                        dl->AddCircleFilled(ImVec2(cx, cy), dotRadius, ledColor);
                    } else {
                        dl->AddCircleFilled(ImVec2(cx, cy), dotRadius, tokens.surfaces.cardBg);
                        dl->AddCircle(ImVec2(cx, cy), dotRadius, tokens.borders.subtle, 0, 1.0f);
                    }
                }
                ImGui::Dummy(ImVec2(totalBeats * dotSpacing + 4.0f, fontH));
            }

            const float gap = 6.0f;
            const float playW = 50.0f;
            const float sigW = 68.0f;

            // Position TAP button in top row directly aligned above the TimeSig dropdown
            const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            const float comboStartX = rightEdge - sigW;

            ImGui::SetCursorPos(ImVec2(comboStartX, 6.0f));

            auto now = std::chrono::steady_clock::now();
            bool tapFlash = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTapFlash).count() < 120;
            if (tapFlash) {
                ImGui::PushStyleColor(ImGuiCol_Button, tokens.text.accent.vec4);
                ImGui::PushStyleColor(ImGuiCol_Text, tokens.text.inverse.vec4);
            }
            if (CenteredButton("TAP", ImVec2(sigW, 24.0f))) {
                m_lastTapFlash = now;
                // Remove tap times older than 2.5 seconds
                while (!m_tapTimes.empty() && std::chrono::duration<double>(now - m_tapTimes.front()).count() > 2.5) {
                    m_tapTimes.erase(m_tapTimes.begin());
                }
                if (!m_tapTimes.empty() && std::chrono::duration<double>(now - m_tapTimes.back()).count() > 2.5) {
                    m_tapTimes.clear();
                }
                m_tapTimes.push_back(now);
                if (m_tapTimes.size() > 6) {
                    m_tapTimes.erase(m_tapTimes.begin());
                }
                if (m_tapTimes.size() >= 2) {
                    double totalDurationSec = std::chrono::duration<double>(m_tapTimes.back() - m_tapTimes.front()).count();
                    double avgIntervalSec = totalDurationSec / static_cast<double>(m_tapTimes.size() - 1);
                    if (avgIntervalSec > 0.05) {
                        float computedBpm = static_cast<float>(60.0 / avgIntervalSec);
                        computedBpm = std::clamp(std::round(computedBpm), 40.0f, 260.0f);
                        m_metronome.setBpm(computedBpm);
                    }
                }
            }
            if (tapFlash) {
                ImGui::PopStyleColor(2);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tap Tempo (Tap repeatedly to set BPM)");

            ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, 36.0f));
            float bpmW = std::max(60.0f, comboStartX - gap - (ImGui::GetStyle().WindowPadding.x + playW + gap));

            // Push FramePadding so PLAY, Tempo button/input, and TimeSig Combo share exact 24px height
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.5f));

            if (metroPlaying) {
                ImGui::PushStyleColor(ImGuiCol_Button, tokens.signal.faulted.vec4);
                ImGui::PushStyleColor(ImGuiCol_Text, tokens.text.primary.vec4);
            }
            if (CenteredButton(metroPlaying ? "STOP##M" : "PLAY##M", ImVec2(playW, 24.0f))) {
                m_metronome.setPlaying(!metroPlaying);
            }
            if (metroPlaying) {
                ImGui::PopStyleColor(2);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(metroPlaying ? "Stop Metronome" : "Start Metronome");

            ImGui::SameLine(0, gap);

            // Tempo Button / Direct Click-to-Type Input
            float curBpm = m_metronome.bpm();
            if (m_tempoEditing) {
                ImGui::SetNextItemWidth(bpmW);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputInt("##BPM_Edit", &m_tempoEditValue, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    m_tempoEditValue = std::clamp(m_tempoEditValue, 40, 260);
                    m_metronome.setBpm(static_cast<float>(m_tempoEditValue));
                    m_tempoEditing = false;
                }
                if (ImGui::IsItemDeactivated()) {
                    m_tempoEditValue = std::clamp(m_tempoEditValue, 40, 260);
                    m_metronome.setBpm(static_cast<float>(m_tempoEditValue));
                    m_tempoEditing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    m_tempoEditing = false;
                }
            } else {
                char bpmLabel[32];
                std::snprintf(bpmLabel, sizeof(bpmLabel), "%.0f BPM##BPMBtn", curBpm);
                if (CenteredButton(bpmLabel, ImVec2(bpmW, 24.0f))) {
                    m_tempoEditing = true;
                    m_tempoEditValue = static_cast<int>(std::round(curBpm));
                }
                if (ImGui::IsItemHovered()) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        m_metronome.setBpm(120.0f);
                        m_tempoEditing = false;
                    }
                    ImGui::SetTooltip("Tempo: %.0f BPM\nSingle-click to type tempo\nDouble-click to reset to 120 BPM", curBpm);
                }
            }

            ImGui::SameLine(comboStartX);

            const char* timeSigOptions[] = { "4/4", "3/4", "2/4", "6/8" };
            const int timeSigBeats[] = { 4, 3, 2, 6 };
            int currentSigIdx = 0;
            int currentBeats = m_metronome.beatsPerBar();
            for (int i = 0; i < 4; ++i) {
                if (timeSigBeats[i] == currentBeats) { currentSigIdx = i; break; }
            }
            ImGui::SetNextItemWidth(sigW);
            if (ImGui::Combo("##TimeSig", &currentSigIdx, timeSigOptions, 4)) {
                m_metronome.setBeatsPerBar(timeSigBeats[currentSigIdx]);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Time Signature");

            ImGui::PopStyleVar();
        }
        ImGui::EndChild();

        // ----------------------------------------------------
        // Column 3: Noise Gate Module
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::BeginChild("Mod_Gate", ImVec2(0, modH), true, ImGuiWindowFlags_NoScrollbar);
        {
            auto& gate = m_graph.inputNoiseGate();
            bool gateOn = gate.isEnabled();
            float availW = ImGui::GetContentRegionAvail().x;

            ImGui::TextColored(tokens.text.muted.vec4, "NOISE GATE");
            if (availW > 80.0f) {
                ImGui::SameLine(availW - 65.0f);
            } else {
                ImGui::SameLine();
            }
            if (ImGui::Checkbox("Active##Gate", &gateOn)) {
                gate.setEnabled(gateOn);
            }

            ImGui::SetCursorPosY(38.0f);
            float thresh = gate.thresholdDb();
            ImGui::SetNextItemWidth(std::max(80.0f, availW - 4.0f));
            if (ResettableSliderFloat("##GateThresh", &thresh, -80.0f, -20.0f, -60.0f, "Thresh: %.0f dB")) {
                gate.setThresholdDb(thresh);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gate Threshold (Double-click to reset -60 dB)");
        }
        ImGui::EndChild();

        // ----------------------------------------------------
        // Column 4: Master Output Module
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::BeginChild("Mod_Master", ImVec2(0, modH), true, ImGuiWindowFlags_NoScrollbar);
        {
            float masterVol = m_graph.masterVolumeDb();
            float availMasterW = ImGui::GetContentRegionAvail().x;

            ImGui::TextColored(tokens.text.primary.vec4, "MASTER OUTPUT");

            char valStr[32];
            std::snprintf(valStr, sizeof(valStr), "%+.1f dB", masterVol);
            float valTextW = ImGui::CalcTextSize(valStr).x;
            if (availMasterW > valTextW + 110.0f) {
                ImGui::SameLine(availMasterW - valTextW - 4.0f);
            } else {
                ImGui::SameLine();
            }
            ImGui::TextColored(tokens.text.primary.vec4, "%s", valStr);

            ImGui::SetCursorPosY(38.0f);
            const float meterW = 9.0f;
            const float meterGap = 3.0f;
            const float sliderW = std::max(60.0f, availMasterW - (meterW * 2.0f + meterGap + 12.0f));

            ImGui::SetNextItemWidth(sliderW);
            if (ResettableSliderFloat("##MasterVol", &masterVol, -36.0f, +6.0f, 0.0f, "Vol: %+.1f dB")) {
                m_graph.setMasterVolumeDb(masterVol);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Master Volume (Double-click to reset 0.0 dB)");

            ImGui::SameLine(0, 6);
            renderMeter("OutMeterL", m_graph.outputMeter().peakLeft(), meterW, 22.0f);
            ImGui::SameLine(0, meterGap);
            renderMeter("OutMeterR", m_graph.outputMeter().peakRight(), meterW, 22.0f);
        }
        ImGui::EndChild();

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        ImGui::EndTable();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void RackView::renderSceneBar() {
    const auto& tokens = themeTokens();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(tokens.text.secondary.vec4, "SCENE PRESETS:");
    ImGui::SameLine(0, 12);

    for (size_t s = 0; s < m_scenes.numScenes(); ++s) {
        const auto* sc = m_scenes.getScene(s);
        if (!sc) continue;

        bool isActive = (static_cast<int>(s) == m_scenes.activeSceneIndex());
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, tokens.signal.active.vec4);
            ImGui::PushStyleColor(ImGuiCol_Text, tokens.text.primary.vec4);
        }

        if (CenteredButton(sc->name.c_str())) {
            m_scenes.applyScene(static_cast<int>(s), m_graph);
            triggerHudToast("Loaded " + sc->name);
        }

        if (isActive) {
            ImGui::PopStyleColor(2);
        }
        ImGui::SameLine(0, 8);
    }

    if (CenteredButton("Save Scene")) {
        m_scenes.captureCurrentScene(m_scenes.activeSceneIndex(), m_graph);
        const auto* cur = m_scenes.getScene(m_scenes.activeSceneIndex());
        triggerHudToast("Saved to " + (cur ? cur->name : "Scene"));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Save current plugin chain into active scene preset");
    }

    ImGui::SameLine(0, 10);
    if (CenteredButton("Save Preset As...")) {
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
                    triggerHudToast("Loaded preset: " + pName);
                }
            }
            ImGui::EndCombo();
        }
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
            if (CenteredButton("Save", ImVec2(100, 24))) {
                if (m_presetNameBuffer[0] != '\0') {
                    m_scenes.savePresetChain(m_presetNameBuffer, m_graph);
                    triggerHudToast(std::string("Preset '") + m_presetNameBuffer + "' saved!");
                    m_showSavePresetModal = false;
                }
            }
            ImGui::SameLine();
            if (CenteredButton("Cancel", ImVec2(80, 24))) {
                m_showSavePresetModal = false;
            }
            ImGui::End();
        }
    }
}

void RackView::renderSignalRack() {
    m_graph.processReclamation();

    const auto& tokens = themeTokens();
    ImGui::TextColored(tokens.text.muted.vec4, "SIGNAL CHAIN (RACK):");

    ImGui::BeginChild("RackScrollArea", ImVec2(0, -38), true, ImGuiWindowFlags_HorizontalScrollbar);

    const size_t numNodes = m_graph.numNodes();
    bool hasParallel = false;
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = m_graph.getNode(i);
        if (node && node->type() == audio::NodeType::ParallelSplitMerge) {
            hasParallel = true;
            break;
        }
    }

    const float cardW = 240.0f;
    const float cardH = 224.0f;
    const float wireW = 32.0f;
    const float insertCardW = 80.0f;
    const float outputCardW = 100.0f;
    const float inputCardW = 180.0f;

    // 1. Analytical Pre-computation of Total Chain Footprint
    float totalContentWidth = inputCardW; // 180px

    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = m_graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            totalContentWidth += (wireW + cardW); // +32px + 240px = 272px
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            const float envPadX = 14.0f;
            const float slotWireW = 28.0f;
            const size_t n0 = (block && block->getBranch(0)) ? block->getBranch(0)->numSlots() : 0;
            const size_t n1 = (block && block->getBranch(1)) ? block->getBranch(1)->numSlots() : 0;
            const float w0 = envPadX + (n0 * (cardW + slotWireW)) + insertCardW + envPadX;
            const float w1 = envPadX + (n1 * (cardW + slotWireW)) + insertCardW + envPadX;
            const float envelopeW = std::max({ w0, w1, 460.0f });

            // InWire(24) + Fork(36) + Envelope + Conv(36) + Comb(104) = Envelope + 200px
            totalContentWidth += (24.0f + 36.0f + envelopeW + 36.0f + 104.0f);
        }
    }

    // Insert serial card (+32px wire + 80px card = 112px)
    totalContentWidth += (wireW + insertCardW);

    // Output destination card (+32px wire + 100px card = 132px)
    totalContentWidth += (wireW + outputCardW);

    const float totalContentHeight = hasParallel ? 616.0f : 240.0f;

    // 2. Viewport Dimension Acquisition & Dynamic Centering
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float viewportWidth = avail.x;
    const float viewportHeight = avail.y;

    const float safeW = (std::isnan(viewportWidth) || viewportWidth <= 0.0f) ? 0.0f : viewportWidth;
    const float safeH = (std::isnan(viewportHeight) || viewportHeight <= 0.0f) ? 0.0f : viewportHeight;
    const float offsetX = std::max(20.0f, (safeW - totalContentWidth) * 0.5f);
    const float offsetY = std::max(16.0f, (safeH - totalContentHeight) * 0.5f);

    float centerY = 120.0f;
    float serialCardY = 8.0f;
    float rackTotalH = 240.0f;

    if (hasParallel) {
        centerY = offsetY + 310.0f;
        serialCardY = centerY - (cardH * 0.5f); // offsetY + 198.0f
        rackTotalH = offsetY + 600.0f;
    } else {
        serialCardY = offsetY + 8.0f;
        centerY = serialCardY + (cardH * 0.5f);  // offsetY + 120.0f
        rackTotalH = offsetY + cardH + 16.0f;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    float scrollX = ImGui::GetScrollX();
    float scrollY = ImGui::GetScrollY();

    auto toScreen = [&](float lx, float ly) -> ImVec2 {
        return ImVec2(winPos.x + lx - scrollX, winPos.y + ly - scrollY);
    };

    float currentX = offsetX;
    float animTime = static_cast<float>(ImGui::GetTime());

    // 1. Input Node Card
    ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
    renderInputCard(serialCardY);
    currentX += inputCardW;

    float prevPeak = std::max(m_graph.inputMeter().peakLeft(), m_graph.inputMeter().peakRight());

    // 2. Render Graph Nodes
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = m_graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            bool bypassed = slot ? slot->isBypassed() : false;

            drawCubicHermiteCable(dl, toScreen(currentX, centerY), toScreen(currentX + wireW, centerY),
                                 prevPeak, false, animTime);
            currentX += wireW;

            ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
            renderPluginSlot(slot, static_cast<int>(i), -1, -1);
            currentX += cardW;

            if (slot) {
                prevPeak = bypassed ? 0.0f : std::max(slot->meter().peakLeft(), slot->meter().peakRight());
            }
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            drawCubicHermiteCable(dl, toScreen(currentX, centerY), toScreen(currentX + 24.0f, centerY),
                                 prevPeak, false, animTime);
            currentX += 24.0f;

            ImGui::SetCursorPos(ImVec2(currentX, 0.0f));
            renderParallelBlock(block, static_cast<int>(i), centerY);
            currentX = ImGui::GetCursorPosX();

            prevPeak = 0.5f; // Active parallel mix signal
        }
    }

    // 3. Serial Rack Insertion Slot Card (+ Add Plugin)
    drawCubicHermiteCable(dl, toScreen(currentX, centerY), toScreen(currentX + wireW, centerY),
                         prevPeak, false, animTime);
    currentX += wireW;

    ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, static_cast<ImVec4>(tokens.surfaces.cardBg));
    ImGui::BeginChild("InsertSerialCard", ImVec2(insertCardW, cardH), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2((insertCardW - 48.0f) * 0.5f, (cardH - 48.0f) * 0.5f));
    if (renderCenteredPlusButton("##AddPluginSerial", ImVec2(48.0f, 48.0f), 11.0f)) {
        m_pluginBrowserModal->open(-1, -1);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Insert a new plugin into the signal chain");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    currentX += insertCardW;

    // 4. Output Node Card
    drawCubicHermiteCable(dl, toScreen(currentX, centerY), toScreen(currentX + wireW, centerY),
                         prevPeak, false, animTime);
    currentX += wireW;

    ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, static_cast<ImVec4>(tokens.surfaces.cardBg));
    ImGui::BeginChild("OutputDestNode", ImVec2(100.0f, cardH), true);
    renderBadgePill("OUTPUT", tokens.surfaces.branchABg, tokens.borders.branchABorder, tokens.text.badgeVst3, 22.0f);
    ImGui::Spacing();
    ImGui::TextDisabled("To ASIO");
    ImGui::Spacing();
    renderMeter("FinalL", m_graph.outputMeter().peakLeft(), 14, 120);
    ImGui::SameLine(0, 6);
    renderMeter("FinalR", m_graph.outputMeter().peakRight(), 14, 120);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    currentX += 100.0f;

    // 3. Right Extent Boundary & Clean Horizontal Scrolling
    const float rightMargin = (totalContentWidth < viewportWidth)
        ? (offsetX + totalContentWidth)
        : (currentX + 32.0f);

    ImGui::SetCursorPos(ImVec2(rightMargin, ImMax(rackTotalH, viewportHeight)));
    ImGui::Dummy(ImVec2(0, 0));

    ImGui::EndChild();
}

void RackView::renderSignalCable(float width) {
    ImGui::SameLine(0, 0);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float cardH = 224.0f;
    const float centerY = pos.y + (cardH * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    drawCubicHermiteCable(dl, ImVec2(pos.x, centerY), ImVec2(pos.x + width, centerY),
                         0.25f, false, static_cast<float>(ImGui::GetTime()));

    ImGui::Dummy(ImVec2(width, cardH));
    ImGui::SameLine(0, 0);
}

void RackView::renderInputCard(float cardY) {
    if (cardY >= 0.0f) {
        ImGui::SetCursorPosY(cardY);
    }
    const auto& tokens = themeTokens();
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, static_cast<ImVec4>(tokens.surfaces.cardBg));
    ImGui::BeginChild("InputGainNode", ImVec2(180, 224), true);
    renderBadgePill("INPUT", tokens.surfaces.branchABg, tokens.borders.branchABorder, tokens.text.badgeVst3, 22.0f);
    ImGui::Spacing();

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
    if (ResettableVSliderFloat("##InGain", ImVec2(24, 115), &inGain, -24.0f, +24.0f, 0.0f, "")) {
        m_graph.setInputGainDb(inGain);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Input Gain (Double-click to reset 0 dB)");
    ImGui::SameLine(0, 8);
    renderMeter("InL", m_graph.inputMeter().peakLeft(), 10, 115);
    ImGui::SameLine(0, 4);
    renderMeter("InR", m_graph.inputMeter().peakRight(), 10, 115);

    ImGui::SameLine(0, 8);
    ImGui::BeginGroup();
    ImGui::Text("%.0fdB", inGain);
    ImGui::TextDisabled("Gain");
    ImGui::EndGroup();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

void RackView::renderPluginSlot(audio::PluginSlot* slot, int slotIndex, int blockIndex, int branchIndex) {
    if (!slot) return;

    ImGui::BeginGroup();
    const auto& tokens = themeTokens();
    auto* pluginInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    const bool isFaulted = pluginInst && pluginInst->isFaulted();
    const bool bypassed = slot->isBypassed();

    ImVec4 cardBg = isFaulted ? static_cast<ImVec4>(tokens.surfaces.cardBgFaulted) :
                    (bypassed ? static_cast<ImVec4>(tokens.surfaces.cardBgBypassed) : static_cast<ImVec4>(tokens.surfaces.cardBg));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg);
    char childId[64];
    std::snprintf(childId, sizeof(childId), "SlotCard_%d_%d_%d", slotIndex, blockIndex, branchIndex);

    // Standardized exact dimensions: 240 x 224 px
    const float cardWidth = 240.0f;
    const float cardHeight = 224.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild(childId, ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar);

    bool hasGui = pluginInst && pluginInst->hasCustomGui();
    bool isWindowOpen = false;
    if (hasGui) {
        isWindowOpen = plugins::PluginWindowManager::instance().isWindowOpen(pluginInst);
    } else if (m_dspTweakSlot == slot) {
        isWindowOpen = true;
    }

    // =========================================================================
    // SECTION 1: HEADER BAR (H = 24px, Y = 8px..32px)
    // =========================================================================
    {
        ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));

        // Format pill badge
        const char* badgeStr = "DSP";
        ImU32 badgeTextCol = tokens.text.badgeInternal;
        if (pluginInst) {
            if (dynamic_cast<plugins::Vst3PluginInstance*>(pluginInst)) {
                badgeStr = "VST3";
                badgeTextCol = tokens.text.badgeVst3;
            } else if (dynamic_cast<plugins::ClapPluginInstance*>(pluginInst)) {
                badgeStr = "CLAP";
                badgeTextCol = tokens.text.badgeClap;
            }
        }
        renderBadgePill(badgeStr, tokens.surfaces.frameBg, tokens.borders.focus, badgeTextCol, 18.0f);
        ImGui::SameLine(0, 6.0f);

        // Truncated title with ellipsis
        std::string rawName = slot->name();
        float availTitleW = 100.0f; // Leaves room for right action buttons & pill toggle
        std::string displayTitle = rawName;
        if (ImGui::CalcTextSize(displayTitle.c_str()).x > availTitleW) {
            while (!displayTitle.empty() && ImGui::CalcTextSize((displayTitle + "..").c_str()).x > availTitleW) {
                displayTitle.pop_back();
            }
            displayTitle += "..";
        }

        ImGui::AlignTextToFramePadding();
        const ImVec4 titleColor = isFaulted ? tokens.signal.faulted.vec4 :
                                  (bypassed ? tokens.signal.bypassed.vec4 : tokens.text.primary.vec4);
        ImGui::TextColored(titleColor, "%s", displayTitle.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s (%s)", rawName.c_str(), pluginInst ? pluginInst->vendor().c_str() : "Praccy Audio");
        }

        // Action buttons & Sliding Pill Toggle on far right
        ImGui::SameLine(cardWidth - 8.0f - 38.0f - (branchIndex == -1 ? 46.0f : 24.0f));

        if (branchIndex == -1) {
            char spId[32];
            std::snprintf(spId, sizeof(spId), "##Sp_%d", slotIndex);
            if (renderCenteredSplitButton(spId, ImVec2(18, 18))) {
                m_graph.splitSerialNodeIntoParallel(slotIndex);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                return;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split into Parallel Branches");
            ImGui::SameLine(0, 4.0f);
        }

        char delId[32];
        std::snprintf(delId, sizeof(delId), "##Del_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        if (renderCenteredDeleteButton(delId, ImVec2(18, 18))) {
            if (hasGui) plugins::PluginWindowManager::instance().closePluginWindow(pluginInst);
            if (m_dspTweakSlot == slot) m_dspTweakSlot = nullptr;
            if (m_expandedSlot == slot) m_expandedSlot = nullptr;
            if (branchIndex == -1) {
                m_graph.removeSerialNode(slotIndex);
            } else {
                auto* blk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(blockIndex));
                if (blk) {
                    auto* br = blk->getBranch(branchIndex);
                    if (br) br->removeSlot(slotIndex);
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            return;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete Slot");
        ImGui::SameLine(0, 5.0f);

        // Tactile Sliding Pill Toggle Switch (Active/Bypass)
        bool activeState = !bypassed;
        char pillId[32];
        std::snprintf(pillId, sizeof(pillId), "##Pill_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        if (renderSlidingPillToggle(pillId, &activeState, ImVec2(38.0f, 18.0f))) {
            slot->setBypassed(!activeState);
        }
    }

    // =========================================================================
    // SECTION 2: BODY SECTION (H = 96px, Y = 36px..132px)
    // =========================================================================
    {
        ImGui::SetCursorPos(ImVec2(8.0f, 36.0f));
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
        const float previewW = 224.0f;
        const float previewH = 96.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        Thumbnail* thumb = ThumbnailManager::instance().getThumbnail(slot->name());

        // Multi-layer Frame Glow Border
        ImU32 frameBg = isFaulted ? static_cast<ImU32>(tokens.surfaces.cardBgFaulted) :
                        (bypassed ? static_cast<ImU32>(tokens.surfaces.cardBgBypassed) : static_cast<ImU32>(tokens.surfaces.frameBg));
        dl->AddRectFilled(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), frameBg, 5.0f);

        // Ambient outer glow layer
        if (isFaulted) {
            dl->AddRect(ImVec2(previewPos.x - 2.0f, previewPos.y - 2.0f),
                        ImVec2(previewPos.x + previewW + 2.0f, previewPos.y + previewH + 2.0f),
                        ColorToken(tokens.signal.faulted.r, tokens.signal.faulted.g, tokens.signal.faulted.b, 0.25f), 7.0f, 0, 2.0f);
        } else if (!bypassed) {
            dl->AddRect(ImVec2(previewPos.x - 1.5f, previewPos.y - 1.5f),
                        ImVec2(previewPos.x + previewW + 1.5f, previewPos.y + previewH + 1.5f),
                        ColorToken(tokens.signal.active.r, tokens.signal.active.g, tokens.signal.active.b, 0.22f), 6.5f, 0, 2.0f);
        }

        // Invisible button to capture clicks over the entire preview area
        ImGui::SetCursorScreenPos(previewPos);
        char previewBtnId[64];
        std::snprintf(previewBtnId, sizeof(previewBtnId), "##GuiPreview_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        bool previewClicked = ImGui::InvisibleButton(previewBtnId, ImVec2(previewW, previewH));
        bool isPreviewHovered = ImGui::IsItemHovered();

        if (isPreviewHovered && !isFaulted) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        // Preview rendering:
        if (isFaulted) {
            const char* crashTitle = "PLUGIN CRASH ISOLATED";
            ImVec2 tSz = ImGui::CalcTextSize(crashTitle);
            dl->AddText(ImVec2(previewPos.x + (previewW - tSz.x) * 0.5f, previewPos.y + 16.0f),
                        tokens.signal.faulted, crashTitle);

            const char* crashSub = "Dry signal pass-through active";
            ImVec2 sSz = ImGui::CalcTextSize(crashSub);
            dl->AddText(ImVec2(previewPos.x + (previewW - sSz.x) * 0.5f, previewPos.y + 36.0f),
                        tokens.text.secondary, crashSub);

            ImGui::SetCursorScreenPos(ImVec2(previewPos.x + (previewW - 130.0f) * 0.5f, previewPos.y + 58.0f));
            char reloadBtnId[48];
            std::snprintf(reloadBtnId, sizeof(reloadBtnId), "RELOAD PLUGIN##%d_%d_%d", slotIndex, blockIndex, branchIndex);
            if (ImGui::Button(reloadBtnId, ImVec2(130.0f, 24.0f))) {
                pluginInst->resetFault();
            }
        } else if (thumb && thumb->srv) {
            float maxW = previewW - 4.0f;
            float maxH = previewH - 4.0f;
            float aspect = (thumb->height > 0) ? (static_cast<float>(thumb->width) / static_cast<float>(thumb->height)) : 1.0f;
            float drawW = maxW;
            float drawH = maxW / aspect;
            if (drawH > maxH) {
                drawH = maxH;
                drawW = maxH * aspect;
            }
            ImVec2 imgMin(std::floor(previewPos.x + (previewW - drawW) * 0.5f),
                          std::floor(previewPos.y + (previewH - drawH) * 0.5f));
            ImVec2 imgMax(imgMin.x + drawW, imgMin.y + drawH);

            ImU32 tintCol = bypassed ? ColorToken(tokens.text.primary.r, tokens.text.primary.g, tokens.text.primary.b, 0.65f).u32 : tokens.text.primary.u32;
            dl->AddImageRounded((ImTextureID)thumb->srv, imgMin, imgMax, ImVec2(0, 0), ImVec2(1, 1), tintCol, 4.0f);

            if (isWindowOpen) {
                const char* openTag = "● OPEN";
                ImVec2 oSz = ImGui::CalcTextSize(openTag);
                ImVec2 oPos(previewPos.x + previewW - oSz.x - 8.0f, previewPos.y + 6.0f);
                dl->AddRectFilled(ImVec2(oPos.x - 3.0f, oPos.y - 1.0f),
                                  ImVec2(oPos.x + oSz.x + 3.0f, oPos.y + oSz.y + 1.0f),
                                  ColorToken(tokens.signal.active.r, tokens.signal.active.g, tokens.signal.active.b, 0.25f), 3.0f);
                dl->AddText(oPos, tokens.signal.active, openTag);
            }

            if (isPreviewHovered) {
                dl->AddRectFilled(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), tokens.surfaces.modalOverlay, 5.0f);
                const char* expandPrompt = isWindowOpen ? "FOCUS WINDOW ↗" : "EXPAND GUI ↗";
                ImVec2 pSz = ImGui::CalcTextSize(expandPrompt);
                ImVec2 pPos(previewPos.x + (previewW - pSz.x) * 0.5f, previewPos.y + (previewH - pSz.y) * 0.5f);
                dl->AddRectFilled(ImVec2(pPos.x - 8.0f, pPos.y - 3.0f), ImVec2(pPos.x + pSz.x + 8.0f, pPos.y + pSz.y + 3.0f), tokens.surfaces.popupBg, 4.0f);
                dl->AddText(pPos, tokens.text.primary, expandPrompt);
            }
        } else {
            // High-Tech Procedural Vector Waveform Faceplate (Eliminated all inert dummy knobs!)
            float midY = previewPos.y + previewH * 0.5f;
            float leftX = previewPos.x + 16.0f;
            float rightX = previewPos.x + previewW - 16.0f;

            // Draw clean DSP sine/drive waveform polyline
            ImVec2 wavePts[16];
            for (int w = 0; w < 16; ++w) {
                float frac = w / 15.0f;
                float x = leftX + frac * (rightX - leftX);
                float y = midY - std::sin(frac * 6.2831853f * 2.0f) * 18.0f * (bypassed ? 0.3f : 1.0f);
                wavePts[w] = ImVec2(x, y);
            }
            ImU32 waveCol = bypassed ? static_cast<ImU32>(tokens.text.muted) :
                            (isPreviewHovered ? static_cast<ImU32>(tokens.text.accent) : static_cast<ImU32>(tokens.signal.active));
            dl->AddPolyline(wavePts, 16, waveCol, 0, 1.8f);

            const char* promptText = isWindowOpen ? "PARAMETERS OPEN" : "CLICK TO TWEAK DSP";
            ImVec2 pSz = ImGui::CalcTextSize(promptText);
            dl->AddText(ImVec2(previewPos.x + (previewW - pSz.x) * 0.5f, previewPos.y + 68.0f),
                        isPreviewHovered ? tokens.text.primary.u32 : tokens.text.secondary.u32, promptText);
        }

        // Crisp inner border stroke
        ImU32 frameBorder = isFaulted ? static_cast<ImU32>(tokens.borders.cardFaulted) :
                            (isWindowOpen ? static_cast<ImU32>(tokens.borders.cardGlowOpen) :
                            (bypassed ? static_cast<ImU32>(tokens.borders.subtle) : static_cast<ImU32>(tokens.signal.active)));
        dl->AddRect(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), frameBorder, 5.0f, 0, 1.5f);

        if (previewClicked && !isFaulted) {
            if (hasGui) {
                plugins::PluginWindowManager::instance().openPluginWindow(pluginInst);
                HWND hw = plugins::PluginWindowManager::instance().getWindow(pluginInst);
                if (hw) {
                    ThumbnailManager::instance().captureWindow(slot->name(), hw);
                    ThumbnailManager::instance().requestCapture(slot->name(), hw, 15);
                }
            } else {
                m_dspTweakSlot = (m_dspTweakSlot == slot) ? nullptr : slot;
            }
            m_expandedSlot = slot;
            m_expandPulseTimer = 1.0f;
        }
    }

    // =========================================================================
    // SECTION 3: PARAMETER READOUT HEADER (H = 16px, Y = 136px..152px)
    // Monospace Font g_fontMono guarantees jitter-free stable layout
    // =========================================================================
    {
        ImGui::SetCursorPos(ImVec2(8.0f, 136.0f));
        if (g_fontMono) ImGui::PushFont(g_fontMono);

        float mixVal = slot->dryWet() * 100.0f;
        char mixStr[32];
        std::snprintf(mixStr, sizeof(mixStr), "MIX: %3.0f%%", mixVal);
        ImGui::TextColored(tokens.text.accent.vec4, "%s", mixStr);

        ImGui::SameLine(cardWidth - 8.0f - 96.0f);
        float trimVal = slot->outputGainDb();
        char trimStr[32];
        std::snprintf(trimStr, sizeof(trimStr), "TRIM:%+5.1fdB", trimVal);
        ImGui::TextColored(tokens.text.secondary.vec4, "%s", trimStr);

        if (g_fontMono) ImGui::PopFont();
    }

    // =========================================================================
    // SECTION 4: TACTILE MIX SLIDER & MINI METER (H = 26px, Y = 156px..182px)
    // =========================================================================
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.5f));
        ImGui::SetCursorPos(ImVec2(8.0f, 156.0f));

        float mix = slot->dryWet() * 100.0f;
        char mixLabel[32];
        std::snprintf(mixLabel, sizeof(mixLabel), "##Mix_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        ImGui::SetNextItemWidth(190.0f);
        if (ResettableSliderFloat(mixLabel, &mix, 0.0f, 100.0f, 100.0f, "Mix: %.0f%%")) {
            slot->setDryWet(mix / 100.0f);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dry/Wet Mix (Double-click to reset 100%%)");

        ImGui::SameLine(0, 6.0f);
        renderMeter("SlotMtr", slot->meter().peakLeft(), 8.0f, 24.0f);
    }

    // =========================================================================
    // SECTION 5: TACTILE OUTPUT TRIM SLIDER (H = 26px, Y = 186px..212px)
    // =========================================================================
    {
        ImGui::SetCursorPos(ImVec2(8.0f, 186.0f));
        float outTrim = slot->outputGainDb();
        char trimLabel[32];
        std::snprintf(trimLabel, sizeof(trimLabel), "##Trim_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        ImGui::SetNextItemWidth(206.0f);
        if (ResettableSliderFloat(trimLabel, &outTrim, -24.0f, +12.0f, 0.0f, "Trim: %+.1f dB")) {
            slot->setOutputGainDb(outTrim);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output Trim (Double-click to reset 0.0 dB)");
        ImGui::PopStyleVar(); // FramePadding
    }

    // Context Menu for MIDI Learn & Options
    char popupId[64];
    std::snprintf(popupId, sizeof(popupId), "SlotCtx_%d_%d_%d", slotIndex, blockIndex, branchIndex);
    if (ImGui::BeginPopupContextItem(popupId)) {
        if (hasGui) {
            if (ImGui::MenuItem("Open Plugin GUI Window")) {
                plugins::PluginWindowManager::instance().openPluginWindow(pluginInst);
            }
            HWND hWin = plugins::PluginWindowManager::instance().getWindow(pluginInst);
            if (hWin && ImGui::MenuItem("Capture GUI Preview Screenshot")) {
                ThumbnailManager::instance().captureWindow(slot->name(), hWin);
            }
            ImGui::Separator();
        } else if (pluginInst) {
            if (ImGui::MenuItem("Edit DSP Parameters")) {
                m_dspTweakSlot = slot;
            }
            ImGui::Separator();
        }
        if (ImGui::MenuItem("MIDI Learn: Bypass Toggle")) {
            m_midi.startLearning(midi::BindingTargetType::SlotBypass, slotIndex, branchIndex);
        }
        if (ImGui::MenuItem("MIDI Learn: Dry/Wet Mix")) {
            m_midi.startLearning(midi::BindingTargetType::SlotDryWet, slotIndex, branchIndex);
        }
        if (ImGui::MenuItem("MIDI Learn: Output Trim")) {
            m_midi.startLearning(midi::BindingTargetType::SlotOutputGain, slotIndex, branchIndex);
        }
        if (ImGui::MenuItem("Delete Slot")) {
            if (hasGui) plugins::PluginWindowManager::instance().closePluginWindow(pluginInst);
            if (m_dspTweakSlot == slot) m_dspTweakSlot = nullptr;
            if (m_expandedSlot == slot) m_expandedSlot = nullptr;
            if (branchIndex == -1) {
                m_graph.removeSerialNode(slotIndex);
            } else {
                auto* blk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(blockIndex));
                if (blk) {
                    auto* br = blk->getBranch(branchIndex);
                    if (br) br->removeSlot(slotIndex);
                }
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(); // WindowPadding
    ImGui::PopStyleColor(); // ChildBg
    ImGui::EndGroup();
}

void RackView::renderParallelBlock(audio::ParallelSplitMergeBlock* block, int blockIndex, float centerY) {
    if (!block) return;

    ImGui::PushID(blockIndex);
    const auto& tokens = themeTokens();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    float scrollX = ImGui::GetScrollX();
    float scrollY = ImGui::GetScrollY();

    auto toScreen = [&](float lx, float ly) -> ImVec2 {
        return ImVec2(winPos.x + lx - scrollX, winPos.y + ly - scrollY);
    };

    const float cardW = 240.0f;
    const float cardH = 224.0f;
    const float slotWireW = 28.0f;
    const float insertCardW = 80.0f;
    const float envHeaderH = 36.0f;
    const float envPadX = 14.0f;
    const float envPadY = 10.0f;
    const float envH = envHeaderH + (2.0f * envPadY) + cardH; // 280.0f

    auto* br0 = block->getBranch(0);
    auto* br1 = block->getBranch(1);
    const size_t numSlots0 = br0 ? br0->numSlots() : 0;
    const size_t numSlots1 = br1 ? br1->numSlots() : 0;

    // Calculate dynamic envelope width based on number of slots in each branch
    float w0 = envPadX + (numSlots0 * (cardW + slotWireW)) + insertCardW + envPadX;
    float w1 = envPadX + (numSlots1 * (cardW + slotWireW)) + insertCardW + envPadX;
    const float envelopeW = std::max({ w0, w1, 460.0f });

    // Vertical positioning:
    // With centerY = 322.0f, card0MidY = 170.0f (centerY - 152.0f), card1MidY = 474.0f (centerY + 152.0f)
    const float card0MidY = centerY - 152.0f; // 170.0f
    const float card1MidY = centerY + 152.0f; // 474.0f

    const float card0Top = card0MidY - (cardH * 0.5f); // 58.0f
    const float card1Top = card1MidY - (cardH * 0.5f); // 362.0f

    const float envY0 = card0Top - envPadY - envHeaderH; // 12.0f
    const float envY1 = card1Top - envPadY - envHeaderH; // 316.0f

    const float startX = ImGui::GetCursorPosX();
    const float forkLeadW = 36.0f;
    const float forkStartX = startX;
    const float envX = forkStartX + forkLeadW;

    const std::string key0 = "b" + std::to_string(blockIndex) + "_0";
    const std::string key1 = "b" + std::to_string(blockIndex) + "_1";
    bool min0 = m_branchMinimized[key0];
    bool min1 = m_branchMinimized[key1];

    const float port0Y = min0 ? (envY0 + envHeaderH * 0.5f) : card0MidY;
    const float port1Y = min1 ? (envY1 + envHeaderH * 0.5f) : card1MidY;

    int pendingDissolveKeepBranch = -99;

    // 1. Split Fork Junction & Wires entering Envelope Left Ports
    dl->AddCircleFilled(toScreen(forkStartX, centerY), 4.5f, tokens.cables.core);
    drawRoutingWire(dl, toScreen(forkStartX, centerY), toScreen(envX, port0Y), false);
    drawRoutingWire(dl, toScreen(forkStartX, centerY), toScreen(envX, port1Y), false);

    // Left Input Ports on Envelopes
    dl->AddCircleFilled(toScreen(envX, port0Y), 6.0f, tokens.cables.socketRing);
    dl->AddCircleFilled(toScreen(envX, port0Y), 3.5f, tokens.signal.branchA);

    dl->AddCircleFilled(toScreen(envX, port1Y), 6.0f, tokens.cables.socketRing);
    dl->AddCircleFilled(toScreen(envX, port1Y), 3.5f, tokens.signal.branchB);

    // =========================================================================
    // 2. BRANCH A ENVELOPE (Top Chassis)
    // =========================================================================
    {
        const float chassisH0 = min0 ? envHeaderH : envH;
        // Envelope Background & Border (Cool Cyan / Navy theme)
        dl->AddRectFilled(toScreen(envX, envY0), toScreen(envX + envelopeW, envY0 + chassisH0), tokens.surfaces.branchABg, 8.0f);
        dl->AddRect(toScreen(envX, envY0), toScreen(envX + envelopeW, envY0 + chassisH0), tokens.borders.branchABorder, 8.0f, 0, 1.5f);

        // Header Background Bar (Rounded top corners, or fully rounded if minimized)
        ImDrawFlags roundFlags0 = min0 ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop;
        dl->AddRectFilled(toScreen(envX, envY0), toScreen(envX + envelopeW, envY0 + envHeaderH), tokens.surfaces.branchAHeader, 8.0f, roundFlags0);
        if (!min0) {
            dl->AddLine(toScreen(envX, envY0 + envHeaderH), toScreen(envX + envelopeW, envY0 + envHeaderH), tokens.borders.branchABorder, 1.0f);
        }

        // Header Controls
        if (br0) {
            ImGui::SetCursorPos(ImVec2(envX + 12.0f, envY0 + 6.0f));
            ImGui::BeginGroup();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.5f));

            // Branch A Badge Pill
            renderBadgePill("BRANCH A", tokens.surfaces.branchABg, tokens.borders.branchABorder, tokens.signal.branchA, 24.0f);
            ImGui::SameLine(0, 8);

            char countBuf[32];
            std::snprintf(countBuf, sizeof(countBuf), "(%zu %s)", numSlots0, numSlots0 == 1 ? "Slot" : "Slots");
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", countBuf);
            ImGui::SameLine(0, 12);

            // Mute Button
            bool m0 = br0->isMuted();
            if (m0) ImGui::PushStyleColor(ImGuiCol_Button, tokens.signal.faulted.vec4);
            if (CenteredButton("M##br0", ImVec2(24, 24))) { br0->setMuted(!m0); }
            if (m0) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(m0 ? "Unmute Branch A" : "Mute Branch A");

            ImGui::SameLine(0, 4);

            // Solo Button
            bool s0 = br0->isSolo();
            if (s0) ImGui::PushStyleColor(ImGuiCol_Button, tokens.signal.active.vec4);
            if (CenteredButton("S##br0", ImVec2(24, 24))) { br0->setSolo(!s0); }
            if (s0) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(s0 ? "Unsolo Branch A" : "Solo Branch A");

            ImGui::SameLine(0, 4);

            // Phase Invert Button
            bool p0 = br0->isPhaseInvert();
            if (p0) ImGui::PushStyleColor(ImGuiCol_Button, tokens.text.accent.vec4);
            if (CenteredButton("\xC3\x98##br0", ImVec2(24, 24))) { br0->setPhaseInvert(!p0); }
            if (p0) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Phase Invert (180° Polarity Flip)");

            ImGui::SameLine(0, 8);

            // Pan Slider (Double-click reset to 0.0)
            float pan0 = br0->pan();
            ImGui::SetNextItemWidth(74);
            if (ResettableSliderFloat("##Pan0", &pan0, -1.0f, 1.0f, 0.0f, "Pan: %+.2f")) { br0->setPan(pan0); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Branch A Pan (Double-click to center)");

            ImGui::SameLine(0, 6);

            // Gain Slider (Double-click reset to 0.0dB)
            float gain0 = br0->gainDb();
            ImGui::SetNextItemWidth(74);
            if (ResettableSliderFloat("##Gain0", &gain0, -36.0f, +12.0f, 0.0f, "%+.1f dB")) { br0->setGainDb(gain0); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Branch A Gain (Double-click to reset 0.0 dB)");

            ImGui::PopStyleVar();
            ImGui::EndGroup();

            // Top-right controls: Minimise & Delete Branch A
            float btnAreaX0 = envX + envelopeW - 58.0f;
            ImGui::SetCursorPos(ImVec2(btnAreaX0, envY0 + 6.0f));

            if (renderCenteredMinimiseButton("##minBr0", ImVec2(24, 24), min0)) {
                m_branchMinimized[key0] = !min0;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(min0 ? "Maximise Branch A" : "Minimise Branch A");

            ImGui::SameLine(0, 4);

            char delPopup0[32];
            std::snprintf(delPopup0, sizeof(delPopup0), "DelConfirmBr0_%d", blockIndex);
            if (renderCenteredDeleteButton("##delBr0", ImVec2(24, 24))) {
                ImGui::OpenPopup(delPopup0);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete Branch A (Branch B will be kept in rack)");

            if (ImGui::BeginPopup(delPopup0)) {
                ImGui::TextColored(tokens.text.error.vec4, "Delete Branch A?");
                ImGui::TextDisabled("Branch B plugins will be moved into the rack.");
                ImGui::Spacing();
                if (ImGui::Button("Delete Branch", ImVec2(110, 24))) {
                    pendingDissolveKeepBranch = 1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(60, 24))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // If not minimized, render slots and internal wiring
            if (!min0) {
                float curX0 = envX + envPadX;

                // Wire from envelope entry port to first slot/insert card
                drawRoutingWire(dl, toScreen(envX, card0MidY), toScreen(curX0, card0MidY), false);

                for (size_t s = 0; s < numSlots0; ++s) {
                    auto* bSlot = br0->getSlot(s);
                    if (!bSlot) continue;

                    ImGui::SetCursorPos(ImVec2(curX0, card0Top));
                    renderPluginSlot(bSlot, static_cast<int>(s), blockIndex, 0);
                    curX0 += cardW;

                    drawRoutingWire(dl, toScreen(curX0, card0MidY), toScreen(curX0 + slotWireW, card0MidY), false);
                    curX0 += slotWireW;
                }

                // Branch A Inline Insert Button Card
                ImGui::SetCursorPos(ImVec2(curX0, card0Top));
                ImGui::BeginGroup();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.cardBg.vec4);
                char insertBr0Card[32];
                std::snprintf(insertBr0Card, sizeof(insertBr0Card), "InsertBr0Card_%d", blockIndex);
                ImGui::BeginChild(insertBr0Card, ImVec2(insertCardW, cardH), true, ImGuiWindowFlags_NoScrollbar);
                ImGui::SetCursorPos(ImVec2((insertCardW - 48.0f) * 0.5f, (cardH - 48.0f) * 0.5f));
                if (renderCenteredPlusButton("##AddPluginBr0", ImVec2(48.0f, 48.0f), 11.0f)) {
                    m_pluginBrowserModal->open(blockIndex, 0);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Insert plugin into Branch A");
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                curX0 += insertCardW;

                // Wire from insert card to envelope exit port
                if (curX0 < envX + envelopeW) {
                    drawRoutingWire(dl, toScreen(curX0, card0MidY), toScreen(envX + envelopeW, card0MidY), false);
                }
            }
        }
    }

    // =========================================================================
    // 3. BRANCH B ENVELOPE (Bottom Chassis)
    // =========================================================================
    {
        const float chassisH1 = min1 ? envHeaderH : envH;
        // Envelope Background & Border (Warm Amber theme)
        dl->AddRectFilled(toScreen(envX, envY1), toScreen(envX + envelopeW, envY1 + chassisH1), tokens.surfaces.branchBBg, 8.0f);
        dl->AddRect(toScreen(envX, envY1), toScreen(envX + envelopeW, envY1 + chassisH1), tokens.borders.branchBBorder, 8.0f, 0, 1.5f);

        // Header Background Bar (Rounded top corners, or fully rounded if minimized)
        ImDrawFlags roundFlags1 = min1 ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop;
        dl->AddRectFilled(toScreen(envX, envY1), toScreen(envX + envelopeW, envY1 + envHeaderH), tokens.surfaces.branchBHeader, 8.0f, roundFlags1);
        if (!min1) {
            dl->AddLine(toScreen(envX, envY1 + envHeaderH), toScreen(envX + envelopeW, envY1 + envHeaderH), tokens.borders.branchBBorder, 1.0f);
        }

        // Header Controls
        if (br1) {
            ImGui::SetCursorPos(ImVec2(envX + 12.0f, envY1 + 6.0f));
            ImGui::BeginGroup();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.5f));

            // Branch B Badge Pill
            renderBadgePill("BRANCH B", tokens.surfaces.branchBBg, tokens.borders.branchBBorder, tokens.signal.branchB, 24.0f);
            ImGui::SameLine(0, 8);

            char countBuf[32];
            std::snprintf(countBuf, sizeof(countBuf), "(%zu %s)", numSlots1, numSlots1 == 1 ? "Slot" : "Slots");
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", countBuf);
            ImGui::SameLine(0, 12);

            // Mute Button
            bool m1 = br1->isMuted();
            if (m1) ImGui::PushStyleColor(ImGuiCol_Button, tokens.signal.faulted.vec4);
            if (CenteredButton("M##br1", ImVec2(24, 24))) { br1->setMuted(!m1); }
            if (m1) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(m1 ? "Unmute Branch B" : "Mute Branch B");

            ImGui::SameLine(0, 4);

            // Solo Button
            bool s1 = br1->isSolo();
            if (s1) ImGui::PushStyleColor(ImGuiCol_Button, tokens.signal.active.vec4);
            if (CenteredButton("S##br1", ImVec2(24, 24))) { br1->setSolo(!s1); }
            if (s1) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(s1 ? "Unsolo Branch B" : "Solo Branch B");

            ImGui::SameLine(0, 4);

            // Phase Invert Button
            bool p1 = br1->isPhaseInvert();
            if (p1) ImGui::PushStyleColor(ImGuiCol_Button, tokens.text.accent.vec4);
            if (CenteredButton("\xC3\x98##br1", ImVec2(24, 24))) { br1->setPhaseInvert(!p1); }
            if (p1) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Phase Invert (180° Polarity Flip)");

            ImGui::SameLine(0, 8);

            // Pan Slider (Double-click reset to 0.0)
            float pan1 = br1->pan();
            ImGui::SetNextItemWidth(74);
            if (ResettableSliderFloat("##Pan1", &pan1, -1.0f, 1.0f, 0.0f, "Pan: %+.2f")) { br1->setPan(pan1); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Branch B Pan (Double-click to center)");

            ImGui::SameLine(0, 6);

            // Gain Slider (Double-click reset to 0.0dB)
            float gain1 = br1->gainDb();
            ImGui::SetNextItemWidth(74);
            if (ResettableSliderFloat("##Gain1", &gain1, -36.0f, +12.0f, 0.0f, "%+.1f dB")) { br1->setGainDb(gain1); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Branch B Gain (Double-click to reset 0.0 dB)");

            ImGui::PopStyleVar();
            ImGui::EndGroup();

            // Top-right controls: Minimise & Delete Branch B
            float btnAreaX1 = envX + envelopeW - 58.0f;
            ImGui::SetCursorPos(ImVec2(btnAreaX1, envY1 + 6.0f));

            if (renderCenteredMinimiseButton("##minBr1", ImVec2(24, 24), min1)) {
                m_branchMinimized[key1] = !min1;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(min1 ? "Maximise Branch B" : "Minimise Branch B");

            ImGui::SameLine(0, 4);

            char delPopup1[32];
            std::snprintf(delPopup1, sizeof(delPopup1), "DelConfirmBr1_%d", blockIndex);
            if (renderCenteredDeleteButton("##delBr1", ImVec2(24, 24))) {
                ImGui::OpenPopup(delPopup1);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete Branch B (Branch A will be kept in rack)");

            if (ImGui::BeginPopup(delPopup1)) {
                ImGui::TextColored(tokens.text.error.vec4, "Delete Branch B?");
                ImGui::TextDisabled("Branch A plugins will be moved into the rack.");
                ImGui::Spacing();
                if (ImGui::Button("Delete Branch", ImVec2(110, 24))) {
                    pendingDissolveKeepBranch = 0;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(60, 24))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            // If not minimized, render slots and internal wiring
            if (!min1) {
                float curX1 = envX + envPadX;

                // Wire from envelope entry port to first slot/insert card
                drawRoutingWire(dl, toScreen(envX, card1MidY), toScreen(curX1, card1MidY), false);

                for (size_t s = 0; s < numSlots1; ++s) {
                    auto* bSlot = br1->getSlot(s);
                    if (!bSlot) continue;

                    ImGui::SetCursorPos(ImVec2(curX1, card1Top));
                    renderPluginSlot(bSlot, static_cast<int>(s), blockIndex, 1);
                    curX1 += cardW;

                    drawRoutingWire(dl, toScreen(curX1, card1MidY), toScreen(curX1 + slotWireW, card1MidY), false);
                    curX1 += slotWireW;
                }

                // Branch B Inline Insert Button Card
                ImGui::SetCursorPos(ImVec2(curX1, card1Top));
                ImGui::BeginGroup();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.cardBg.vec4);
                char insertBr1Card[32];
                std::snprintf(insertBr1Card, sizeof(insertBr1Card), "InsertBr1Card_%d", blockIndex);
                ImGui::BeginChild(insertBr1Card, ImVec2(insertCardW, cardH), true, ImGuiWindowFlags_NoScrollbar);
                ImGui::SetCursorPos(ImVec2((insertCardW - 48.0f) * 0.5f, (cardH - 48.0f) * 0.5f));
                if (renderCenteredPlusButton("##AddPluginBr1", ImVec2(48.0f, 48.0f), 11.0f)) {
                    m_pluginBrowserModal->open(blockIndex, 1);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Insert plugin into Branch B");
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                curX1 += insertCardW;

                // Wire from insert card to envelope exit port
                if (curX1 < envX + envelopeW) {
                    drawRoutingWire(dl, toScreen(curX1, card1MidY), toScreen(envX + envelopeW, card1MidY), false);
                }
            }
        }
    }

    // =========================================================================
    // 4. CONVERGENCE & COMBINER NODE
    // =========================================================================
    // Right Exit Ports on Envelopes
    dl->AddCircleFilled(toScreen(envX + envelopeW, port0Y), 6.0f, tokens.cables.socketRing);
    dl->AddCircleFilled(toScreen(envX + envelopeW, port0Y), 3.5f, tokens.signal.branchA);

    dl->AddCircleFilled(toScreen(envX + envelopeW, port1Y), 6.0f, tokens.cables.socketRing);
    dl->AddCircleFilled(toScreen(envX + envelopeW, port1Y), 3.5f, tokens.signal.branchB);

    const float convLeadW = 36.0f;
    const float combW = 104.0f;
    const float combH = 60.0f;
    const float combLeft = envX + envelopeW + convLeadW;
    const float combTop = centerY - (combH * 0.5f);

    drawRoutingWire(dl, toScreen(envX + envelopeW, port0Y), toScreen(combLeft, centerY), false);
    drawRoutingWire(dl, toScreen(envX + envelopeW, port1Y), toScreen(combLeft, centerY), false);

    // Combiner card background
    dl->AddRectFilled(toScreen(combLeft, combTop), toScreen(combLeft + combW, combTop + combH), tokens.surfaces.combinerBg, 6.0f);
    dl->AddRect(toScreen(combLeft, combTop), toScreen(combLeft + combW, combTop + combH), tokens.borders.combinerBorder, 6.0f, 0, 1.2f);

    // Combiner Ports
    dl->AddCircleFilled(toScreen(combLeft, centerY), 4.0f, tokens.cables.core);
    dl->AddCircleFilled(toScreen(combLeft + combW, centerY), 4.0f, tokens.cables.core);

    ImGui::SetCursorPos(ImVec2(combLeft + 6.0f, combTop + 6.0f));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));

    ImGui::SetCursorPosX(combLeft + (combW - ImGui::CalcTextSize("SUM / BLEND").x) * 0.5f);
    ImGui::TextColored(tokens.text.accent.vec4, "SUM / BLEND");

    float blend = block->blend();
    ImGui::SetCursorPosX(combLeft + 8.0f);
    ImGui::SetNextItemWidth(combW - 16.0f);
    if (ResettableSliderFloat("##ABBlend", &blend, -1.0f, 1.0f, 0.0f, "A:B %+.2f")) {
        block->setBlend(blend);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Branch A / B Crossfade Blend\n0.00: 50/50 Equal Sum\n-1.00: 100%% Branch A\n+1.00: 100%% Branch B\nDouble-click to center");
    }

    ImGui::PopStyleVar();
    ImGui::EndGroup();

    // Set cursor to the right edge of combiner node for downstream serial routing
    ImGui::SetCursorPosX(combLeft + combW);

    // Handle deferred branch dissolve if triggered
    if (pendingDissolveKeepBranch >= 0) {
        int branchToDelete = (pendingDissolveKeepBranch == 1) ? 0 : 1;
        auto* delBr = block->getBranch(branchToDelete);
        if (delBr) {
            for (size_t s = 0; s < delBr->numSlots(); ++s) {
                auto* slot = delBr->getSlot(s);
                if (slot) {
                    auto* pInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
                    if (pInst) plugins::PluginWindowManager::instance().closePluginWindow(pInst);
                }
            }
        }
        m_graph.dissolveParallelBlock(blockIndex, pendingDissolveKeepBranch);
    }

    ImGui::PopID();
}

bool RackView::renderRotaryKnob(const char* label, float* value, float minVal, float maxVal, float radius, const char* format, float defaultVal) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const auto& tokens = themeTokens();

    float diameter = radius * 2.0f;
    ImVec2 center(pos.x + radius + 4.0f, pos.y + radius + 4.0f);

    // Invisible button to capture drag & clicks
    ImGui::InvisibleButton(label, ImVec2(diameter + 8.0f, diameter + 8.0f));
    bool isHovered = ImGui::IsItemHovered();
    bool isActive = ImGui::IsItemActive();

    bool valueChanged = false;
    ImGuiID knobId = ImGui::GetItemID();

    if (s_suppressResetId == knobId) {
        if (!io.MouseDown[0]) {
            s_suppressResetId = 0;
        } else {
            *value = std::clamp(defaultVal, minVal, maxVal);
        }
    } else if (isActive) {
        float speed = 0.005f * (maxVal - minVal);
        float delta = -io.MouseDelta.y * speed;
        if (delta != 0.0f) {
            *value = std::clamp(*value + delta, minVal, maxVal);
            valueChanged = true;
        }
    }

    if (isHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *value = std::clamp(defaultVal, minVal, maxVal);
        s_suppressResetId = knobId;
        ImGui::ClearActiveID();
        valueChanged = true;
    }

    if (isHovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        char tip[64];
        std::snprintf(tip, sizeof(tip), format, *value);
        ImGui::SetTooltip("%s (Double-click to reset)", tip);
    }

    // Normalized value (0.0 to 1.0)
    float norm = (*value - minVal) / (maxVal - minVal);
    norm = std::clamp(norm, 0.0f, 1.0f);

    // Knob Outer Shadow / Ring
    dl->AddCircleFilled(center, radius + 2.0f, tokens.borders.shadow);
    dl->AddCircle(center, radius + 2.0f, isActive ? tokens.borders.focus.u32 : (isHovered ? tokens.borders.cardHovered.u32 : tokens.borders.subtle.u32), 0, 1.5f);

    // Dark brushed aluminum knob cap
    dl->AddCircleFilled(center, radius, tokens.surfaces.cardBg);

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
        dl->PathStroke(isActive ? tokens.text.accent.u32 : tokens.signal.active.u32, 0, 2.0f);
    }

    // Pointer notch / needle on the knob
    float needleAngle = startAngle + norm * 4.71238898f;
    ImVec2 needleInner(center.x + std::cos(needleAngle) * (radius * 0.35f), center.y + std::sin(needleAngle) * (radius * 0.35f));
    ImVec2 needleOuter(center.x + std::cos(needleAngle) * (radius - 2.0f), center.y + std::sin(needleAngle) * (radius - 2.0f));
    dl->AddLine(needleInner, needleOuter, tokens.text.primary, 2.0f);

    return valueChanged;
}

void RackView::renderBottomBar() {
    ImGui::Separator();
    const auto& tokens = themeTokens();

    // Left side: Settings button (clean name without brackets)
    ImGui::PushStyleColor(ImGuiCol_Button, tokens.surfaces.buttonBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tokens.surfaces.buttonBgHovered.vec4);
    ImGui::PushStyleColor(ImGuiCol_Text, tokens.text.primary.vec4);
    if (CenteredButton("Settings", ImVec2(80.0f, 26.0f))) {
        m_settingsModal->open();
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open Audio, Plugin, and Host Settings");
    }

    ImGui::SameLine(0, 8);
    // Practice Tools (Looper & Backing Track Player) toggle button
    bool toolsActive = m_practiceToolsModal && m_practiceToolsModal->isOpen();
    if (toolsActive) {
        ImGui::PushStyleColor(ImGuiCol_Button, tokens.surfaces.buttonBgActive.vec4);
    }
    if (CenteredButton("Practice Tools", ImVec2(105.0f, 26.0f))) {
        m_practiceToolsModal->toggle();
    }
    if (toolsActive) {
        ImGui::PopStyleColor();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open Practice Tools (Riff Looper & WAV Backing Track Player)");
    }

    ImGui::SameLine(0, 14);
    ImGui::AlignTextToFramePadding();

    // Left side: MIDI status / learn
    if (m_midi.isLearning()) {
        ImGui::TextColored(tokens.text.warning.vec4, "[*] MIDI LEARN ACTIVE: Move any knob, slider, or press a footswitch to bind...");
        ImGui::SameLine(0, 12);
        if (CenteredButton("Cancel")) {
            m_midi.cancelLearning();
        }
    } else {
        ImGui::TextDisabled("MIDI: %s", m_midi.lastActivityDescription().c_str());
    }

    // Right side: ASIO Activity & Real-Time DSP Status Pill
    const double sr = m_asio.currentSampleRate();
    const int bufSize = m_asio.currentBufferSize();
    const double latencyMs = (sr > 0.0) ? (static_cast<double>(bufSize) / sr * 1000.0) : 0.0;
    float dspLoad = m_dspLoadPercent ? m_dspLoadPercent->load(std::memory_order_relaxed) : 0.0f;
    uint32_t drops = m_dspDropouts ? m_dspDropouts->load(std::memory_order_relaxed) : 0;

    char statusText[160];
    if (m_asio.isRunning()) {
        if (drops > 0) {
            std::snprintf(statusText, sizeof(statusText), "ACTIVE  |  %.0f kHz  |  %d spls (%.1f ms)  |  DSP: %.0f%%  |  DROPS: %u",
                          sr / 1000.0, bufSize, latencyMs, dspLoad, drops);
        } else {
            std::snprintf(statusText, sizeof(statusText), "ACTIVE  |  %.0f kHz  |  %d spls (%.1f ms)  |  DSP: %.0f%%",
                          sr / 1000.0, bufSize, latencyMs, dspLoad);
        }
    } else {
        std::snprintf(statusText, sizeof(statusText), "STOPPED");
    }

    const float pillW = ImGui::CalcTextSize(statusText).x + 36.0f;
    const float availW = ImGui::GetContentRegionAvail().x;
    const float rightMargin = 14.0f;

    if (availW > pillW + rightMargin) {
        ImGui::SameLine(ImGui::GetCursorPosX() + availW - pillW - rightMargin);
    } else {
        ImGui::SameLine(0, 12);
    }

    renderStatusPill(m_asio.isRunning(), statusText);
}

void RackView::renderMeter(const char* label, float level, float width, float height) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();

    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), tokens.surfaces.frameBg, 2.0f);

    float db = audio::DspUtils::gainToDb(level);
    float norm = (db + 60.0f) / 60.0f;
    norm = std::clamp(norm, 0.0f, 1.0f);

    float fillHeight = height * norm;
    ImU32 meterColor = (db > -0.5f) ? tokens.signal.meterClip.u32 :
                       (db > -6.0f) ? tokens.signal.meterWarning.u32 :
                                      tokens.signal.meterNormal.u32;

    drawList->AddRectFilled(ImVec2(pos.x, pos.y + height - fillHeight),
                            ImVec2(pos.x + width, pos.y + height),
                            meterColor, 2.0f);

    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), tokens.borders.subtle, 2.0f);
    ImGui::Dummy(ImVec2(width, height));
}









void RackView::renderDspTweakModal() {
    if (!m_dspTweakSlot) return;

    auto* pInst = dynamic_cast<plugins::IPluginInstance*>(m_dspTweakSlot->innerNode());
    if (!pInst) {
        m_dspTweakSlot = nullptr;
        return;
    }

    const size_t numParams = pInst->numParameters();
    float winHeight = std::max(220.0f, 130.0f + static_cast<float>(numParams) * 36.0f);
    ImGui::SetNextWindowSize(ImVec2(390, winHeight), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    char titleBuf[64];
    std::snprintf(titleBuf, sizeof(titleBuf), "%s Parameters##DspTweak", m_dspTweakSlot->name().c_str());

    const auto& tokens = themeTokens();
    if (ImGui::Begin(titleBuf, &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(tokens.text.accent.vec4, "%s", m_dspTweakSlot->name().c_str());
        ImGui::TextDisabled("Vendor: %s | Version: %s", pInst->vendor().c_str(), pInst->version().c_str());
        ImGui::Separator();
        ImGui::Spacing();

        if (numParams == 0) {
            ImGui::TextDisabled("No modifiable parameters available.");
        } else {
            float availW = ImGui::GetContentRegionAvail().x;
            for (size_t i = 0; i < numParams; ++i) {
                auto desc = pInst->getParameterDesc(i);
                float val = pInst->getParameterValue(desc.id);

                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", desc.name.c_str());
                ImGui::SameLine(95.0f);
                ImGui::SetNextItemWidth(availW - 95.0f);

                char label[64];
                std::snprintf(label, sizeof(label), "##p_%zu", i);
                if (ResettableSliderFloat(label, &val, desc.minValue, desc.maxValue, desc.defaultValue, "%.2f")) {
                    pInst->setParameterValue(desc.id, val);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Double-click to reset (default: %.2f)", desc.defaultValue);
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (CenteredButton("Close", ImVec2(80, 24))) {
            open = false;
        }
    }
    ImGui::End();

    if (!open) {
        m_dspTweakSlot = nullptr;
    }
}

void RackView::renderUpdateModal() {
    if (!m_showUpdateModal) return;
    const auto& tokens = themeTokens();

    ImGui::OpenPopup("Praccy Updates##Modal");
    ImGui::SetNextWindowSize(ImVec2(520, 390), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Praccy Updates##Modal", &m_showUpdateModal, ImGuiWindowFlags_NoResize)) {
        ImGui::TextColored(tokens.text.accent.vec4, "PRACCY UPDATE MANAGER");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Installed Version: %s", "v1.1.3");
        ImGui::Spacing();

        // Option to enable beta builds from dev branch
        state::AppConfig cfg;
        cfg.load();
        bool includeBeta = cfg.checkBetaUpdates;
        if (ImGui::Checkbox("Enable beta builds from 'dev' branch on GitHub", &includeBeta)) {
            cfg.checkBetaUpdates = includeBeta;
            cfg.save();
            UpdateChecker::instance().checkForUpdates(includeBeta);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Check for cutting-edge development commits directly from origin/dev on GitHub");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        auto info = UpdateChecker::instance().getInfo();

        if (info.status == UpdateStatus::Checking) {
            ImGui::TextColored(tokens.text.accent.vec4, "Checking GitHub repository for %s updates...", includeBeta ? "beta" : "stable");
            ImGui::Spacing();
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 8));
        } else if (info.status == UpdateStatus::UpToDate) {
            ImGui::TextColored(tokens.text.success.vec4, "[v] You are up to date!");
            ImGui::TextDisabled("Channel: %s", includeBeta ? "Beta (dev branch)" : "Stable (Releases)");
            ImGui::Text("Latest version: %s", info.latestVersion.c_str());
        } else if (info.status == UpdateStatus::UpdateAvailable) {
            ImGui::TextColored(tokens.text.warning.vec4, "[*] New %s build available!", includeBeta ? "Beta" : "Release");
            ImGui::Text("Latest: %s", info.latestVersion.c_str());
            if (!info.releaseTitle.empty()) {
                ImGui::TextColored(tokens.text.primary.vec4, "%s", info.releaseTitle.c_str());
            }
            if (!info.publishedDate.empty()) {
                ImGui::TextDisabled("Date: %s", info.publishedDate.c_str());
            }

            ImGui::Spacing();
            if (!info.assetUrl.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Button, ColorToken(tokens.signal.active.r, tokens.signal.active.g, tokens.signal.active.b, 0.45f).vec4);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorToken(tokens.signal.active.r, tokens.signal.active.g, tokens.signal.active.b, 0.65f).vec4);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ColorToken(tokens.signal.active.r, tokens.signal.active.g, tokens.signal.active.b, 0.85f).vec4);
                if (CenteredButton("Download & Apply Update", ImVec2(210, 30))) {
                    UpdateChecker::instance().startDownload();
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0, 10);
            }
            if (CenteredButton("Open in GitHub ->", ImVec2(150, 30))) {
                ShellExecuteA(nullptr, "open", info.downloadUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            if (info.assetUrl.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("Note: Direct binary asset not found for this commit; click 'Open in GitHub' to view.");
            }
        } else if (info.status == UpdateStatus::Downloading) {
            ImGui::TextColored(tokens.text.accent.vec4, "Downloading update package...");
            if (!info.assetName.empty()) {
                ImGui::TextDisabled("File: %s", info.assetName.c_str());
            }

            char progBuf[64];
            if (info.totalBytes > 0) {
                float dlMB = static_cast<float>(info.downloadedBytes) / (1024.0f * 1024.0f);
                float totMB = static_cast<float>(info.totalBytes) / (1024.0f * 1024.0f);
                std::snprintf(progBuf, sizeof(progBuf), "%.1f / %.1f MB (%.0f%%)", dlMB, totMB, info.downloadProgress * 100.0f);
            } else if (info.downloadedBytes > 0) {
                float dlMB = static_cast<float>(info.downloadedBytes) / (1024.0f * 1024.0f);
                std::snprintf(progBuf, sizeof(progBuf), "%.1f MB downloaded", dlMB);
            } else {
                std::snprintf(progBuf, sizeof(progBuf), "Connecting...");
            }

            ImGui::Spacing();
            ImGui::ProgressBar(info.downloadProgress, ImVec2(-1, 22), progBuf);
            ImGui::Spacing();

            if (info.downloadProgress >= 1.0f) {
                ImGui::TextColored(tokens.text.warning.vec4, "Extracting and verifying package files...");
            } else {
                ImGui::TextDisabled("Please wait while Praccy streams the update package...");
            }
        } else if (info.status == UpdateStatus::ReadyToInstall) {
            ImGui::TextColored(tokens.text.success.vec4, "[v] Update downloaded and ready to apply!");
            ImGui::TextWrapped("Praccy will now close, copy the updated files into place, and restart automatically.");
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Button, ColorToken(tokens.signal.active.r, tokens.signal.active.g, tokens.signal.active.b, 0.45f).vec4);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ColorToken(tokens.signal.active.r, tokens.signal.active.g, tokens.signal.active.b, 0.65f).vec4);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ColorToken(tokens.signal.active.r, tokens.signal.active.g, tokens.signal.active.b, 0.85f).vec4);
            if (CenteredButton("Restart & Apply Update Now", ImVec2(230, 32))) {
                UpdateChecker::instance().applyUpdateAndRestart();
            }
            ImGui::PopStyleColor(3);
        } else if (info.status == UpdateStatus::Error) {
            ImGui::TextColored(tokens.text.error.vec4, "[!] Update check / download failed");
            ImGui::TextWrapped("%s", info.errorMessage.c_str());
            if (!info.downloadUrl.empty()) {
                ImGui::Spacing();
                if (CenteredButton("Open GitHub in Browser ->", ImVec2(200, 28))) {
                    ShellExecuteA(nullptr, "open", info.downloadUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        }

        float bottomY = std::max(ImGui::GetCursorPosY() + 12.0f, 335.0f);
        ImGui::SetCursorPosY(bottomY);
        ImGui::Separator();

        bool isDownloading = (info.status == UpdateStatus::Downloading);
        if (!isDownloading) {
            if (CenteredButton("Check Again", ImVec2(110, 26))) {
                UpdateChecker::instance().checkForUpdates(includeBeta);
            }
            ImGui::SameLine();
        }
        if (CenteredButton("Close", ImVec2(90, 26))) {
            m_showUpdateModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}





void RackView::triggerHudToast(std::string message) {
    m_hudToastText = std::move(message);
    m_hudToastTimer = kHudToastDuration;
}

void RackView::renderFloatingHudToast() {
    if (m_hudToastTimer <= 0.0f || m_hudToastText.empty()) return;

    m_hudToastTimer -= ImGui::GetIO().DeltaTime;
    if (m_hudToastTimer < 0.0f) m_hudToastTimer = 0.0f;

    const float alpha = computeHudToastAlpha(m_hudToastTimer, kHudToastDuration);
    if (alpha <= 0.001f) return;

    const auto& tokens = themeTokens();
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    const ImVec2 textSize = ImGui::CalcTextSize(m_hudToastText.c_str());
    const float padX = 24.0f;
    const float padY = 10.0f;
    const float boxW = textSize.x + padX * 2.0f + 20.0f;
    const float boxH = textSize.y + padY * 2.0f;

    const float posX = vp->Pos.x + (vp->Size.x - boxW) * 0.5f;
    const float posY = vp->Pos.y + 44.0f;

    ImGui::SetNextWindowPos(ImVec2(posX, posY));
    ImGui::SetNextWindowSize(ImVec2(boxW, boxH));
    ImGui::SetNextWindowBgAlpha(0.0f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoInputs |
                                  ImGuiWindowFlags_NoDecoration |
                                  ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoFocusOnAppearing |
                                  ImGuiWindowFlags_NoNav |
                                  ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##FloatingHudToastOverlay", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const ImU32 bgCol = ColorToken(
            static_cast<uint8_t>(tokens.surfaces.cardBg.r * 255.0f),
            static_cast<uint8_t>(tokens.surfaces.cardBg.g * 255.0f),
            static_cast<uint8_t>(tokens.surfaces.cardBg.b * 255.0f),
            static_cast<uint8_t>(std::clamp(tokens.surfaces.cardBg.a * alpha * 0.95f * 255.0f, 0.0f, 255.0f))
        ).u32;
        const ImU32 borderCol = ColorToken(
            static_cast<uint8_t>(tokens.borders.focus.r * 255.0f),
            static_cast<uint8_t>(tokens.borders.focus.g * 255.0f),
            static_cast<uint8_t>(tokens.borders.focus.b * 255.0f),
            static_cast<uint8_t>(std::clamp(tokens.borders.focus.a * alpha * 0.90f * 255.0f, 0.0f, 255.0f))
        ).u32;
        const ImU32 textCol = ColorToken(
            static_cast<uint8_t>(tokens.text.primary.r * 255.0f),
            static_cast<uint8_t>(tokens.text.primary.g * 255.0f),
            static_cast<uint8_t>(tokens.text.primary.b * 255.0f),
            static_cast<uint8_t>(std::clamp(alpha * 255.0f, 0.0f, 255.0f))
        ).u32;
        const ImU32 ledCol = ColorToken(
            static_cast<uint8_t>(tokens.signal.active.r * 255.0f),
            static_cast<uint8_t>(tokens.signal.active.g * 255.0f),
            static_cast<uint8_t>(tokens.signal.active.b * 255.0f),
            static_cast<uint8_t>(std::clamp(alpha * 255.0f, 0.0f, 255.0f))
        ).u32;

        dl->AddRectFilled(ImVec2(posX, posY), ImVec2(posX + boxW, posY + boxH), bgCol, 8.0f);
        dl->AddRect(ImVec2(posX, posY), ImVec2(posX + boxW, posY + boxH), borderCol, 8.0f, 0, 1.5f);

        const ImVec2 dotCenter(posX + 16.0f, posY + boxH * 0.5f);
        dl->AddCircleFilled(dotCenter, 4.0f, ledCol, 16);

        const ImVec2 textPos(posX + 28.0f, posY + (boxH - textSize.y) * 0.5f);
        dl->AddText(textPos, textCol, m_hudToastText.c_str());
    }
    ImGui::End();
}

} // namespace praccy::ui

