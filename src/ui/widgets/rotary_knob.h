#pragma once

#include <imgui.h>
#include <string>
#include <cmath>
#include <algorithm>
#include <numbers>
#include <cstdio>
#include "../design_tokens.h"
#include "../ui_helpers.h"

extern ImFont* g_fontUI;
extern ImFont* g_fontMono;

namespace praccy::ui {

struct HardwareKnobOptions {
    float radius{13.0f};          // Outer indicator radius (typical 11.0f - 16.0f)
    float arcThickness{2.2f};     // Gauge of the outer active arc
    bool showValue{true};         // Numerical value + unit in g_fontMono
    bool showLabel{true};         // Full label centered below
    const char* unit{""};         // Unit suffix (e.g. "dB", "%", "Hz", "ms")
    float sensitivity{0.005f};    // Base drag speed
};

/**
 * @brief Professional vector-rendered hardware rotary knob widget.
 * 
 * Features:
 * - 240° sweep arc (7 o'clock to 5 o'clock) with anti-aliased path strokes
 * - Outer active indicator arc colored with tokens.signal.active
 * - Metallic knurled disc body with subtle bevel border
 * - Directional needle pointer line with central cap
 * - Double-click reset to defaultValue
 * - Fine adjustment resolution (5x precision) when holding Shift
 * - Two-tier typography: numerical value + unit in g_fontMono, full label centered below
 */
inline bool DrawHardwareKnob(
    const char* id,
    const char* label,
    float* value,
    float minVal,
    float maxVal,
    float defaultVal = 0.0f,
    const char* format = "%.1f",
    const HardwareKnobOptions& opt = HardwareKnobOptions{}
) {
    if (!value) return false;

    ImVec2 screenPos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const auto& tokens = themeTokens();

    const float R = opt.radius;
    const float diameter = R * 2.0f;
    const float pad = 4.0f;
    const float widgetW = diameter + pad * 2.0f;
    const float widgetH = diameter + pad * 2.0f;
    const ImVec2 center(screenPos.x + widgetW * 0.5f, screenPos.y + pad + R);

    // InvisibleButton to handle hit-testing, mouse dragging, wheel, double-click
    ImGui::InvisibleButton(id, ImVec2(widgetW, widgetH));
    const bool isHovered = ImGui::IsItemHovered();
    const bool isActive = ImGui::IsItemActive();
    const ImGuiID knobId = ImGui::GetItemID();

    bool valueChanged = false;

    // 1. Double-click reset with drag suppression
    if (s_suppressResetId == knobId) {
        if (!io.MouseDown[0]) {
            s_suppressResetId = 0;
        } else {
            *value = std::clamp(defaultVal, minVal, maxVal);
        }
    } else if (isActive) {
        // Shift key enables 5x finer adjustment resolution
        const float speedMult = io.KeyShift ? 0.2f : 1.0f;
        const float range = (maxVal - minVal);
        const float speed = opt.sensitivity * speedMult * (range != 0.0f ? range : 1.0f);
        const float delta = -io.MouseDelta.y * speed;
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

    // Mouse wheel support
    if (isHovered && io.MouseWheel != 0.0f) {
        const float speedMult = io.KeyShift ? 0.02f : 0.05f;
        const float range = (maxVal - minVal);
        const float delta = io.MouseWheel * speedMult * (range != 0.0f ? range : 1.0f);
        *value = std::clamp(*value + delta, minVal, maxVal);
        valueChanged = true;
    }

    // 2. Normalization & Angles
    // 240° sweep from 7 o'clock (150° = 2.61799 rad) to 5 o'clock (390° = 30° = 6.80678 rad)
    constexpr float kStartAngle = 2.61799387799f; // 150 deg (7 o'clock)
    constexpr float kSweepAngle = 4.18879020479f; // 240 deg
    constexpr float kEndAngle   = kStartAngle + kSweepAngle;

    float norm = (maxVal > minVal) ? ((*value - minVal) / (maxVal - minVal)) : 0.0f;
    norm = std::clamp(norm, 0.0f, 1.0f);
    const float kCurrentAngle = kStartAngle + norm * kSweepAngle;

    // 3. Drop Shadow & Background Outer Track Arc
    dl->AddCircleFilled(ImVec2(center.x, center.y + 1.5f), R + 1.5f, tokens.borders.shadow.u32);

    // Dimmed background track arc
    dl->PathClear();
    dl->PathArcTo(center, R, kStartAngle, kEndAngle, 24);
    dl->PathStroke(tokens.surfaces.frameBg.u32, 0, opt.arcThickness);

    // 4. Outer Active Indicator Arc
    if (norm > 0.002f) {
        dl->PathClear();
        dl->PathArcTo(center, R, kStartAngle, kCurrentAngle, 24);
        const ImU32 arcColor = isActive ? tokens.borders.focus.u32 :
                               (isHovered ? tokens.text.accent.u32 : tokens.signal.active.u32);
        dl->PathStroke(arcColor, 0, opt.arcThickness);
    }

    // 5. Metallic Knurled Disc Body
    const float bodyR = R - opt.arcThickness - 1.2f;
    dl->AddCircleFilled(center, bodyR, tokens.surfaces.cardBg.u32);

    // Knurling ticks around rim
    constexpr int kKnurlTicks = 12;
    for (int k = 0; k < kKnurlTicks; ++k) {
        float ka = kCurrentAngle + static_cast<float>(k) * (2.0f * std::numbers::pi_v<float> / static_cast<float>(kKnurlTicks));
        ImVec2 p1(center.x + std::cos(ka) * (bodyR - 2.0f), center.y + std::sin(ka) * (bodyR - 2.0f));
        ImVec2 p2(center.x + std::cos(ka) * (bodyR - 0.5f), center.y + std::sin(ka) * (bodyR - 0.5f));
        dl->AddLine(p1, p2, tokens.surfaces.buttonBg.u32, 1.0f);
    }

    // Bevel ring
    const ImU32 bevelCol = isActive ? tokens.borders.focus.u32 :
                           (isHovered ? tokens.borders.cardHovered.u32 : tokens.borders.subtle.u32);
    dl->AddCircle(center, bodyR, bevelCol, 24, 1.2f);

    // 6. Central Cap
    const float capR = bodyR * 0.42f;
    dl->AddCircleFilled(center, capR, tokens.surfaces.buttonBg.u32);
    dl->AddCircle(center, capR, tokens.borders.subtle.u32, 16, 1.0f);

    // 7. Directional Needle Pointer
    const ImVec2 needleInner(center.x + std::cos(kCurrentAngle) * capR,
                             center.y + std::sin(kCurrentAngle) * capR);
    const ImVec2 needleOuter(center.x + std::cos(kCurrentAngle) * (bodyR - 1.0f),
                             center.y + std::sin(kCurrentAngle) * (bodyR - 1.0f));
    dl->AddLine(needleInner, needleOuter, tokens.text.primary.u32, 2.0f);

    // 8. Two-tier Typography
    float textCursorY = screenPos.y + widgetH - 2.0f;
    if (opt.showValue) {
        char valBuf[32];
        if (opt.unit && opt.unit[0] != '\0') {
            char numBuf[24];
            std::snprintf(numBuf, sizeof(numBuf), format, *value);
            std::snprintf(valBuf, sizeof(valBuf), "%s%s", numBuf, opt.unit);
        } else {
            std::snprintf(valBuf, sizeof(valBuf), format, *value);
        }
        if (g_fontMono) ImGui::PushFont(g_fontMono);
        ImVec2 vSz = ImGui::CalcTextSize(valBuf);
        ImVec2 vPos(center.x - vSz.x * 0.5f, textCursorY);
        dl->AddText(vPos, isActive ? tokens.text.accent.u32 : (isHovered ? tokens.text.primary.u32 : tokens.text.secondary.u32), valBuf);
        if (g_fontMono) ImGui::PopFont();
        textCursorY += vSz.y;
    }

    if (opt.showLabel && label && label[0] != '\0') {
        ImVec2 lSz = ImGui::CalcTextSize(label);
        ImVec2 lPos(center.x - lSz.x * 0.5f, textCursorY);
        dl->AddText(lPos, tokens.text.secondary.u32, label);
    }

    // 9. Tooltip with full metadata
    if (isHovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        char valBuf[64];
        std::snprintf(valBuf, sizeof(valBuf), format, *value);
        if (opt.unit && opt.unit[0] != '\0') {
            ImGui::SetTooltip("%s: %s %s\n(Shift+Drag for fine adjustment, Double-click to reset)",
                              label ? label : "Parameter", valBuf, opt.unit);
        } else {
            ImGui::SetTooltip("%s: %s\n(Shift+Drag for fine adjustment, Double-click to reset)",
                              label ? label : "Parameter", valBuf);
        }
    }

    return valueChanged;
}

} // namespace praccy::ui
