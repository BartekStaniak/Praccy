#include "rack_view.h"
#include "thumbnail_manager.h"
#include "update_checker.h"
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

// State tracking for double-click resets to suppress subsequent mouse-drag overrides
static ImGuiID s_suppressResetId = 0;

static bool ResettableSliderFloat(const char* label, float* v, float v_min, float v_max, float default_val, const char* format = "%.3f", ImGuiSliderFlags flags = 0) {
    ImGuiID id = ImGui::GetID(label);
    if (!ImGui::GetIO().MouseDown[0] && s_suppressResetId == id) {
        s_suppressResetId = 0;
    }

    bool changed = ImGui::SliderFloat(label, v, v_min, v_max, format, flags);

    if (s_suppressResetId == id) {
        if (*v != default_val) {
            *v = default_val;
            changed = true;
        }
        ImGui::ClearActiveID();
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *v = default_val;
        s_suppressResetId = id;
        ImGui::ClearActiveID();
        changed = true;
    }

    return changed;
}

static bool ResettableVSliderFloat(const char* label, const ImVec2& size, float* v, float v_min, float v_max, float default_val, const char* format = "", ImGuiSliderFlags flags = 0) {
    ImGuiID id = ImGui::GetID(label);
    if (!ImGui::GetIO().MouseDown[0] && s_suppressResetId == id) {
        s_suppressResetId = 0;
    }

    bool changed = ImGui::VSliderFloat(label, size, v, v_min, v_max, format, flags);

    if (s_suppressResetId == id) {
        if (*v != default_val) {
            *v = default_val;
            changed = true;
        }
        ImGui::ClearActiveID();
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *v = default_val;
        s_suppressResetId = id;
        ImGui::ClearActiveID();
        changed = true;
    }

    return changed;
}

static bool ResettableDragFloat(const char* label, float* v, float v_speed, float v_min, float v_max, float default_val, const char* format = "%.0f", ImGuiSliderFlags flags = 0) {
    ImGuiID id = ImGui::GetID(label);
    if (!ImGui::GetIO().MouseDown[0] && s_suppressResetId == id) {
        s_suppressResetId = 0;
    }

    bool changed = ImGui::DragFloat(label, v, v_speed, v_min, v_max, format, flags);

    if (s_suppressResetId == id) {
        if (*v != default_val) {
            *v = default_val;
            changed = true;
        }
        ImGui::ClearActiveID();
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *v = default_val;
        s_suppressResetId = id;
        ImGui::ClearActiveID();
        changed = true;
    }

    return changed;
}

static bool CenteredButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    bool popPad = false;
    if (size.y > 0.0f) {
        float fontH = ImGui::GetFontSize();
        float padY = std::max(0.0f, (size.y - fontH) * 0.5f);
        float padX = ImGui::GetStyle().FramePadding.x;
        if (size.x > 0.0f) {
            float textW = ImGui::CalcTextSize(label).x;
            if (size.x < textW + 2.0f * padX) {
                padX = std::max(0.0f, (size.x - textW) * 0.5f);
            }
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));
        popPad = true;
    }
    bool clicked = ImGui::Button(label, size);
    if (popPad) ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    return clicked;
}

static bool renderCenteredStarButton(const char* id, const ImVec2& size = ImVec2(24, 22), bool isFavorite = false) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();

    if (held) {
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(40, 48, 62, 220), 4.0f);
    } else if (hovered) {
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), IM_COL32(35, 42, 54, 180), 4.0f);
    }

    float cx = std::floor(pos.x + size.x * 0.5f);
    float cy = std::floor(pos.y + size.y * 0.5f);

    const float rOuter = 6.8f;
    const float rInner = 2.9f;
    const float pi = 3.1415926535f;

    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        float angle = -pi * 0.5f + (i * pi / 5.0f);
        float r = (i % 2 == 0) ? rOuter : rInner;
        pts[i] = ImVec2(cx + std::cos(angle) * r, cy + std::sin(angle) * r);
    }

    if (isFavorite) {
        ImU32 fillCol = held ? IM_COL32(230, 175, 35, 255) : (hovered ? IM_COL32(255, 220, 75, 255) : IM_COL32(255, 195, 45, 255));
        dl->AddConvexPolyFilled(pts, 10, fillCol);
        dl->AddPolyline(pts, 10, IM_COL32(210, 145, 20, 255), ImDrawFlags_Closed, 1.0f);
    } else {
        ImU32 strokeCol = held ? IM_COL32(180, 190, 210, 255) : (hovered ? IM_COL32(220, 225, 240, 255) : IM_COL32(110, 118, 135, 200));
        dl->AddPolyline(pts, 10, strokeCol, ImDrawFlags_Closed, 1.4f);
    }

    return clicked;
}

static bool renderCenteredSplitButton(const char* id, const ImVec2& size = ImVec2(24, 20)) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bgCol;
    if (held)         bgCol = IM_COL32(35, 65, 105, 255);
    else if (hovered) bgCol = IM_COL32(50, 85, 135, 255);
    else              bgCol = IM_COL32(40, 60, 95, 220);

    ImU32 borderCol = hovered ? IM_COL32(80, 130, 195, 255) : IM_COL32(55, 80, 120, 200);
    ImU32 iconCol   = hovered ? IM_COL32(245, 250, 255, 255) : IM_COL32(200, 215, 235, 255);

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 4.0f);

    float cx = std::floor(pos.x + size.x * 0.5f);
    float cy = std::floor(pos.y + size.y * 0.5f);

    const float barHalfW = 1.0f;
    const float barHalfH = 4.5f;
    const float offset = 2.5f;

    dl->AddRectFilled(ImVec2(cx - offset - barHalfW, cy - barHalfH), ImVec2(cx - offset + barHalfW, cy + barHalfH), iconCol, 0.5f);
    dl->AddRectFilled(ImVec2(cx + offset - barHalfW, cy - barHalfH), ImVec2(cx + offset + barHalfW, cy + barHalfH), iconCol, 0.5f);

    return clicked;
}

static bool renderCenteredDeleteButton(const char* id, const ImVec2& size = ImVec2(22, 20)) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bgCol;
    if (held)         bgCol = IM_COL32(165, 45, 45, 255);
    else if (hovered) bgCol = IM_COL32(185, 55, 55, 255);
    else              bgCol = IM_COL32(135, 45, 45, 220);

    ImU32 borderCol = hovered ? IM_COL32(230, 90, 90, 255) : IM_COL32(170, 60, 60, 200);
    ImU32 iconCol   = hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(245, 215, 215, 255);

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 4.0f);

    float cx = std::floor(pos.x + size.x * 0.5f);
    float cy = std::floor(pos.y + size.y * 0.5f);

    const float arm = std::min(4.0f, (size.y - 6.0f) * 0.5f);
    dl->AddLine(ImVec2(cx - arm, cy - arm), ImVec2(cx + arm, cy + arm), iconCol, 1.8f);
    dl->AddLine(ImVec2(cx - arm, cy + arm), ImVec2(cx + arm, cy - arm), iconCol, 1.8f);

    return clicked;
}

static bool renderCenteredPauseButton(const char* id, const ImVec2& size = ImVec2(22, 20), bool isBypassed = false) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bgCol;
    if (held)         bgCol = IM_COL32(35, 65, 105, 255);
    else if (hovered) bgCol = IM_COL32(50, 85, 135, 255);
    else              bgCol = isBypassed ? IM_COL32(50, 54, 65, 220) : IM_COL32(35, 75, 45, 220);

    ImU32 borderCol = hovered ? IM_COL32(80, 130, 195, 255) : (isBypassed ? IM_COL32(70, 75, 90, 200) : IM_COL32(45, 120, 60, 200));
    ImU32 iconCol   = isBypassed ? IM_COL32(140, 145, 160, 255) : (hovered ? IM_COL32(245, 250, 255, 255) : IM_COL32(120, 250, 150, 255));

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 4.0f);

    float cx = std::floor(pos.x + size.x * 0.5f);
    float cy = std::floor(pos.y + size.y * 0.5f);

    const float barHalfW = 1.2f;
    const float barHalfH = 4.5f;
    const float offset = 2.8f;

    dl->AddRectFilled(ImVec2(cx - offset - barHalfW, cy - barHalfH), ImVec2(cx - offset + barHalfW, cy + barHalfH), iconCol, 1.0f);
    dl->AddRectFilled(ImVec2(cx + offset - barHalfW, cy - barHalfH), ImVec2(cx + offset + barHalfW, cy + barHalfH), iconCol, 1.0f);

    return clicked;
}

static void drawRoutingWire(ImDrawList* dl, ImVec2 p1, ImVec2 p2, bool withArrow = false) {
    // Outer glow
    dl->AddLine(p1, p2, IM_COL32(40, 95, 160, 90), 4.5f);
    // Core wire
    dl->AddLine(p1, p2, IM_COL32(110, 175, 255, 230), 2.0f);

    if (withArrow) {
        float dx = p2.x - p1.x;
        float dy = p2.y - p1.y;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len > 6.0f) {
            float nx = dx / len;
            float ny = dy / len;
            float px = -ny;
            float py = nx;

            ImVec2 tip = p2;
            const float arrowSize = 6.5f;
            const float arrowW = 4.5f;
            ImVec2 left(tip.x - nx * arrowSize + px * arrowW, tip.y - ny * arrowSize + py * arrowW);
            ImVec2 right(tip.x - nx * arrowSize - px * arrowW, tip.y - ny * arrowSize - py * arrowW);

            dl->AddTriangleFilled(left, tip, right, IM_COL32(160, 215, 255, 255));
        }
    }
}

static bool renderCenteredPlusButton(const char* id, const ImVec2& size = ImVec2(52, 52), float iconRadius = 11.0f) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 bgCol;
    if (held)         bgCol = IM_COL32(30, 52, 85, 255);
    else if (hovered) bgCol = IM_COL32(42, 68, 110, 255);
    else              bgCol = IM_COL32(32, 44, 65, 220);

    ImU32 borderCol = hovered ? IM_COL32(80, 140, 230, 255) : IM_COL32(52, 70, 100, 180);
    ImU32 iconCol   = hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(185, 205, 235, 220);

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
    ThumbnailManager::instance().update();
    if (m_expandPulseTimer > 0.0f) {
        m_expandPulseTimer -= ImGui::GetIO().DeltaTime * 2.8f;
        if (m_expandPulseTimer < 0.0f) m_expandPulseTimer = 0.0f;
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

        if (m_showPluginBrowser) {
            renderPluginBrowserModal();
        }

        renderDspTweakModal();
        renderUpdateModal();
        renderSettingsModal();
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void RackView::renderPraccyLogo() {
    Thumbnail* logoThumb = ThumbnailManager::instance().getThumbnail("__praccy_logo__");
    if (!logoThumb || !logoThumb->srv) {
        const char* potentialPaths[] = {
            "resources/pick_logo_cropped.png",
            "../resources/pick_logo_cropped.png",
            "../../resources/pick_logo_cropped.png",
            "resources/logo.png",
            "../resources/logo.png"
        };
        for (const char* p : potentialPaths) {
            if (std::filesystem::exists(p)) {
                ThumbnailManager::instance().loadFromFile("__praccy_logo__", p);
                logoThumb = ThumbnailManager::instance().getThumbnail("__praccy_logo__");
                if (logoThumb && logoThumb->srv) break;
            }
        }
    }

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const float boxW = 145.0f;
    const float boxH = 74.0f;

    // Stylized logo card
    dl->AddRectFilled(pos, ImVec2(pos.x + boxW, pos.y + boxH), IM_COL32(20, 22, 28, 220), 6.0f);
    dl->AddRect(pos, ImVec2(pos.x + boxW, pos.y + boxH), IM_COL32(45, 52, 68, 200), 6.0f);

    if (logoThumb && logoThumb->srv) {
        // Draw the actual guitar pick logo
        const float pickH = 54.0f;
        const float aspect = (logoThumb->height > 0) ? (static_cast<float>(logoThumb->width) / static_cast<float>(logoThumb->height)) : 0.868f;
        const float pickW = pickH * aspect;
        const float pickX = pos.x + 10.0f;
        const float pickY = pos.y + (boxH - pickH) * 0.5f;

        dl->AddImage((ImTextureID)logoThumb->srv, ImVec2(pickX, pickY), ImVec2(pickX + pickW, pickY + pickH));

        float textX = pickX + pickW + 10.0f;
        // Text: PRACCY (vertically centered alongside the pick logo)
        dl->AddText(ImVec2(textX, pos.y + (boxH * 0.5f) - 15.0f), IM_COL32(245, 195, 120, 255), "PRACCY");
        // Version tag: v1.0.2
        dl->AddText(ImVec2(textX, pos.y + (boxH * 0.5f) + 3.0f), IM_COL32(140, 150, 170, 220), "v1.0.2");
    } else {
        // Fallback procedural medallion
        const float cx = pos.x + 24.0f;
        const float cy = pos.y + (boxH * 0.5f);

        dl->AddCircleFilled(ImVec2(cx, cy), 16.0f, IM_COL32(250, 150, 40, 35));
        dl->AddCircleFilled(ImVec2(cx, cy), 12.0f, IM_COL32(30, 34, 46, 255));
        dl->AddCircle(ImVec2(cx, cy), 12.0f, IM_COL32(250, 155, 45, 230), 0, 1.5f);

        dl->AddLine(ImVec2(cx - 5.0f, cy - 4.0f), ImVec2(cx - 5.0f, cy + 4.0f), IM_COL32(255, 180, 70, 255), 1.5f);
        dl->AddLine(ImVec2(cx - 1.5f, cy - 8.0f), ImVec2(cx - 1.5f, cy + 8.0f), IM_COL32(255, 205, 90, 255), 2.0f);
        dl->AddLine(ImVec2(cx + 2.0f, cy - 6.0f), ImVec2(cx + 2.0f, cy + 6.0f), IM_COL32(255, 180, 70, 255), 1.8f);
        dl->AddLine(ImVec2(cx + 5.5f, cy - 3.0f), ImVec2(cx + 5.5f, cy + 3.0f), IM_COL32(255, 160, 50, 255), 1.5f);

        dl->AddText(ImVec2(pos.x + 46.0f, pos.y + (boxH * 0.5f) - 15.0f), IM_COL32(255, 160, 45, 255), "PRACCY");
        dl->AddText(ImVec2(pos.x + 46.0f, pos.y + (boxH * 0.5f) + 3.0f), IM_COL32(130, 140, 160, 200), "v1.0.2");
    }

    ImGui::Dummy(ImVec2(boxW, boxH));
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
    ImGui::BeginChild("PracticeRibbon", ImVec2(0, 88.0f), false, ImGuiWindowFlags_NoScrollbar);

    // Responsive 5-column layout: Praccy Logo on far-left, followed by Tuner, Metro, Gate, Master
    if (ImGui::BeginTable("PracticeRibbonTable", 5, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("LogoCol",   ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn("TunerCol",  ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("MetroCol",  ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("GateCol",   ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("MasterCol", ImGuiTableColumnFlags_WidthStretch, 0.22f);

        const float modH = 74.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.17f, 0.22f, 0.85f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));

        // ----------------------------------------------------
        // Column 0: Praccy Logo
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        renderPraccyLogo();

        // ----------------------------------------------------
        // Column 1: Strobe Tuner Module
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::BeginChild("Mod_Tuner", ImVec2(0, modH), true, ImGuiWindowFlags_NoScrollbar);
        {
            auto result = m_tuner.currentResult();
            ImGui::TextColored(ImVec4(0.70f, 0.74f, 0.82f, 1.0f), "TUNER");
            ImGui::SameLine();
            if (result.confidence) {
                bool inTune = std::abs(result.centDeviation) <= 3.0f;
                ImVec4 noteColor = inTune ? ImVec4(0.25f, 0.95f, 0.40f, 1.0f) : ImVec4(0.98f, 0.82f, 0.25f, 1.0f);
                ImGui::TextColored(noteColor, "%s (%.1f Hz)", result.noteName.c_str(), result.frequencyHz);
            } else {
                ImGui::TextDisabled("(Listening)");
            }

            ImGui::SetCursorPosY(38.0f);
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const float gaugeW = std::max(60.0f, ImGui::GetContentRegionAvail().x - 4.0f);
            const float gaugeH = 22.0f;
            ImDrawList* dl = ImGui::GetWindowDrawList();

            dl->AddRectFilled(pos, ImVec2(pos.x + gaugeW, pos.y + gaugeH), IM_COL32(22, 24, 30, 255), 4.0f);
            dl->AddRect(pos, ImVec2(pos.x + gaugeW, pos.y + gaugeH), IM_COL32(45, 50, 62, 255), 4.0f);

            const float midX = pos.x + (gaugeW * 0.5f);
            dl->AddLine(ImVec2(midX, pos.y + 2), ImVec2(midX, pos.y + gaugeH - 2), IM_COL32(40, 200, 80, 220), 2.0f);
            dl->AddLine(ImVec2(pos.x + gaugeW * 0.25f, pos.y + 4), ImVec2(pos.x + gaugeW * 0.25f, pos.y + gaugeH - 4), IM_COL32(65, 70, 85, 200), 1.0f);
            dl->AddLine(ImVec2(pos.x + gaugeW * 0.75f, pos.y + 4), ImVec2(pos.x + gaugeW * 0.75f, pos.y + gaugeH - 4), IM_COL32(65, 70, 85, 200), 1.0f);

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
        ImGui::EndChild();

        // ----------------------------------------------------
        // Column 2: Metronome Module
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::BeginChild("Mod_Metro", ImVec2(0, modH), true, ImGuiWindowFlags_NoScrollbar);
        {
            ImGui::TextColored(ImVec4(0.70f, 0.74f, 0.82f, 1.0f), "METRONOME");
            ImGui::SameLine();
            bool metroPlaying = m_metronome.isPlaying();

            // Beat LEDs beside title
            {
                const int totalBeats = m_metronome.beatsPerBar();
                const int currentBeat = m_metronome.currentBeat();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 curPos = ImGui::GetCursorScreenPos();
                const float dotRadius = 4.0f;
                const float dotSpacing = 11.0f;

                for (int b = 0; b < totalBeats; ++b) {
                    float cx = curPos.x + 8.0f + (b * dotSpacing);
                    float cy = curPos.y + 8.0f;
                    bool active = metroPlaying && (b == currentBeat);
                    if (active) {
                        ImU32 haloColor = (b == 0) ? IM_COL32(255, 60, 60, 80) : IM_COL32(250, 160, 30, 80);
                        dl->AddCircleFilled(ImVec2(cx, cy), dotRadius + 2.5f, haloColor);
                        ImU32 ledColor = (b == 0) ? IM_COL32(255, 70, 70, 255) : IM_COL32(255, 180, 40, 255);
                        dl->AddCircleFilled(ImVec2(cx, cy), dotRadius, ledColor);
                    } else {
                        dl->AddCircleFilled(ImVec2(cx, cy), dotRadius, IM_COL32(36, 40, 48, 255));
                        dl->AddCircle(ImVec2(cx, cy), dotRadius, IM_COL32(55, 60, 72, 255), 0, 1.0f);
                    }
                }
            }

            ImGui::SetCursorPosY(38.0f);
            if (CenteredButton(metroPlaying ? "STOP##M" : "PLAY##M", ImVec2(50, 24))) {
                m_metronome.setPlaying(!metroPlaying);
            }
            ImGui::SameLine(0, 6);

            float bpm = m_metronome.bpm();
            ImGui::SetNextItemWidth(76);
            if (ResettableDragFloat("##BPM", &bpm, 1.0f, 40.0f, 260.0f, 120.0f, "%.0f BPM")) {
                m_metronome.setBpm(bpm);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tempo (Double-click to reset 120 BPM)");

            ImGui::SameLine(0, 6);
            const char* timeSigOptions[] = { "4/4", "3/4", "2/4", "6/8" };
            const int timeSigBeats[] = { 4, 3, 2, 6 };
            int currentSigIdx = 0;
            int currentBeats = m_metronome.beatsPerBar();
            for (int i = 0; i < 4; ++i) {
                if (timeSigBeats[i] == currentBeats) { currentSigIdx = i; break; }
            }
            ImGui::SetNextItemWidth(54);
            if (ImGui::Combo("##TimeSig", &currentSigIdx, timeSigOptions, 4)) {
                m_metronome.setBeatsPerBar(timeSigBeats[currentSigIdx]);
            }
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

            ImGui::TextColored(ImVec4(0.70f, 0.74f, 0.82f, 1.0f), "NOISE GATE");
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

            ImGui::TextColored(ImVec4(0.70f, 0.74f, 0.82f, 1.0f), "MASTER OUTPUT");

            char valStr[32];
            std::snprintf(valStr, sizeof(valStr), "%+.1f dB", masterVol);
            float valTextW = ImGui::CalcTextSize(valStr).x;
            if (availMasterW > valTextW + 110.0f) {
                ImGui::SameLine(availMasterW - valTextW - 4.0f);
            } else {
                ImGui::SameLine();
            }
            ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, 1.0f), "%s", valStr);

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

        if (CenteredButton(sc->name.c_str())) {
            m_scenes.applyScene(static_cast<int>(s), m_graph);
            m_sceneFeedbackMsg = "Loaded " + sc->name;
            m_sceneFeedbackTimer = 2.5f;
        }

        if (isActive) {
            ImGui::PopStyleColor(2);
        }
        ImGui::SameLine(0, 8);
    }

    if (CenteredButton("Save Scene")) {
        m_scenes.captureCurrentScene(m_scenes.activeSceneIndex(), m_graph);
        const auto* cur = m_scenes.getScene(m_scenes.activeSceneIndex());
        m_sceneFeedbackMsg = "Saved to " + (cur ? cur->name : "Scene");
        m_sceneFeedbackTimer = 2.5f;
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
                    m_sceneFeedbackMsg = "Loaded preset: " + pName;
                    m_sceneFeedbackTimer = 2.5f;
                }
            }
            ImGui::EndCombo();
        }
    }

    if (m_sceneFeedbackTimer > 0.0f) {
        m_sceneFeedbackTimer -= ImGui::GetIO().DeltaTime;
        ImGui::SameLine(0, 12);

        const ImVec2 textSize = ImGui::CalcTextSize(m_sceneFeedbackMsg.c_str());
        const float pillW = textSize.x + 28.0f;
        const float pillH = 22.0f;
        const float frameH = ImGui::GetFrameHeight();
        const float offsetY = std::max(0.0f, (frameH - pillH) * 0.5f);

        const ImVec2 screenPos = ImGui::GetCursorScreenPos();
        const ImVec2 pos(screenPos.x, screenPos.y + offsetY);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background capsule pill
        dl->AddRectFilled(pos, ImVec2(pos.x + pillW, pos.y + pillH), IM_COL32(20, 36, 26, 240), 11.0f);
        dl->AddRect(pos, ImVec2(pos.x + pillW, pos.y + pillH), IM_COL32(45, 120, 60, 255), 11.0f);

        // Centered glowing LED dot
        ImVec2 dotCenter(pos.x + 10.0f, pos.y + (pillH * 0.5f));
        dl->AddCircleFilled(dotCenter, 5.0f, IM_COL32(40, 240, 80, 70));
        dl->AddCircleFilled(dotCenter, 3.0f, IM_COL32(50, 255, 90, 255));

        // Centered text
        ImVec2 textPos(pos.x + 19.0f, pos.y + ((pillH - textSize.y) * 0.5f));
        dl->AddText(textPos, IM_COL32(180, 245, 200, 255), m_sceneFeedbackMsg.c_str());

        ImGui::Dummy(ImVec2(pillW, frameH));
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
                    m_sceneFeedbackMsg = std::string("Preset '") + m_presetNameBuffer + "' saved!";
                    m_sceneFeedbackTimer = 2.5f;
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
    ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.80f, 1.0f), "SIGNAL CHAIN (RACK):");

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

    float centerY = 120.0f;
    float serialCardY = 8.0f;
    float rackTotalH = 240.0f;

    if (hasParallel) {
        centerY = 322.0f;
        serialCardY = centerY - (cardH * 0.5f); // 210.0f
        rackTotalH = 615.0f;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    float scrollX = ImGui::GetScrollX();
    float scrollY = ImGui::GetScrollY();

    auto toScreen = [&](float lx, float ly) -> ImVec2 {
        return ImVec2(winPos.x + lx - scrollX, winPos.y + ly - scrollY);
    };

    float currentX = 12.0f;

    // 1. Input Node Card
    ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
    renderInputCard(serialCardY);
    currentX += 180.0f;

    // 2. Render Graph Nodes
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = m_graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            drawRoutingWire(dl, toScreen(currentX, centerY), toScreen(currentX + wireW, centerY), true);
            currentX += wireW;

            ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
            renderPluginSlot(slot, static_cast<int>(i), -1, -1);
            currentX += cardW;
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            drawRoutingWire(dl, toScreen(currentX, centerY), toScreen(currentX + 24.0f, centerY), true);
            currentX += 24.0f;

            ImGui::SetCursorPos(ImVec2(currentX, 0.0f));
            renderParallelBlock(block, static_cast<int>(i), centerY);
            currentX = ImGui::GetCursorPosX();
        }
    }

    // 3. Serial Rack Insertion Slot Card (+ Add Plugin)
    drawRoutingWire(dl, toScreen(currentX, centerY), toScreen(currentX + wireW, centerY), true);
    currentX += wireW;

    const float insertCardW = 80.0f;
    ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.15f, 0.19f, 0.6f));
    ImGui::BeginChild("InsertSerialCard", ImVec2(insertCardW, cardH), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2((insertCardW - 48.0f) * 0.5f, (cardH - 48.0f) * 0.5f));
    if (renderCenteredPlusButton("##AddPluginSerial", ImVec2(48.0f, 48.0f), 11.0f)) {
        m_insertTargetBlockIndex = -1;
        m_insertTargetBranchIndex = -1;
        m_showPluginBrowser = true;
        m_focusPluginBrowser = true;
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
    drawRoutingWire(dl, toScreen(currentX, centerY), toScreen(currentX + wireW, centerY), true);
    currentX += wireW;

    ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
    ImGui::BeginChild("OutputDestNode", ImVec2(100.0f, cardH), true);
    ImGui::TextColored(ImVec4(0.35f, 0.80f, 1.0f, 1.0f), "[ OUTPUT ]");
    ImGui::TextDisabled("To ASIO");
    ImGui::Spacing();
    ImGui::Spacing();
    renderMeter("FinalL", m_graph.outputMeter().peakLeft(), 14, 120);
    ImGui::SameLine(0, 6);
    renderMeter("FinalR", m_graph.outputMeter().peakRight(), 14, 120);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    currentX += 100.0f;

    // Ensure scroll area bounds
    ImGui::SetCursorPos(ImVec2(currentX + 30.0f, rackTotalH));
    ImGui::Dummy(ImVec2(0, 0));

    ImGui::EndChild();
}

void RackView::renderSignalCable(float width) {
    ImGui::SameLine(0, 0);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float cardH = 224.0f;
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

void RackView::renderInputCard(float cardY) {
    if (cardY >= 0.0f) {
        ImGui::SetCursorPosY(cardY);
    }
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.18f, 0.22f, 1.0f));
    ImGui::BeginChild("InputGainNode", ImVec2(180, 224), true);
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
    const bool bypassed = slot->isBypassed();
    ImVec4 cardBg = bypassed ? ImVec4(0.12f, 0.13f, 0.16f, 0.95f) : ImVec4(0.16f, 0.18f, 0.23f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg);
    char childId[64];
    std::snprintf(childId, sizeof(childId), "SlotCard_%d_%d_%d", slotIndex, blockIndex, branchIndex);

    const float cardWidth = 240.0f;
    const float cardHeight = 224.0f;
    ImGui::BeginChild(childId, ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar);

    auto* pluginInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    bool isWindowOpen = pluginInst ? plugins::PluginWindowManager::instance().isWindowOpen(pluginInst) : false;

    // ----------------------------------------------------
    // Row 1: Header (Plugin Name + [|| Split] + [|| Pause] + [X] Delete)
    // ----------------------------------------------------
    {
        std::string displayName = slot->name();
        if (displayName.length() > 14) {
            displayName = displayName.substr(0, 13) + "..";
        }

        ImGui::TextColored(bypassed ? ImVec4(0.55f, 0.58f, 0.65f, 1.0f) : ImVec4(0.98f, 0.98f, 1.0f, 1.0f),
                           "%s", displayName.c_str());

        if (branchIndex == -1) {
            // Serial slot: Split + Bypass (Pause) + Delete
            ImGui::SameLine(cardWidth - 82.0f);

            char splitBtnId[32];
            std::snprintf(splitBtnId, sizeof(splitBtnId), "##Sp_%d", slotIndex);
            if (renderCenteredSplitButton(splitBtnId, ImVec2(22, 20))) {
                m_graph.splitSerialNodeIntoParallel(slotIndex);
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                return;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split into Parallel A/B Branches");

            ImGui::SameLine(0, 5);
            char pauseBtnId[32];
            std::snprintf(pauseBtnId, sizeof(pauseBtnId), "##Ps_%d", slotIndex);
            if (renderCenteredPauseButton(pauseBtnId, ImVec2(22, 20), bypassed)) {
                slot->setBypassed(!bypassed);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(bypassed ? "Unpause / Enable Slot" : "Pause / Bypass Slot");

            ImGui::SameLine(0, 5);
            char delBtnId[32];
            std::snprintf(delBtnId, sizeof(delBtnId), "##Del_%d", slotIndex);
            if (renderCenteredDeleteButton(delBtnId, ImVec2(22, 20))) {
                if (pluginInst) {
                    plugins::PluginWindowManager::instance().closePluginWindow(pluginInst);
                }
                m_graph.removeSerialNode(slotIndex);
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                return;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete Plugin Slot");
        } else {
            // Branch slot: Bypass (Pause) + Delete
            ImGui::SameLine(cardWidth - 54.0f);

            char pauseBtnId[32];
            std::snprintf(pauseBtnId, sizeof(pauseBtnId), "##Ps_%d_%d_%d", slotIndex, blockIndex, branchIndex);
            if (renderCenteredPauseButton(pauseBtnId, ImVec2(22, 20), bypassed)) {
                slot->setBypassed(!bypassed);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(bypassed ? "Unpause / Enable Slot" : "Pause / Bypass Slot");

            ImGui::SameLine(0, 5);
            char delBtnId[32];
            std::snprintf(delBtnId, sizeof(delBtnId), "##Del_%d_%d_%d", slotIndex, blockIndex, branchIndex);
            if (renderCenteredDeleteButton(delBtnId, ImVec2(22, 20))) {
                if (pluginInst) {
                    plugins::PluginWindowManager::instance().closePluginWindow(pluginInst);
                }
                auto* blk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(blockIndex));
                if (blk) {
                    auto* br = blk->getBranch(branchIndex);
                    if (br) br->removeSlot(slotIndex);
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                return;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete Slot from Branch");
        }
    }

    ImGui::Spacing();

    // ----------------------------------------------------
    // Row 2: Plugin GUI Screenshot / Preview Screen
    // ----------------------------------------------------
    {
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
        const float previewW = cardWidth - 16.0f; // 224.0f
        const float previewH = 104.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        Thumbnail* thumb = ThumbnailManager::instance().getThumbnail(slot->name());

        // Frame background & border
        ImU32 frameBg = bypassed ? IM_COL32(16, 18, 24, 255) : IM_COL32(20, 24, 32, 255);
        ImU32 frameBorder = isWindowOpen ? IM_COL32(40, 215, 95, 255) : IM_COL32(48, 54, 70, 255);
        float borderWidth = isWindowOpen ? 1.5f : 1.0f;

        dl->AddRectFilled(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), frameBg, 5.0f);

        // Invisible button to capture click and hover over the entire preview area
        ImGui::SetCursorScreenPos(previewPos);
        char previewBtnId[64];
        std::snprintf(previewBtnId, sizeof(previewBtnId), "##GuiPreview_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        bool previewClicked = ImGui::InvisibleButton(previewBtnId, ImVec2(previewW, previewH));
        bool isPreviewHovered = ImGui::IsItemHovered();

        if (isPreviewHovered) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip(pluginInst ? "Click to Open / Focus %s GUI" : "Click to Open DSP Parameters", slot->name().c_str());
            frameBorder = isWindowOpen ? IM_COL32(70, 240, 130, 255) : IM_COL32(75, 145, 235, 255);
            borderWidth = 1.5f;
        }

        // Draw preview content:
        if (thumb && thumb->srv) {
            // Calculate aspect ratio fit within previewW x previewH with 2px margin
            float maxW = previewW - 4.0f;
            float maxH = previewH - 4.0f;
            float aspect = (thumb->height > 0) ? (static_cast<float>(thumb->width) / static_cast<float>(thumb->height)) : 1.0f;

            float drawW = maxW;
            float drawH = maxW / aspect;
            if (drawH > maxH) {
                drawH = maxH;
                drawW = maxH * aspect;
            }

            ImVec2 imgMin(
                std::floor(previewPos.x + (previewW - drawW) * 0.5f),
                std::floor(previewPos.y + (previewH - drawH) * 0.5f)
            );
            ImVec2 imgMax(imgMin.x + drawW, imgMin.y + drawH);

            ImU32 tintCol = bypassed ? IM_COL32(160, 160, 175, 180) : IM_COL32(255, 255, 255, 255);
            dl->AddImageRounded((ImTextureID)thumb->srv, imgMin, imgMax, ImVec2(0, 0), ImVec2(1, 1), tintCol, 4.0f);

            // Sleek format badge on top-left of image
            std::string formatTag = "[DSP]";
            ImU32 tagColor = IM_COL32(245, 170, 45, 230);
            if (pluginInst) {
                if (dynamic_cast<plugins::Vst3PluginInstance*>(pluginInst)) {
                    formatTag = "VST3";
                    tagColor = IM_COL32(65, 185, 255, 230);
                } else {
                    formatTag = "CLAP";
                    tagColor = IM_COL32(220, 110, 240, 230);
                }
            }
            ImVec2 badgePos(previewPos.x + 6.0f, previewPos.y + 6.0f);
            ImVec2 badgeSz = ImGui::CalcTextSize(formatTag.c_str());
            dl->AddRectFilled(ImVec2(badgePos.x - 2.0f, badgePos.y - 1.0f),
                              ImVec2(badgePos.x + badgeSz.x + 4.0f, badgePos.y + badgeSz.y + 1.0f),
                              IM_COL32(15, 18, 25, 200), 3.0f);
            dl->AddText(badgePos, tagColor, formatTag.c_str());

            // If window is open, draw an active "● OPEN" badge on top-right
            if (isWindowOpen) {
                const char* openTag = "● OPEN";
                ImVec2 oSz = ImGui::CalcTextSize(openTag);
                ImVec2 oPos(previewPos.x + previewW - oSz.x - 8.0f, previewPos.y + 6.0f);
                dl->AddRectFilled(ImVec2(oPos.x - 3.0f, oPos.y - 1.0f),
                                  ImVec2(oPos.x + oSz.x + 3.0f, oPos.y + oSz.y + 1.0f),
                                  IM_COL32(18, 50, 28, 220), 3.0f);
                dl->AddText(oPos, IM_COL32(50, 240, 110, 255), openTag);
            }

            // If hovered, draw subtle translucent glass overlay + "[ EXPAND GUI ↗ ]" prompt
            if (isPreviewHovered) {
                dl->AddRectFilled(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH),
                                  IM_COL32(15, 20, 30, 90), 5.0f);
                const char* expandPrompt = isWindowOpen ? "FOCUS WINDOW ↗" : "EXPAND GUI ↗";
                ImVec2 pSz = ImGui::CalcTextSize(expandPrompt);
                ImVec2 pPos(previewPos.x + (previewW - pSz.x) * 0.5f, previewPos.y + (previewH - pSz.y) * 0.5f);
                dl->AddRectFilled(ImVec2(pPos.x - 8.0f, pPos.y - 3.0f),
                                  ImVec2(pPos.x + pSz.x + 8.0f, pPos.y + pSz.y + 3.0f),
                                  IM_COL32(18, 24, 38, 230), 4.0f);
                dl->AddRect(ImVec2(pPos.x - 8.0f, pPos.y - 3.0f),
                            ImVec2(pPos.x + pSz.x + 8.0f, pPos.y + pSz.y + 3.0f),
                            IM_COL32(80, 140, 220, 200), 4.0f);
                dl->AddText(pPos, IM_COL32(230, 240, 255, 255), expandPrompt);
            }
        } else {
            // Procedural Faceplate Fallback (when no screenshot has been captured yet)
            // Rivets at corners
            dl->AddCircleFilled(ImVec2(previewPos.x + 6.0f, previewPos.y + 6.0f), 2.0f, IM_COL32(65, 70, 85, 255));
            dl->AddCircleFilled(ImVec2(previewPos.x + previewW - 6.0f, previewPos.y + 6.0f), 2.0f, IM_COL32(65, 70, 85, 255));

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
            dl->AddText(ImVec2(previewPos.x + 60.0f, previewPos.y + 6.0f), IM_COL32(130, 138, 155, 255), vendorText.c_str());

            // Center: Screen / Camera Icon & Click to Open
            float midX = previewPos.x + previewW * 0.5f;
            float midY = previewPos.y + 48.0f;

            // Draw vector monitor icon
            dl->AddRect(ImVec2(midX - 14.0f, midY - 12.0f), ImVec2(midX + 14.0f, midY + 8.0f),
                        isPreviewHovered ? IM_COL32(100, 180, 255, 255) : IM_COL32(100, 115, 140, 255), 2.0f, 0, 1.5f);
            dl->AddLine(ImVec2(midX, midY + 8.0f), ImVec2(midX, midY + 13.0f),
                        isPreviewHovered ? IM_COL32(100, 180, 255, 255) : IM_COL32(100, 115, 140, 255), 1.5f);
            dl->AddLine(ImVec2(midX - 6.0f, midY + 13.0f), ImVec2(midX + 6.0f, midY + 13.0f),
                        isPreviewHovered ? IM_COL32(100, 180, 255, 255) : IM_COL32(100, 115, 140, 255), 1.5f);

            const char* promptText = pluginInst ? "CLICK TO OPEN GUI" : "CLICK TO EDIT DSP";
            ImVec2 pSz = ImGui::CalcTextSize(promptText);
            dl->AddText(ImVec2(midX - pSz.x * 0.5f, previewPos.y + 70.0f),
                        isPreviewHovered ? IM_COL32(230, 240, 255, 255) : IM_COL32(150, 160, 180, 255),
                        promptText);

            if (pluginInst) {
                const char* subPrompt = "(Auto-captures preview)";
                ImVec2 sSz = ImGui::CalcTextSize(subPrompt);
                dl->AddText(ImVec2(midX - sSz.x * 0.5f, previewPos.y + 86.0f),
                            IM_COL32(110, 120, 140, 220), subPrompt);
            }
        }

        // Outer border
        dl->AddRect(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), frameBorder, 5.0f, 0, borderWidth);

        // Click-to-expand handling & expansion animation
        if (previewClicked) {
            if (pluginInst) {
                plugins::PluginWindowManager::instance().openPluginWindow(pluginInst);
                HWND hw = plugins::PluginWindowManager::instance().getWindow(pluginInst);
                if (hw) {
                    ThumbnailManager::instance().captureWindow(slot->name(), hw);
                    ThumbnailManager::instance().requestCapture(slot->name(), hw, 15);
                }
            } else {
                m_dspTweakSlot = slot;
            }
            m_expandedSlotIndex = slotIndex;
            m_expandPulseTimer = 1.0f;
        }

        // If window is currently open, schedule auto-capture or periodic refresh
        if (isWindowOpen && pluginInst) {
            HWND hw = plugins::PluginWindowManager::instance().getWindow(pluginInst);
            if (hw) {
                ThumbnailManager::instance().requestCapture(slot->name(), hw, thumb ? 120 : 15);
            }
        }

        // Expanding pulse wave animation from center
        if (m_expandedSlotIndex == slotIndex && m_expandPulseTimer > 0.0f) {
            float progress = 1.0f - m_expandPulseTimer; // 0.0 -> 1.0
            float radius = 15.0f + progress * (previewW * 0.7f);
            int alpha = static_cast<int>((1.0f - progress) * 220);
            ImVec2 center(previewPos.x + previewW * 0.5f, previewPos.y + previewH * 0.5f);
            dl->AddCircle(center, radius, IM_COL32(60, 210, 255, alpha), 36, 2.5f);
            dl->AddCircle(center, radius * 0.7f, IM_COL32(120, 240, 180, alpha / 2), 36, 1.5f);
        }

        // Return cursor below the preview screen
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
        std::snprintf(btnId, sizeof(btnId), "%s##Btn_%d_%d_%d", active ? "ACTIVE" : "BYPASS", slotIndex, blockIndex, branchIndex);
        if (CenteredButton(btnId, ImVec2(66, 24))) {
            slot->setBypassed(active);
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine(0, 8);

        // Dry / Wet Slider
        float mix = slot->dryWet() * 100.0f;
        char mixLabel[32];
        std::snprintf(mixLabel, sizeof(mixLabel), "##Mix_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        ImGui::SetNextItemWidth(96);
        if (ResettableSliderFloat(mixLabel, &mix, 0.0f, 100.0f, 100.0f, "Mix: %.0f%%")) {
            slot->setDryWet(mix / 100.0f);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dry/Wet Mix (Double-click to reset 100%%)");
        ImGui::SameLine(0, 6);
        renderMeter("SlotMtr", slot->meter().peakLeft(), 8, 22);
    }

    // ----------------------------------------------------
    // Row 4: Output Trim Slider
    // ----------------------------------------------------
    {
        float outTrim = slot->outputGainDb();
        char trimLabel[32];
        std::snprintf(trimLabel, sizeof(trimLabel), "##Trim_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        ImGui::SetNextItemWidth(cardWidth - 20.0f);
        if (ResettableSliderFloat(trimLabel, &outTrim, -24.0f, +12.0f, 0.0f, "Trim: %+.1f dB")) {
            slot->setOutputGainDb(outTrim);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output Trim (Double-click to reset 0.0 dB)");
    }

    // Context Menu for MIDI Learn & GUI Options
    char popupId[64];
    std::snprintf(popupId, sizeof(popupId), "SlotCtx_%d_%d_%d", slotIndex, blockIndex, branchIndex);
    if (ImGui::BeginPopupContextItem(popupId)) {
        if (pluginInst) {
            if (ImGui::MenuItem("Open Plugin GUI Window")) {
                plugins::PluginWindowManager::instance().openPluginWindow(pluginInst);
            }
            HWND hWin = plugins::PluginWindowManager::instance().getWindow(pluginInst);
            if (hWin && ImGui::MenuItem("Capture GUI Preview Screenshot")) {
                ThumbnailManager::instance().captureWindow(slot->name(), hWin);
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
            if (pluginInst) plugins::PluginWindowManager::instance().closePluginWindow(pluginInst);
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
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

void RackView::renderParallelBlock(audio::ParallelSplitMergeBlock* block, int blockIndex, float centerY) {
    if (!block) return;

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

    // 1. Split Fork Junction & Wires entering Envelope Left Ports
    dl->AddCircleFilled(toScreen(forkStartX, centerY), 4.5f, IM_COL32(110, 175, 255, 255));
    drawRoutingWire(dl, toScreen(forkStartX, centerY), toScreen(envX, card0MidY), true);
    drawRoutingWire(dl, toScreen(forkStartX, centerY), toScreen(envX, card1MidY), true);

    // Left Input Ports on Envelopes
    dl->AddCircleFilled(toScreen(envX, card0MidY), 6.0f, IM_COL32(18, 24, 34, 255));
    dl->AddCircleFilled(toScreen(envX, card0MidY), 3.5f, IM_COL32(80, 185, 255, 255));

    dl->AddCircleFilled(toScreen(envX, card1MidY), 6.0f, IM_COL32(32, 22, 18, 255));
    dl->AddCircleFilled(toScreen(envX, card1MidY), 3.5f, IM_COL32(255, 170, 50, 255));

    // =========================================================================
    // 2. BRANCH A ENVELOPE (Top Chassis)
    // =========================================================================
    {
        // Envelope Background & Border (Cool Cyan / Navy theme)
        dl->AddRectFilled(toScreen(envX, envY0), toScreen(envX + envelopeW, envY0 + envH), IM_COL32(18, 22, 30, 235), 8.0f);
        dl->AddRect(toScreen(envX, envY0), toScreen(envX + envelopeW, envY0 + envH), IM_COL32(45, 80, 120, 220), 8.0f, 0, 1.5f);

        // Header Background Bar (Rounded top corners)
        dl->AddRectFilled(toScreen(envX, envY0), toScreen(envX + envelopeW, envY0 + envHeaderH), IM_COL32(23, 29, 40, 255), 8.0f, ImDrawFlags_RoundCornersTop);
        dl->AddLine(toScreen(envX, envY0 + envHeaderH), toScreen(envX + envelopeW, envY0 + envHeaderH), IM_COL32(45, 80, 120, 180), 1.0f);

        // Header Controls
        if (br0) {
            ImGui::SetCursorPos(ImVec2(envX + 12.0f, envY0 + 6.0f));
            ImGui::BeginGroup();

            // Branch A Badge
            ImVec2 badgePos = ImGui::GetCursorScreenPos();
            const char* badgeText = "BRANCH A";
            ImVec2 badgeSz = ImGui::CalcTextSize(badgeText);
            dl->AddRectFilled(ImVec2(badgePos.x - 3.0f, badgePos.y - 2.0f),
                              ImVec2(badgePos.x + badgeSz.x + 5.0f, badgePos.y + badgeSz.y + 2.0f),
                              IM_COL32(18, 48, 72, 255), 4.0f);
            dl->AddRect(ImVec2(badgePos.x - 3.0f, badgePos.y - 2.0f),
                        ImVec2(badgePos.x + badgeSz.x + 5.0f, badgePos.y + badgeSz.y + 2.0f),
                        IM_COL32(50, 145, 215, 255), 4.0f);
            dl->AddText(badgePos, IM_COL32(100, 215, 255, 255), badgeText);

            ImGui::Dummy(ImVec2(badgeSz.x + 6.0f, 22.0f));
            ImGui::SameLine(0, 8);

            char countBuf[32];
            std::snprintf(countBuf, sizeof(countBuf), "(%zu %s)", numSlots0, numSlots0 == 1 ? "Slot" : "Slots");
            ImGui::TextDisabled("%s", countBuf);
            ImGui::SameLine(0, 14);

            // Mute Button
            bool m0 = br0->isMuted();
            if (m0) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
            if (CenteredButton("M##br0", ImVec2(24, 24))) { br0->setMuted(!m0); }
            if (m0) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(m0 ? "Unmute Branch A" : "Mute Branch A");

            ImGui::SameLine(0, 4);

            // Solo Button
            bool s0 = br0->isSolo();
            if (s0) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.70f, 0.10f, 1.0f));
            if (CenteredButton("S##br0", ImVec2(24, 24))) { br0->setSolo(!s0); }
            if (s0) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(s0 ? "Unsolo Branch A" : "Solo Branch A");

            ImGui::SameLine(0, 4);

            // Phase Invert Button
            bool p0 = br0->isPhaseInvert();
            if (p0) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.45f, 0.15f, 1.0f));
            if (CenteredButton("\xC3\x98##br0", ImVec2(24, 24))) { br0->setPhaseInvert(!p0); }
            if (p0) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Phase Invert (180° Polarity Flip)");

            ImGui::SameLine(0, 10);

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

            ImGui::EndGroup();

            // Branch A Slots & Internal Wiring
            float curX0 = envX + envPadX;

            // Wire from envelope entry port to first slot/insert card
            drawRoutingWire(dl, toScreen(envX, card0MidY), toScreen(curX0, card0MidY), false);

            for (size_t s = 0; s < numSlots0; ++s) {
                auto* bSlot = br0->getSlot(s);
                if (!bSlot) continue;

                ImGui::SetCursorPos(ImVec2(curX0, card0Top));
                renderPluginSlot(bSlot, static_cast<int>(s), blockIndex, 0);
                curX0 += cardW;

                drawRoutingWire(dl, toScreen(curX0, card0MidY), toScreen(curX0 + slotWireW, card0MidY), true);
                curX0 += slotWireW;
            }

            // Branch A Inline Insert Button Card
            ImGui::SetCursorPos(ImVec2(curX0, card0Top));
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.15f, 0.19f, 0.6f));
            ImGui::BeginChild("InsertBr0Card", ImVec2(insertCardW, cardH), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::SetCursorPos(ImVec2((insertCardW - 48.0f) * 0.5f, (cardH - 48.0f) * 0.5f));
            if (renderCenteredPlusButton("##AddPluginBr0", ImVec2(48.0f, 48.0f), 11.0f)) {
                m_insertTargetBlockIndex = blockIndex;
                m_insertTargetBranchIndex = 0;
                m_showPluginBrowser = true;
                m_focusPluginBrowser = true;
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

    // =========================================================================
    // 3. BRANCH B ENVELOPE (Bottom Chassis)
    // =========================================================================
    {
        // Envelope Background & Border (Warm Amber theme)
        dl->AddRectFilled(toScreen(envX, envY1), toScreen(envX + envelopeW, envY1 + envH), IM_COL32(26, 21, 19, 235), 8.0f);
        dl->AddRect(toScreen(envX, envY1), toScreen(envX + envelopeW, envY1 + envH), IM_COL32(130, 75, 40, 220), 8.0f, 0, 1.5f);

        // Header Background Bar (Rounded top corners)
        dl->AddRectFilled(toScreen(envX, envY1), toScreen(envX + envelopeW, envY1 + envHeaderH), IM_COL32(35, 27, 23, 255), 8.0f, ImDrawFlags_RoundCornersTop);
        dl->AddLine(toScreen(envX, envY1 + envHeaderH), toScreen(envX + envelopeW, envY1 + envHeaderH), IM_COL32(130, 75, 40, 180), 1.0f);

        // Header Controls
        if (br1) {
            ImGui::SetCursorPos(ImVec2(envX + 12.0f, envY1 + 6.0f));
            ImGui::BeginGroup();

            // Branch B Badge
            ImVec2 badgePos = ImGui::GetCursorScreenPos();
            const char* badgeText = "BRANCH B";
            ImVec2 badgeSz = ImGui::CalcTextSize(badgeText);
            dl->AddRectFilled(ImVec2(badgePos.x - 3.0f, badgePos.y - 2.0f),
                              ImVec2(badgePos.x + badgeSz.x + 5.0f, badgePos.y + badgeSz.y + 2.0f),
                              IM_COL32(65, 38, 20, 255), 4.0f);
            dl->AddRect(ImVec2(badgePos.x - 3.0f, badgePos.y - 2.0f),
                        ImVec2(badgePos.x + badgeSz.x + 5.0f, badgePos.y + badgeSz.y + 2.0f),
                        IM_COL32(215, 120, 45, 255), 4.0f);
            dl->AddText(badgePos, IM_COL32(255, 185, 75, 255), badgeText);

            ImGui::Dummy(ImVec2(badgeSz.x + 6.0f, 22.0f));
            ImGui::SameLine(0, 8);

            char countBuf[32];
            std::snprintf(countBuf, sizeof(countBuf), "(%zu %s)", numSlots1, numSlots1 == 1 ? "Slot" : "Slots");
            ImGui::TextDisabled("%s", countBuf);
            ImGui::SameLine(0, 14);

            // Mute Button
            bool m1 = br1->isMuted();
            if (m1) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.20f, 0.20f, 1.0f));
            if (CenteredButton("M##br1", ImVec2(24, 24))) { br1->setMuted(!m1); }
            if (m1) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(m1 ? "Unmute Branch B" : "Mute Branch B");

            ImGui::SameLine(0, 4);

            // Solo Button
            bool s1 = br1->isSolo();
            if (s1) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.70f, 0.10f, 1.0f));
            if (CenteredButton("S##br1", ImVec2(24, 24))) { br1->setSolo(!s1); }
            if (s1) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(s1 ? "Unsolo Branch B" : "Solo Branch B");

            ImGui::SameLine(0, 4);

            // Phase Invert Button
            bool p1 = br1->isPhaseInvert();
            if (p1) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.90f, 0.45f, 0.15f, 1.0f));
            if (CenteredButton("\xC3\x98##br1", ImVec2(24, 24))) { br1->setPhaseInvert(!p1); }
            if (p1) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Phase Invert (180° Polarity Flip)");

            ImGui::SameLine(0, 10);

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

            ImGui::EndGroup();

            // Branch B Slots & Internal Wiring
            float curX1 = envX + envPadX;

            // Wire from envelope entry port to first slot/insert card
            drawRoutingWire(dl, toScreen(envX, card1MidY), toScreen(curX1, card1MidY), false);

            for (size_t s = 0; s < numSlots1; ++s) {
                auto* bSlot = br1->getSlot(s);
                if (!bSlot) continue;

                ImGui::SetCursorPos(ImVec2(curX1, card1Top));
                renderPluginSlot(bSlot, static_cast<int>(s), blockIndex, 1);
                curX1 += cardW;

                drawRoutingWire(dl, toScreen(curX1, card1MidY), toScreen(curX1 + slotWireW, card1MidY), true);
                curX1 += slotWireW;
            }

            // Branch B Inline Insert Button Card
            ImGui::SetCursorPos(ImVec2(curX1, card1Top));
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.15f, 0.19f, 0.6f));
            ImGui::BeginChild("InsertBr1Card", ImVec2(insertCardW, cardH), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::SetCursorPos(ImVec2((insertCardW - 48.0f) * 0.5f, (cardH - 48.0f) * 0.5f));
            if (renderCenteredPlusButton("##AddPluginBr1", ImVec2(48.0f, 48.0f), 11.0f)) {
                m_insertTargetBlockIndex = blockIndex;
                m_insertTargetBranchIndex = 1;
                m_showPluginBrowser = true;
                m_focusPluginBrowser = true;
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

    // =========================================================================
    // 4. CONVERGENCE & COMBINER NODE
    // =========================================================================
    // Right Exit Ports on Envelopes
    dl->AddCircleFilled(toScreen(envX + envelopeW, card0MidY), 6.0f, IM_COL32(18, 24, 34, 255));
    dl->AddCircleFilled(toScreen(envX + envelopeW, card0MidY), 3.5f, IM_COL32(80, 185, 255, 255));

    dl->AddCircleFilled(toScreen(envX + envelopeW, card1MidY), 6.0f, IM_COL32(32, 22, 18, 255));
    dl->AddCircleFilled(toScreen(envX + envelopeW, card1MidY), 3.5f, IM_COL32(255, 170, 50, 255));

    const float convLeadW = 36.0f;
    const float combinerX = envX + envelopeW + convLeadW;

    drawRoutingWire(dl, toScreen(envX + envelopeW, card0MidY), toScreen(combinerX, centerY), true);
    drawRoutingWire(dl, toScreen(envX + envelopeW, card1MidY), toScreen(combinerX, centerY), true);

    // Combiner node [ + ]
    const float combSize = 44.0f;
    ImGui::SetCursorPos(ImVec2(combinerX - (combSize * 0.5f), centerY - (combSize * 0.5f)));

    char combBtnId[32];
    std::snprintf(combBtnId, sizeof(combBtnId), "##Combiner_%d", blockIndex);
    if (renderCenteredPlusButton(combBtnId, ImVec2(combSize, combSize), 10.0f)) {
        ImGui::OpenPopup("CombinerPopup");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Combiner Node (Sums Branch A + B to Stereo)\nClick for options or to remove split");
    }

    if (ImGui::BeginPopup("CombinerPopup")) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1.0f), "Parallel Split / Combiner Options");
        ImGui::Separator();
        if (ImGui::MenuItem("Remove Parallel Split (Dissolve Branches)")) {
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
            ImGui::EndPopup();
            return;
        }
        ImGui::EndPopup();
    }

    // Set cursor to the right edge of combiner node for downstream serial routing
    ImGui::SetCursorPosX(combinerX + (combSize * 0.5f));
}

bool RackView::renderRotaryKnob(const char* label, float* value, float minVal, float maxVal, float radius, const char* format, float defaultVal) {
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

    // Left side: Settings button (clean name without brackets)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.22f, 0.28f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.32f, 0.42f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.95f, 1.0f));
    if (CenteredButton("Settings", ImVec2(80.0f, 26.0f))) {
        m_showSettingsModal = true;
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open Audio, Plugin, and Host Settings");
    }

    ImGui::SameLine(0, 14);
    ImGui::AlignTextToFramePadding();

    // Left side: MIDI status / learn
    if (m_midi.isLearning()) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "[*] MIDI LEARN ACTIVE: Move any knob, slider, or press a footswitch to bind...");
        ImGui::SameLine(0, 12);
        if (CenteredButton("Cancel")) {
            m_midi.cancelLearning();
        }
    } else {
        ImGui::TextDisabled("MIDI: %s", m_midi.lastActivityDescription().c_str());
    }

    // Right side: ASIO Activity Pill
    const double sr = m_asio.currentSampleRate();
    const int bufSize = m_asio.currentBufferSize();
    const double latencyMs = (sr > 0.0) ? (static_cast<double>(bufSize) / sr * 1000.0) : 0.0;

    char statusText[128];
    if (m_asio.isRunning()) {
        std::snprintf(statusText, sizeof(statusText), "ACTIVE  |  %.0f kHz  |  %d spls (%.1f ms)",
                      sr / 1000.0, bufSize, latencyMs);
    } else {
        std::snprintf(statusText, sizeof(statusText), "STOPPED");
    }

    const float pillW = ImGui::CalcTextSize(statusText).x + 36.0f;
    const float availW = ImGui::GetContentRegionAvail().x;

    if (availW > pillW) {
        ImGui::SameLine(ImGui::GetCursorPosX() + availW - pillW);
    } else {
        ImGui::SameLine(0, 12);
    }

    renderStatusPill(m_asio.isRunning(), sr, bufSize, latencyMs);
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
            if (CenteredButton("X##ClearSearch", ImVec2(22, 22))) {
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
        if (CenteredButton("Rescan All", ImVec2(90, 24))) {
            m_scanner.scanAll();
        }
        if (m_scanner.isScanning()) {
            ImGui::SameLine(0, 10);
            ImGui::TextColored(ImVec4(0.40f, 0.75f, 1.0f, 1.0f), "Scanning plugins...");
        }
        ImGui::Separator();

        float leftWidth = 230.0f;
        float contentHeight = ImGui::GetContentRegionAvail().y - 36.0f;

        // LEFT PANE: Folder / Developer Tree View
        ImGui::BeginChild("CategoryTreeChild", ImVec2(leftWidth, contentHeight), true);
        {
            ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.85f, 1.0f), "CATEGORIES");
            ImGui::Separator();

            bool isAll = (!m_showFavoritesFilter && m_selectedDeveloperFilter == "All" && m_selectedFormatFilter == "All");
            char allLabel[64];
            std::snprintf(allLabel, sizeof(allLabel), "All Plugins (%zu)", m_scanner.numPlugins());
            if (ImGui::Selectable(allLabel, isAll)) {
                m_showFavoritesFilter = false;
                m_selectedDeveloperFilter = "All";
                m_selectedFormatFilter = "All";
            }

            char favLabel[64];
            std::snprintf(favLabel, sizeof(favLabel), "\xE2\x98\x85 Favourites (%zu)", m_scanner.numFavorites());
            if (ImGui::Selectable(favLabel, m_showFavoritesFilter)) {
                m_showFavoritesFilter = true;
                m_selectedDeveloperFilter = "All";
                m_selectedFormatFilter = "All";
            }

            ImGui::Spacing();
            if (ImGui::TreeNodeEx("Formats", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool isVst3 = (!m_showFavoritesFilter && m_selectedFormatFilter == "VST3" && m_selectedDeveloperFilter == "All");
                if (ImGui::Selectable("VST3", isVst3)) {
                    m_showFavoritesFilter = false;
                    m_selectedFormatFilter = "VST3";
                    m_selectedDeveloperFilter = "All";
                }
                bool isClap = (!m_showFavoritesFilter && m_selectedFormatFilter == "CLAP" && m_selectedDeveloperFilter == "All");
                if (ImGui::Selectable("CLAP", isClap)) {
                    m_showFavoritesFilter = false;
                    m_selectedFormatFilter = "CLAP";
                    m_selectedDeveloperFilter = "All";
                }
                bool isBuiltin = (!m_showFavoritesFilter && m_selectedFormatFilter == "Built-In" && m_selectedDeveloperFilter == "All");
                if (ImGui::Selectable("Built-In", isBuiltin)) {
                    m_showFavoritesFilter = false;
                    m_selectedFormatFilter = "Built-In";
                    m_selectedDeveloperFilter = "All";
                }
                ImGui::TreePop();
            }

            ImGui::Spacing();
            if (ImGui::TreeNodeEx("Developers", ImGuiTreeNodeFlags_DefaultOpen)) {
                auto devs = m_scanner.getDevelopers();
                for (const auto& dev : devs) {
                    bool isDev = (!m_showFavoritesFilter && m_selectedDeveloperFilter == dev);
                    if (ImGui::Selectable(dev.c_str(), isDev)) {
                        m_showFavoritesFilter = false;
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
                static_cast<plugins::PluginSortMode>(m_pluginSortMode),
                m_showFavoritesFilter
            );

            if (m_insertTargetBlockIndex >= 0 && m_insertTargetBranchIndex >= 0) {
                ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.95f, 1.0f),
                                   "Target: Parallel Block %d, Branch %d  |  %zu plugins matching",
                                   m_insertTargetBlockIndex + 1, m_insertTargetBranchIndex + 1, filtered.size());
            } else if (m_showFavoritesFilter) {
                ImGui::TextColored(ImVec4(0.98f, 0.82f, 0.25f, 1.0f),
                                   "Viewing: \xE2\x98\x85 Favourites  |  %zu plugins matching",
                                   filtered.size());
            } else {
                ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, 1.0f),
                                   "Viewing: %s %s  |  %zu plugins matching",
                                   (m_selectedDeveloperFilter != "All" ? ("[" + m_selectedDeveloperFilter + "]").c_str() : ""),
                                   (m_selectedFormatFilter != "All" ? ("[" + m_selectedFormatFilter + "]").c_str() : "All"),
                                   filtered.size());
            }
            ImGui::Separator();

            if (ImGui::BeginTable("PluginsTable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Fav", ImGuiTableColumnFlags_WidthFixed, 28.0f);
                ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 75.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.50f);
                ImGui::TableSetupColumn("Developer", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 84.0f);
                ImGui::TableHeadersRow();

                for (size_t p = 0; p < filtered.size(); ++p) {
                    const auto& desc = filtered[p];
                    ImGui::TableNextRow();

                    // Column 0: Star / Favourite
                    ImGui::TableNextColumn();
                    std::string favKey = desc.path.empty() ? desc.name : desc.path;
                    bool isFav = m_scanner.isFavorite(favKey);
                    char starBtnId[32];
                    std::snprintf(starBtnId, sizeof(starBtnId), "##fav_%zu", p);
                    if (renderCenteredStarButton(starBtnId, ImVec2(24.0f, 22.0f), isFav)) {
                        m_scanner.toggleFavorite(favKey);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(isFav ? "Remove from Favourites" : "Add to Favourites");
                    }

                    // Column 1: Format
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImVec4 badgeCol = (desc.type == plugins::PluginType::VST3) ? ImVec4(0.3f, 0.7f, 1.0f, 1.0f) :
                                      (desc.type == plugins::PluginType::CLAP) ? ImVec4(0.9f, 0.5f, 0.9f, 1.0f) :
                                                                                 ImVec4(0.98f, 0.60f, 0.20f, 1.0f);
                    ImGui::TextColored(badgeCol, "%s", desc.typeString().c_str());

                    // Column 2: Name
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

                    // Column 3: Developer
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", desc.vendor.c_str());

                    // Column 4: Action (+ Insert)
                    ImGui::TableNextColumn();
                    float availW = ImGui::GetContentRegionAvail().x;
                    if (availW > 74.0f) {
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (availW - 74.0f) * 0.5f);
                    }
                    char btnLabel[32];
                    std::snprintf(btnLabel, sizeof(btnLabel), "+ Insert##%zu", p);
                    bool insertClicked = CenteredButton(btnLabel, ImVec2(74.0f, 22.0f));

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
        if (CenteredButton("Search Paths...", ImVec2(120, 24))) {
            ImGui::OpenPopup("ManageSearchPathsPopup");
        }
        ImGui::SameLine(0, 8);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("(%zu search locations registered)", m_scanner.searchPaths().size());

        ImGui::SameLine(ImGui::GetWindowWidth() - 90);
        if (CenteredButton("Close", ImVec2(75, 24))) {
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
                if (CenteredButton(rmLabel, ImVec2(60, 20))) {
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
            if (CenteredButton("+ Add Path", ImVec2(90, 24))) {
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

    ImGui::OpenPopup("Praccy Updates##Modal");
    ImGui::SetNextWindowSize(ImVec2(520, 390), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Praccy Updates##Modal", &m_showUpdateModal, ImGuiWindowFlags_NoResize)) {
        ImGui::TextColored(ImVec4(0.98f, 0.60f, 0.20f, 1.0f), "PRACCY UPDATE MANAGER");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Installed Version: %s", "v1.0.2");
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
            ImGui::TextColored(ImVec4(0.40f, 0.75f, 1.0f, 1.0f), "Checking GitHub repository for %s updates...", includeBeta ? "beta" : "stable");
            ImGui::Spacing();
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 8));
        } else if (info.status == UpdateStatus::UpToDate) {
            ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.45f, 1.0f), "[v] You are up to date!");
            ImGui::TextDisabled("Channel: %s", includeBeta ? "Beta (dev branch)" : "Stable (Releases)");
            ImGui::Text("Latest version: %s", info.latestVersion.c_str());
        } else if (info.status == UpdateStatus::UpdateAvailable) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.25f, 1.0f), "[*] New %s build available!", includeBeta ? "Beta" : "Release");
            ImGui::Text("Latest: %s", info.latestVersion.c_str());
            if (!info.releaseTitle.empty()) {
                ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, 1.0f), "%s", info.releaseTitle.c_str());
            }
            if (!info.publishedDate.empty()) {
                ImGui::TextDisabled("Date: %s", info.publishedDate.c_str());
            }

            ImGui::Spacing();
            if (!info.assetUrl.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(35, 120, 60, 255));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(45, 150, 75, 255));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(25, 95, 45, 255));
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
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "Downloading update package...");
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
                ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.0f), "Extracting and verifying package files...");
            } else {
                ImGui::TextDisabled("Please wait while Praccy streams the update package...");
            }
        } else if (info.status == UpdateStatus::ReadyToInstall) {
            ImGui::TextColored(ImVec4(0.25f, 0.95f, 0.55f, 1.0f), "[v] Update downloaded and ready to apply!");
            ImGui::TextWrapped("Praccy will now close, copy the updated files into place, and restart automatically.");
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(35, 130, 65, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(45, 165, 80, 255));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(25, 105, 50, 255));
            if (CenteredButton("Restart & Apply Update Now", ImVec2(230, 32))) {
                UpdateChecker::instance().applyUpdateAndRestart();
            }
            ImGui::PopStyleColor(3);
        } else if (info.status == UpdateStatus::Error) {
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "[!] Update check / download failed");
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

void RackView::renderSettingsModal() {
    if (!m_showSettingsModal) return;

    ImGui::OpenPopup("Praccy Settings##Modal");
    ImGui::SetNextWindowSize(ImVec2(620, 440), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Praccy Settings##Modal", &m_showSettingsModal, ImGuiWindowFlags_NoResize)) {
        ImGui::TextColored(ImVec4(0.98f, 0.60f, 0.20f, 1.0f), "PRACCY SETTINGS");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("SettingsTabs")) {
            // TAB 1: AUDIO & ASIO
            if (ImGui::BeginTabItem("Audio & ASIO")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.40f, 0.75f, 1.0f, 1.0f), "ASIO DRIVER CONFIGURATION");
                ImGui::Separator();
                ImGui::Spacing();

                // Driver selector combo
                std::string currentDriverName = m_asio.driverInfo().name;
                if (currentDriverName.empty()) currentDriverName = "Select ASIO Driver...";

                ImGui::Text("Active ASIO Device:");
                ImGui::SetNextItemWidth(340);
                if (ImGui::BeginCombo("##SettingsAsioDriversCombo", currentDriverName.c_str())) {
                    for (int i = 0; i < static_cast<int>(m_cachedDrivers.size()); ++i) {
                        const bool isSelected = (m_selectedDriverIdx == i);
                        if (ImGui::Selectable(m_cachedDrivers[i].name.c_str(), isSelected)) {
                            m_selectedDriverIdx = i;
                            if (m_asio.loadDriver(m_cachedDrivers[i])) {
                                state::AppConfig cfg;
                                cfg.load();
                                cfg.lastAsioDriver = m_cachedDrivers[i].name;
                                cfg.save();
                            }
                        }
                        if (isSelected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::SameLine(0, 10);
                if (CenteredButton("Rescan Drivers", ImVec2(120, 26))) {
                    m_cachedDrivers = audio::AsioManager::enumerateDrivers();
                }

                ImGui::Spacing();
                if (CenteredButton("Open ASIO Control Panel", ImVec2(220, 26))) {
                    m_asio.openControlPanel();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Open the native hardware manufacturer's ASIO control panel (buffer size, latency)");
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, 1.0f), "Hardware Status:");
                if (m_asio.isRunning()) {
                    double sRate = m_asio.currentSampleRate();
                    int bSize = m_asio.currentBufferSize();
                    double latMs = (sRate > 0.0) ? (static_cast<double>(bSize) / sRate * 1000.0) : 0.0;
                    ImGui::BulletText("Sample Rate: %.0f Hz", sRate);
                    ImGui::BulletText("Buffer Size: %d samples", bSize);
                    ImGui::BulletText("Round-trip Latency: %.2f ms", latMs);
                    ImGui::BulletText("Audio Engine State: ACTIVE (Real-time ASIO callback streaming)");
                } else {
                    ImGui::BulletText("Audio Engine State: STOPPED (Driver loaded: %s)", m_asio.isLoaded() ? "Yes" : "No");
                }

                ImGui::EndTabItem();
            }

            // TAB 2: PLUGINS
            if (ImGui::BeginTabItem("Plugins")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.40f, 0.75f, 1.0f, 1.0f), "PLUGIN MANAGER & SCANNER");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Registered Plugins: %zu scanned", m_scanner.numPlugins());
                ImGui::Text("Search Paths: %zu directories", m_scanner.searchPaths().size());
                ImGui::Spacing();

                if (CenteredButton("Open Plugin Manager & Scanner...", ImVec2(250, 28))) {
                    m_showPluginBrowser = true;
                    m_focusPluginBrowser = true;
                }

                ImGui::SameLine(0, 10);
                if (CenteredButton("Rescan Plugins", ImVec2(140, 28))) {
                    m_scanner.scanAll();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::TextDisabled("Formats Supported: VST3 (64-bit), CLAP (64-bit), Native DSP Effects");
                ImGui::TextDisabled("Thumbnail Cache: Auto-captures live plugin GUI previews");

                ImGui::EndTabItem();
            }

            // TAB 3: UPDATES
            if (ImGui::BeginTabItem("Updates")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.40f, 0.75f, 1.0f, 1.0f), "SOFTWARE UPDATES");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::Text("Installed Version: %s", "v1.0.2");
                ImGui::Spacing();

                state::AppConfig cfg;
                cfg.load();
                bool includeBeta = cfg.checkBetaUpdates;
                if (ImGui::Checkbox("Enable beta builds from 'dev' branch on GitHub", &includeBeta)) {
                    cfg.checkBetaUpdates = includeBeta;
                    cfg.save();
                }

                ImGui::Spacing();
                if (CenteredButton("Check for Updates Now...", ImVec2(200, 28))) {
                    m_showUpdateModal = true;
                    UpdateChecker::instance().checkForUpdates(includeBeta);
                }

                ImGui::EndTabItem();
            }

            // TAB 4: ABOUT
            if (ImGui::BeginTabItem("About")) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.98f, 0.60f, 0.20f, 1.0f), "PRACCY v1.0.2");
                ImGui::TextDisabled("Lightweight, Low-Latency Guitar & Audio Practice Host");
                ImGui::Separator();
                ImGui::Spacing();

                ImGui::BulletText("Audio Engine: ASIO Real-time Engine with Lock-Free Ring Buffers");
                ImGui::BulletText("Plugin Hosting: Native VST3 & CLAP with D3D11 Texture Capture");
                ImGui::BulletText("Practice Tools: Chromatic Tuner, Metronome, Noise Gate");
                ImGui::BulletText("Routing: Serial & Parallel Split/Merge Processing");
                ImGui::BulletText("MIDI: Hardware Footswitch & Controller Learn");

                ImGui::Spacing();
                ImGui::Spacing();
                if (CenteredButton("GitHub Repository ->", ImVec2(180, 26))) {
                    ShellExecuteA(nullptr, "open", "https://github.com/praccy/praccy", nullptr, nullptr, SW_SHOWNORMAL);
                }

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        float bottomY = std::max(ImGui::GetCursorPosY() + 12.0f, 395.0f);
        ImGui::SetCursorPosY(bottomY);
        ImGui::Separator();
        if (CenteredButton("Close", ImVec2(90, 26))) {
            m_showSettingsModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace praccy::ui

