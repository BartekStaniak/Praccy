#pragma once

#include "design_tokens.h"
#include "ui_spec.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <cmath>
#include <algorithm>

#include <initializer_list>
#include <utility>

namespace praccy::ui {

class ScopedStyleColor {
public:
    ScopedStyleColor(ImGuiCol idx, const ImVec4& col, bool condition = true)
        : m_count(condition ? 1 : 0) {
        if (condition) ImGui::PushStyleColor(idx, col);
    }

    ScopedStyleColor(ImGuiCol idx, ImU32 col, bool condition = true)
        : m_count(condition ? 1 : 0) {
        if (condition) ImGui::PushStyleColor(idx, col);
    }

    ScopedStyleColor(std::initializer_list<std::pair<ImGuiCol, ImVec4>> styles, bool condition = true)
        : m_count(condition ? static_cast<int>(styles.size()) : 0) {
        if (condition) {
            for (const auto& [idx, col] : styles) {
                ImGui::PushStyleColor(idx, col);
            }
        }
    }

    ~ScopedStyleColor() {
        if (m_count > 0) ImGui::PopStyleColor(m_count);
    }

    ScopedStyleColor(const ScopedStyleColor&) = delete;
    ScopedStyleColor& operator=(const ScopedStyleColor&) = delete;

private:
    int m_count{0};
};

class ScopedStyleVar {
public:
    ScopedStyleVar(ImGuiStyleVar idx, float val, bool condition = true)
        : m_count(condition ? 1 : 0) {
        if (condition) ImGui::PushStyleVar(idx, val);
    }

    ScopedStyleVar(ImGuiStyleVar idx, const ImVec2& val, bool condition = true)
        : m_count(condition ? 1 : 0) {
        if (condition) ImGui::PushStyleVar(idx, val);
    }

    ~ScopedStyleVar() {
        if (m_count > 0) ImGui::PopStyleVar(m_count);
    }

    ScopedStyleVar(const ScopedStyleVar&) = delete;
    ScopedStyleVar& operator=(const ScopedStyleVar&) = delete;

private:
    int m_count{0};
};

// State tracking for double-click resets to suppress subsequent mouse-drag overrides
inline ImGuiID s_suppressResetId = 0;

inline bool ResettableSliderFloat(const char* label, float* v, float v_min, float v_max, float default_val, const char* format = "%.3f", ImGuiSliderFlags flags = 0) {
    ImGuiID id = ImGui::GetID(label);
    if (!ImGui::GetIO().MouseDown[0] && s_suppressResetId == id) {
        s_suppressResetId = 0;
    }

    // Suppress stock opaque grab block so it never collides behind value text
    ScopedStyleColor hideStock({
        {ImGuiCol_FrameBg, ColorToken::transparent().vec4},
        {ImGuiCol_FrameBgHovered, ColorToken::transparent().vec4},
        {ImGuiCol_FrameBgActive, ColorToken::transparent().vec4},
        {ImGuiCol_SliderGrab, ColorToken::transparent().vec4},
        {ImGuiCol_SliderGrabActive, ColorToken::transparent().vec4},
        {ImGuiCol_Border, ColorToken::transparent().vec4}
    });
    ScopedStyleVar grabSize(ImGuiStyleVar_GrabMinSize, 0.0f);

    bool changed = ImGui::SliderFloat(label, v, v_min, v_max, "", flags);
    const ImVec2 rMin = ImGui::GetItemRectMin();
    const ImVec2 rMax = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsItemHovered();
    const bool active  = ImGui::IsItemActive();

    if (s_suppressResetId == id) {
        if (*v != default_val) {
            *v = default_val;
            changed = true;
            ImGui::MarkItemEdited(id);
        }
        ImGui::ClearActiveID();
    }

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *v = default_val;
        s_suppressResetId = id;
        ImGui::MarkItemEdited(id);
        ImGui::ClearActiveID();
        changed = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();
    const float w = rMax.x - rMin.x;
    const float h = rMax.y - rMin.y;

    // 1. Recessed outer frame
    const ImU32 borderCol = active ? tokens.borders.focus.u32
                          : (hovered ? tokens.borders.strong.u32 : tokens.borders.subtle.u32);
    dl->AddRectFilled(rMin, rMax, tokens.surfaces.frameBg.u32, 4.0f);
    dl->AddRect(rMin, rMax, borderCol, 4.0f);

    // 2. Rail track & unity/default tick mark
    const float padX = 2.0f;
    const float trackX1 = rMin.x + padX;
    const float trackX2 = rMax.x - padX;
    const float trackW = std::max(1.0f, trackX2 - trackX1);
    const float range = (v_max > v_min) ? (v_max - v_min) : 1.0f;
    const float defNorm = std::clamp((default_val - v_min) / range, 0.0f, 1.0f);
    const float valNorm = std::clamp((*v - v_min) / range, 0.0f, 1.0f);
    const float defX = trackX1 + defNorm * trackW;
    const float valX = trackX1 + valNorm * trackW;

    // Subtle translucent fill bar (22% alpha) so foreground text always retains >10:1 WCAG contrast
    const bool isBipolar = (v_min < 0.0f && v_max > 0.0f);
    float fillX1 = rMin.x + 1.0f;
    float fillX2 = rMin.x + 1.0f;
    ImDrawFlags cornerFlags = ImDrawFlags_RoundCornersNone;

    if (isBipolar) {
        if (valNorm > defNorm + 0.001f) {
            fillX1 = defX;
            fillX2 = (valNorm >= 0.999f) ? (rMax.x - 1.0f) : std::min(rMax.x - 1.0f, valX + 1.0f);
            cornerFlags = (valNorm >= 0.999f) ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone;
        } else if (valNorm < defNorm - 0.001f) {
            fillX1 = (valNorm <= 0.001f) ? (rMin.x + 1.0f) : std::max(rMin.x + 1.0f, valX - 1.0f);
            fillX2 = defX;
            cornerFlags = (valNorm <= 0.001f) ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersNone;
        }
    } else {
        if (valNorm > 0.001f) {
            fillX1 = rMin.x + 1.0f;
            fillX2 = (valNorm >= 0.999f) ? (rMax.x - 1.0f) : std::min(rMax.x - 1.0f, valX + 1.0f);
            cornerFlags = (valNorm >= 0.999f) ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersLeft;
        }
    }

    if (fillX2 > fillX1 + 0.5f) {
        const ImU32 fillCol = active ? tokens.borders.focus.withAlpha(0.28f).u32
                                     : tokens.signal.active.withAlpha(0.22f).u32;
        dl->AddRectFilled(ImVec2(fillX1, rMin.y + 1.0f), ImVec2(fillX2, rMax.y - 1.0f), fillCol, 3.0f, cornerFlags);
    }

    // Unity / default reference tick at bottom & top edge (only if inside track)
    if (defNorm > 0.02f && defNorm < 0.98f) {
        dl->AddLine(ImVec2(defX, rMin.y + 1.0f), ImVec2(defX, rMin.y + 4.5f), tokens.borders.strong.u32, 1.2f);
        dl->AddLine(ImVec2(defX, rMax.y - 4.5f), ImVec2(defX, rMax.y - 1.0f), tokens.borders.strong.u32, 1.2f);
    }

    // Crisp vertical cursor indicator line (2px wide) flush with track edges at 0% and 100%
    const ImU32 thumbCol = active ? tokens.borders.focus.u32 : tokens.text.accent.u32;
    dl->AddLine(ImVec2(valX, rMin.y + 1.5f), ImVec2(valX, rMax.y - 1.5f), thumbCol, 2.0f);

    // 3. High-contrast centered value readout
    if (format && format[0] != '\0') {
        char valBuf[64];
        std::snprintf(valBuf, sizeof(valBuf), format, *v);
        const ImVec2 tSz = ImGui::CalcTextSize(valBuf);
        const ImVec2 tPos(rMin.x + (w - tSz.x) * 0.5f, rMin.y + (h - tSz.y) * 0.5f);
        dl->AddText(tPos, tokens.text.primary.u32, valBuf);
    }

    return changed;
}

inline bool ResettableVSliderFloat(const char* label, const ImVec2& size, float* v, float v_min, float v_max, float default_val, const char* format = "", ImGuiSliderFlags flags = 0) {
    ImGuiID id = ImGui::GetID(label);
    if (!ImGui::GetIO().MouseDown[0] && s_suppressResetId == id) {
        s_suppressResetId = 0;
    }

    bool changed = ImGui::VSliderFloat(label, size, v, v_min, v_max, format, flags);

    if (s_suppressResetId == id) {
        if (*v != default_val) {
            *v = default_val;
            changed = true;
            ImGui::MarkItemEdited(id);
        }
        ImGui::ClearActiveID();
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *v = default_val;
        s_suppressResetId = id;
        ImGui::MarkItemEdited(id);
        ImGui::ClearActiveID();
        changed = true;
    }

    return changed;
}

[[maybe_unused]] inline bool ResettableDragFloat(const char* label, float* v, float v_speed, float v_min, float v_max, float default_val, const char* format = "%.0f", ImGuiSliderFlags flags = 0) {
    ImGuiID id = ImGui::GetID(label);
    if (!ImGui::GetIO().MouseDown[0] && s_suppressResetId == id) {
        s_suppressResetId = 0;
    }

    bool changed = ImGui::DragFloat(label, v, v_speed, v_min, v_max, format, flags);

    if (s_suppressResetId == id) {
        if (*v != default_val) {
            *v = default_val;
            changed = true;
            ImGui::MarkItemEdited(id);
        }
        ImGui::ClearActiveID();
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        *v = default_val;
        s_suppressResetId = id;
        ImGui::MarkItemEdited(id);
        ImGui::ClearActiveID();
        changed = true;
    }

    return changed;
}

inline bool CenteredButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    float actualH = size.y > 0.0f ? size.y : ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.5f, 0.5f));
    bool popPad = false;
    if (actualH > 0.0f) {
        float fontH = ImGui::GetFontSize();
        float padY = std::max(0.0f, (actualH - fontH) * 0.5f);
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
    bool clicked = ImGui::Button(label, ImVec2(size.x, actualH));
    if (popPad) ImGui::PopStyleVar();
    ImGui::PopStyleVar();
    return clicked;
}

inline void drawStarGeometry(ImDrawList* dl, ImVec2 center, float rOuter, float rInner, ImU32 fillCol, ImU32 strokeCol = 0, float strokeThickness = 1.4f) {
    const float pi = 3.1415926535f;
    ImVec2 pts[10];
    for (int i = 0; i < 10; ++i) {
        float angle = -pi * 0.5f + (i * pi / 5.0f);
        float r = (i % 2 == 0) ? rOuter : rInner;
        pts[i] = ImVec2(center.x + std::cos(angle) * r, center.y + std::sin(angle) * r);
    }
    if (fillCol != 0) {
        for (int i = 0; i < 10; ++i) {
            int next = (i + 1) % 10;
            dl->AddTriangleFilled(center, pts[i], pts[next], fillCol);
        }
        dl->AddPolyline(pts, 10, fillCol, ImDrawFlags_Closed, strokeThickness);
    }
    if (strokeCol != 0 && strokeCol != fillCol) {
        dl->AddPolyline(pts, 10, strokeCol, ImDrawFlags_Closed, strokeThickness);
    }
}

inline bool renderCenteredStarButton(const char* id, const ImVec2& size = ImVec2(24, 22), bool isFavorite = false) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();

    if (held) {
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), tokens.surfaces.buttonBgActive, 4.0f);
    } else if (hovered) {
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), tokens.surfaces.buttonBgHovered, 4.0f);
    }

    float cx = std::floor(pos.x + size.x * 0.5f);
    float cy = std::floor(pos.y + size.y * 0.5f);

    if (isFavorite) {
        ImU32 fillCol = held ? static_cast<ImU32>(tokens.text.accent) : (hovered ? static_cast<ImU32>(tokens.borders.cardGlowActive) : static_cast<ImU32>(tokens.text.accent));
        drawStarGeometry(dl, ImVec2(cx, cy), 6.8f, 2.9f, fillCol, 0, 1.4f);
    } else {
        ImU32 strokeCol = held ? static_cast<ImU32>(tokens.borders.focus) : (hovered ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.muted));
        drawStarGeometry(dl, ImVec2(cx, cy), 6.8f, 2.9f, 0, strokeCol, 1.4f);
    }

    return clicked;
}

inline bool renderCenteredSplitButton(const char* id, const ImVec2& size = ImVec2(24, 20)) {
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

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 4.0f);

    float cx = std::floor(pos.x + size.x * 0.5f);
    float cy = std::floor(pos.y + size.y * 0.5f);

    // Y-fork parallel routing split glyph (replaces misleading '||' pause bars)
    dl->AddLine(ImVec2(cx - 4.5f, cy), ImVec2(cx - 0.5f, cy), iconCol, 1.5f);
    dl->AddLine(ImVec2(cx - 0.5f, cy), ImVec2(cx + 3.5f, cy - 3.5f), iconCol, 1.4f);
    dl->AddLine(ImVec2(cx - 0.5f, cy), ImVec2(cx + 3.5f, cy + 3.5f), iconCol, 1.4f);
    dl->AddCircleFilled(ImVec2(cx + 4.0f, cy - 3.5f), 1.3f, iconCol);
    dl->AddCircleFilled(ImVec2(cx + 4.0f, cy + 3.5f), 1.3f, iconCol);

    return clicked;
}

inline bool renderCenteredDeleteButton(const char* id, const ImVec2& size = ImVec2(22, 20)) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();

    ImU32 bgCol;
    if (held)         bgCol = tokens.surfaces.cardBgFaulted;
    else if (hovered) bgCol = tokens.surfaces.cardBgFaulted;
    else              bgCol = tokens.surfaces.buttonBg;

    ImU32 borderCol = hovered ? static_cast<ImU32>(tokens.borders.cardFaulted) : static_cast<ImU32>(tokens.borders.subtle);
    ImU32 iconCol   = hovered ? static_cast<ImU32>(tokens.text.error) : static_cast<ImU32>(tokens.text.secondary);

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 4.0f);

    float cx = std::floor(pos.x + size.x * 0.5f);
    float cy = std::floor(pos.y + size.y * 0.5f);

    const float arm = std::min(4.0f, (size.y - 6.0f) * 0.5f);
    dl->AddLine(ImVec2(cx - arm, cy - arm), ImVec2(cx + arm, cy + arm), iconCol, 1.8f);
    dl->AddLine(ImVec2(cx - arm, cy + arm), ImVec2(cx + arm, cy - arm), iconCol, 1.8f);

    return clicked;
}

inline bool renderCenteredPauseButton(const char* id, const ImVec2& size = ImVec2(22, 20), bool isBypassed = false) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();

    ImU32 bgCol;
    if (held)         bgCol = tokens.surfaces.buttonBgActive;
    else if (hovered) bgCol = tokens.surfaces.buttonBgHovered;
    else              bgCol = isBypassed ? static_cast<ImU32>(tokens.surfaces.cardBgBypassed) : static_cast<ImU32>(tokens.surfaces.buttonBg);

    ImU32 borderCol = hovered ? static_cast<ImU32>(tokens.borders.focus) : (isBypassed ? static_cast<ImU32>(tokens.borders.cardBypassed) : static_cast<ImU32>(tokens.borders.subtle));
    ImU32 iconCol   = isBypassed ? static_cast<ImU32>(tokens.text.muted) : (hovered ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.signal.active));

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

inline bool renderCenteredMinimiseButton(const char* id, const ImVec2& size = ImVec2(24, 24), bool isMinimized = false) {
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

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 4.0f);

    float cx = std::floor(pos.x + size.x * 0.5f);
    float cy = std::floor(pos.y + size.y * 0.5f);

    if (isMinimized) {
        const float bArm = 4.0f;
        dl->AddRect(ImVec2(cx - bArm, cy - bArm), ImVec2(cx + bArm, cy + bArm), iconCol, 1.0f, 0, 1.6f);
        dl->AddLine(ImVec2(cx - bArm, cy - bArm + 2.5f), ImVec2(cx + bArm, cy - bArm + 2.5f), iconCol, 1.2f);
    } else {
        const float arm = 4.5f;
        dl->AddLine(ImVec2(cx - arm, cy), ImVec2(cx + arm, cy), iconCol, 2.0f);
    }

    return clicked;
}

inline bool renderSlidingPillToggle(const char* id, bool* value, const ImVec2& size = ImVec2(38.0f, 18.0f)) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    if (clicked) {
        *value = !(*value);
    }
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip(*value ? "Active: Click to Bypass" : "Bypassed: Click to Enable");
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();
    const float radius = size.y * 0.5f;

    ImU32 bgCol = *value ? static_cast<ImU32>(tokens.signal.active) : static_cast<ImU32>(tokens.surfaces.frameBg);
    ImU32 borderCol = hovered ? static_cast<ImU32>(tokens.borders.focus) :
                      (*value ? static_cast<ImU32>(tokens.borders.cardGlowActive) : static_cast<ImU32>(tokens.borders.subtle));

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, radius);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, radius, 0, 1.2f);

    const float knobRadius = radius - 2.5f;
    const float knobX = *value ? (pos.x + size.x - radius) : (pos.x + radius);
    const float knobY = pos.y + radius;
    ImU32 knobCol = *value ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.muted);

    dl->AddCircleFilled(ImVec2(knobX, knobY), knobRadius, knobCol);
    return clicked;
}

inline void renderBadgePill(const char* label, ImU32 bgCol, ImU32 borderCol, ImU32 textCol, float height = 0.0f) {
    float actualH = (height > 0.0f) ? height : ImGui::GetFrameHeight();
    ImVec2 badgePos = ImGui::GetCursorScreenPos();
    ImVec2 textSz = ImGui::CalcTextSize(label);
    const float padX = 10.0f;
    float badgeW = textSz.x + padX * 2.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 minPt = badgePos;
    ImVec2 maxPt(badgePos.x + badgeW, badgePos.y + actualH);

    dl->AddRectFilled(minPt, maxPt, bgCol, actualH * 0.5f);
    dl->AddRect(minPt, maxPt, borderCol, actualH * 0.5f, 0, 1.0f);

    float textX = badgePos.x + padX;
    float textY = badgePos.y + (actualH - textSz.y) * 0.5f;
    dl->AddText(ImVec2(textX, textY), textCol, label);
    ImGui::Dummy(ImVec2(badgeW, actualH));
}

inline float computeHudToastAlpha(float remainingTime, float totalDuration = 1.8f) noexcept {
    if (std::isnan(remainingTime) || std::isnan(totalDuration)) return 0.0f;
    if (!std::isfinite(totalDuration) || totalDuration <= 0.0f) return 0.0f;
    if (!std::isfinite(remainingTime)) {
        return (remainingTime > 0.0f) ? 1.0f : 0.0f;
    }
    if (remainingTime <= 0.0f) return 0.0f;
    if (remainingTime >= totalDuration) return 1.0f;
    return std::clamp(remainingTime / totalDuration, 0.0f, 1.0f);
}



inline void BeginCardFrame(const char* str_id, float height = 0.0f, const ImVec2& padding = ImVec2(Metrics::kCardPaddingX, Metrics::kCardPaddingY)) {
    const auto& tokens = themeTokens();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.cardBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.subtle.vec4);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Metrics::kCornerRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);

    ImGuiChildFlags childFlags = ImGuiChildFlags_Borders;
    ImGuiWindowFlags winFlags = ImGuiWindowFlags_None;
    if (height <= 0.0f) {
        childFlags |= ImGuiChildFlags_AutoResizeY;
        winFlags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }
    ImGui::BeginChild(str_id, ImVec2(0.0f, height), childFlags, winFlags);
}

inline void EndCardFrame() {
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::Spacing();
}

} // namespace praccy::ui
