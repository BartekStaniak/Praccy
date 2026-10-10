#pragma once

#include "design_tokens.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <cmath>
#include <algorithm>

namespace praccy::ui {

// State tracking for double-click resets to suppress subsequent mouse-drag overrides
inline ImGuiID s_suppressResetId = 0;

inline bool ResettableSliderFloat(const char* label, float* v, float v_min, float v_max, float default_val, const char* format = "%.3f", ImGuiSliderFlags flags = 0) {
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

inline bool CenteredButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
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

    const float barHalfW = 1.0f;
    const float barHalfH = 4.5f;
    const float offset = 2.5f;

    dl->AddRectFilled(ImVec2(cx - offset - barHalfW, cy - barHalfH), ImVec2(cx - offset + barHalfW, cy + barHalfH), iconCol, 0.5f);
    dl->AddRectFilled(ImVec2(cx + offset - barHalfW, cy - barHalfH), ImVec2(cx + offset + barHalfW, cy + barHalfH), iconCol, 0.5f);

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

inline void renderBadgePill(const char* label, ImU32 bgCol, ImU32 borderCol, ImU32 textCol, float height = 18.0f) {
    ImVec2 badgePos = ImGui::GetCursorScreenPos();
    ImVec2 textSz = ImGui::CalcTextSize(label);
    const float padX = 6.0f;
    float badgeW = textSz.x + padX * 2.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 minPt = badgePos;
    ImVec2 maxPt(badgePos.x + badgeW, badgePos.y + height);

    dl->AddRectFilled(minPt, maxPt, bgCol, height * 0.5f);
    dl->AddRect(minPt, maxPt, borderCol, height * 0.5f, 0, 1.0f);

    float textX = badgePos.x + padX;
    float textY = badgePos.y + (height - textSz.y) * 0.5f;
    dl->AddText(ImVec2(textX, textY), textCol, label);
    ImGui::Dummy(ImVec2(badgeW, height));
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

} // namespace praccy::ui
