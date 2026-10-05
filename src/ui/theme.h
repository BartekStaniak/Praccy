#pragma once

#include <imgui.h>

namespace praccy::ui {

inline void applyPraccyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;

    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(6, 6);
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.94f, 0.96f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.11f, 0.12f, 0.14f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.14f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_Border]                = ImVec4(0.24f, 0.27f, 0.32f, 1.00f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.26f, 0.29f, 0.36f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.32f, 0.36f, 0.44f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.09f, 0.10f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.13f, 0.14f, 0.17f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.10f, 0.11f, 0.13f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.28f, 0.31f, 0.38f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.36f, 0.40f, 0.49f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.44f, 0.49f, 0.60f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.98f, 0.60f, 0.20f, 1.00f); // Warm Amber
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.98f, 0.60f, 0.20f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 0.72f, 0.35f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.22f, 0.25f, 0.31f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.30f, 0.34f, 0.42f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.98f, 0.60f, 0.20f, 0.80f);
    colors[ImGuiCol_Header]                = ImVec4(0.20f, 0.23f, 0.28f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.28f, 0.32f, 0.40f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.98f, 0.60f, 0.20f, 0.80f);
    colors[ImGuiCol_Separator]             = ImVec4(0.25f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.30f, 0.34f, 0.42f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.24f, 0.28f, 0.36f, 1.00f);
}

} // namespace praccy::ui
