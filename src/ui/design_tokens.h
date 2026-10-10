#pragma once

#include <imgui.h>
#include <cstdint>
#include <atomic>

namespace praccy::ui {

enum class ThemeId : uint8_t {
    ObsidianStudio = 0,
    CyberMidnight  = 1,
    NordicSlate    = 2,
    VintageConsole = 3,
    Count          = 4
};

struct ColorToken {
    ImU32 u32{0};
    ImVec4 vec4{0.0f, 0.0f, 0.0f, 0.0f};
    float r{0.0f};
    float g{0.0f};
    float b{0.0f};
    float a{1.0f};

    constexpr ColorToken() = default;
    constexpr ColorToken(ImU32 u, ImVec4 v) noexcept
        : u32(u), vec4(v), r(v.x), g(v.y), b(v.z), a(v.w) {}
    constexpr ColorToken(uint8_t r8, uint8_t g8, uint8_t b8, uint8_t a8 = 255) noexcept
        : u32(((static_cast<ImU32>(a8)) << 24) |
              ((static_cast<ImU32>(b8)) << 16) |
              ((static_cast<ImU32>(g8)) << 8)  |
              ((static_cast<ImU32>(r8))))
        , vec4(static_cast<float>(r8) / 255.0f,
               static_cast<float>(g8) / 255.0f,
               static_cast<float>(b8) / 255.0f,
               static_cast<float>(a8) / 255.0f)
        , r(static_cast<float>(r8) / 255.0f)
        , g(static_cast<float>(g8) / 255.0f)
        , b(static_cast<float>(b8) / 255.0f)
        , a(static_cast<float>(a8) / 255.0f) {}

    constexpr operator ImU32() const noexcept { return u32; }
    constexpr operator ImVec4() const noexcept { return vec4; }
};

struct SurfaceTokens {
    ColorToken windowBg;
    ColorToken panelBg;
    ColorToken cardBg;
    ColorToken cardBgHovered;
    ColorToken cardBgSelected;
    ColorToken cardBgBypassed;
    ColorToken cardBgFaulted;
    ColorToken frameBg;
    ColorToken frameBgHovered;
    ColorToken frameBgActive;
    ColorToken popupBg;
    ColorToken modalOverlay;
    ColorToken headerBg;
    ColorToken headerBgHovered;
    ColorToken headerBgActive;
    ColorToken buttonBg;
    ColorToken buttonBgHovered;
    ColorToken buttonBgActive;
    ColorToken tabBg;
    ColorToken tabBgHovered;
    ColorToken tabBgActive;
    ColorToken scrollbarBg;
    ColorToken scrollbarGrab;
    ColorToken scrollbarGrabHovered;
    ColorToken scrollbarGrabActive;
    ColorToken toastBg;
    ColorToken branchABg;
    ColorToken branchAHeader;
    ColorToken branchBBg;
    ColorToken branchBHeader;
    ColorToken combinerBg;
};

struct BorderTokens {
    ColorToken subtle;
    ColorToken strong;
    ColorToken focus;
    ColorToken card;
    ColorToken cardHovered;
    ColorToken cardSelected;
    ColorToken cardBypassed;
    ColorToken cardFaulted;
    ColorToken cardGlowActive;
    ColorToken cardGlowOpen;
    ColorToken separator;
    ColorToken shadow;
    ColorToken branchABorder;
    ColorToken branchBBorder;
    ColorToken combinerBorder;
};

struct TextTokens {
    ColorToken primary;
    ColorToken secondary;
    ColorToken muted;
    ColorToken accent;
    ColorToken inverse;
    ColorToken badgeVst3;
    ColorToken badgeClap;
    ColorToken badgeInternal;
    ColorToken success;
    ColorToken warning;
    ColorToken error;
};

struct SignalStateTokens {
    ColorToken active;
    ColorToken bypassed;
    ColorToken faulted;
    ColorToken recording;
    ColorToken overdubbing;
    ColorToken playing;
    ColorToken stopped;
    ColorToken meterNormal;
    ColorToken meterWarning;
    ColorToken meterClip;
    ColorToken branchA;
    ColorToken branchB;
    ColorToken tunerInTune;
    ColorToken tunerFlat;
    ColorToken tunerSharp;
};

struct CableTokens {
    ColorToken sleeve;
    ColorToken core;
    ColorToken bypassedSleeve;
    ColorToken bypassedCore;
    ColorToken pulseDot;
    ColorToken pulseDotBypassed;
    ColorToken socketRing;
    ColorToken socketPin;
    ColorToken branchASleeve;
    ColorToken branchACore;
    ColorToken branchBSleeve;
    ColorToken branchBCore;
    ColorToken pulseGlow;
};

struct ThemeTokens {
    ThemeId id{ThemeId::ObsidianStudio};
    const char* name{"Obsidian Studio"};
    bool isDark{true};

    SurfaceTokens surfaces;
    BorderTokens borders;
    TextTokens text;
    SignalStateTokens signal;
    CableTokens cables;
};

namespace detail {

inline const ThemeTokens kThemes[static_cast<size_t>(ThemeId::Count)] = {
    // 0: Obsidian Studio
    {
        ThemeId::ObsidianStudio, "Obsidian Studio", true,
        // Surfaces
        {
            ColorToken(18, 19, 22), ColorToken(22, 24, 28), ColorToken(28, 31, 38),
            ColorToken(36, 40, 50), ColorToken(40, 45, 56), ColorToken(20, 22, 27),
            ColorToken(45, 18, 18), ColorToken(34, 38, 46), ColorToken(46, 52, 64),
            ColorToken(56, 64, 78), ColorToken(26, 29, 36, 250), ColorToken(8, 10, 12, 215),
            ColorToken(32, 36, 44), ColorToken(44, 50, 62), ColorToken(54, 62, 76),
            ColorToken(42, 47, 58), ColorToken(56, 63, 78), ColorToken(72, 80, 100),
            ColorToken(26, 29, 35), ColorToken(45, 51, 62), ColorToken(36, 41, 50),
            ColorToken(18, 20, 24), ColorToken(48, 54, 66), ColorToken(64, 72, 88),
            ColorToken(80, 90, 110), ColorToken(24, 26, 32, 235), ColorToken(20, 26, 36, 235),
            ColorToken(26, 34, 48), ColorToken(32, 25, 20, 235), ColorToken(42, 32, 24),
            ColorToken(24, 28, 38, 250)
        },
        // Borders
        {
            ColorToken(46, 52, 64), ColorToken(70, 78, 96), ColorToken(255, 179, 38),
            ColorToken(50, 56, 70), ColorToken(85, 140, 220), ColorToken(255, 179, 38),
            ColorToken(40, 44, 52, 200), ColorToken(235, 64, 52), ColorToken(255, 179, 38, 180),
            ColorToken(52, 199, 89, 220), ColorToken(42, 48, 60), ColorToken(0, 0, 0, 160),
            ColorToken(45, 85, 140, 220), ColorToken(160, 90, 45, 220), ColorToken(65, 95, 145, 220)
        },
        // Text
        {
            ColorToken(242, 244, 248), ColorToken(168, 175, 188), ColorToken(116, 124, 138),
            ColorToken(255, 179, 38), ColorToken(18, 19, 22), ColorToken(65, 185, 255),
            ColorToken(220, 110, 240), ColorToken(255, 179, 38), ColorToken(52, 199, 89),
            ColorToken(255, 179, 38), ColorToken(235, 64, 52)
        },
        // Signal
        {
            ColorToken(52, 199, 89), ColorToken(84, 92, 106), ColorToken(235, 64, 52),
            ColorToken(245, 50, 50), ColorToken(255, 160, 35), ColorToken(52, 199, 89),
            ColorToken(84, 92, 106), ColorToken(52, 199, 89), ColorToken(245, 175, 35),
            ColorToken(245, 50, 50), ColorToken(65, 185, 255), ColorToken(255, 175, 45),
            ColorToken(52, 199, 89), ColorToken(65, 185, 255), ColorToken(255, 175, 45)
        },
        // Cables
        {
            ColorToken(55, 45, 30, 110), ColorToken(255, 185, 45, 240), ColorToken(35, 38, 45, 90),
            ColorToken(90, 96, 110, 200), ColorToken(255, 225, 130), ColorToken(90, 96, 110, 80),
            ColorToken(22, 25, 32), ColorToken(255, 185, 45), ColorToken(30, 70, 120, 110),
            ColorToken(65, 185, 255, 240), ColorToken(120, 60, 25, 110), ColorToken(255, 160, 45, 240),
            ColorToken(255, 179, 38, 120)
        }
    },
    // 1: Cyber / Midnight
    {
        ThemeId::CyberMidnight, "Cyber / Midnight", true,
        // Surfaces
        {
            ColorToken(10, 14, 23), ColorToken(13, 18, 30), ColorToken(18, 25, 42),
            ColorToken(25, 36, 60), ColorToken(30, 44, 74), ColorToken(14, 18, 28),
            ColorToken(42, 14, 26), ColorToken(24, 34, 56), ColorToken(34, 48, 78),
            ColorToken(44, 62, 98), ColorToken(16, 23, 38, 250), ColorToken(5, 7, 14, 220),
            ColorToken(22, 32, 52), ColorToken(32, 46, 74), ColorToken(42, 60, 94),
            ColorToken(30, 44, 72), ColorToken(42, 62, 102), ColorToken(58, 84, 136),
            ColorToken(18, 24, 38), ColorToken(32, 46, 74), ColorToken(26, 38, 62),
            ColorToken(10, 14, 22), ColorToken(36, 52, 84), ColorToken(50, 72, 114),
            ColorToken(66, 94, 148), ColorToken(16, 22, 36, 235), ColorToken(12, 28, 48, 235),
            ColorToken(16, 38, 64), ColorToken(36, 14, 38, 235), ColorToken(48, 18, 50),
            ColorToken(16, 24, 40, 250)
        },
        // Borders
        {
            ColorToken(36, 54, 88), ColorToken(60, 90, 145), ColorToken(0, 240, 255),
            ColorToken(40, 65, 105), ColorToken(0, 240, 255), ColorToken(255, 0, 128),
            ColorToken(30, 42, 65, 200), ColorToken(255, 42, 85), ColorToken(0, 240, 255, 180),
            ColorToken(0, 230, 160, 220), ColorToken(32, 48, 78), ColorToken(0, 0, 0, 180),
            ColorToken(0, 180, 240, 220), ColorToken(220, 30, 140, 220), ColorToken(40, 120, 200, 220)
        },
        // Text
        {
            ColorToken(240, 248, 255), ColorToken(155, 185, 220), ColorToken(100, 125, 155),
            ColorToken(0, 240, 255), ColorToken(10, 14, 23), ColorToken(0, 240, 255),
            ColorToken(255, 0, 128), ColorToken(255, 180, 40), ColorToken(0, 230, 160),
            ColorToken(255, 185, 30), ColorToken(255, 42, 85)
        },
        // Signal
        {
            ColorToken(0, 240, 255), ColorToken(60, 75, 98), ColorToken(255, 42, 85),
            ColorToken(255, 30, 80), ColorToken(255, 160, 20), ColorToken(0, 230, 160),
            ColorToken(60, 75, 98), ColorToken(0, 230, 160), ColorToken(255, 185, 30),
            ColorToken(255, 40, 80), ColorToken(0, 240, 255), ColorToken(255, 0, 128),
            ColorToken(0, 230, 160), ColorToken(0, 240, 255), ColorToken(255, 0, 128)
        },
        // Cables
        {
            ColorToken(15, 45, 70, 120), ColorToken(0, 240, 255, 240), ColorToken(20, 28, 42, 90),
            ColorToken(65, 85, 115, 200), ColorToken(200, 255, 255), ColorToken(65, 85, 115, 80),
            ColorToken(14, 20, 32), ColorToken(0, 240, 255), ColorToken(15, 50, 80, 120),
            ColorToken(0, 240, 255, 240), ColorToken(70, 15, 50, 120), ColorToken(255, 0, 128, 240),
            ColorToken(0, 240, 255, 120)
        }
    },
    // 2: Nordic Slate
    {
        ThemeId::NordicSlate, "Nordic Slate", true,
        // Surfaces
        {
            ColorToken(22, 27, 34), ColorToken(27, 34, 42), ColorToken(34, 43, 54),
            ColorToken(44, 56, 70), ColorToken(50, 64, 80), ColorToken(25, 31, 38),
            ColorToken(48, 24, 26), ColorToken(40, 50, 64), ColorToken(52, 66, 84),
            ColorToken(64, 80, 102), ColorToken(30, 38, 48, 250), ColorToken(12, 16, 20, 215),
            ColorToken(38, 48, 60), ColorToken(50, 64, 80), ColorToken(60, 76, 96),
            ColorToken(48, 62, 78), ColorToken(62, 80, 100), ColorToken(76, 98, 122),
            ColorToken(30, 38, 48), ColorToken(50, 64, 80), ColorToken(42, 54, 68),
            ColorToken(20, 25, 32), ColorToken(54, 68, 86), ColorToken(70, 88, 110),
            ColorToken(88, 110, 138), ColorToken(30, 38, 48, 235), ColorToken(24, 38, 52, 235),
            ColorToken(30, 48, 66), ColorToken(42, 36, 32, 235), ColorToken(54, 46, 40),
            ColorToken(30, 40, 50, 250)
        },
        // Borders
        {
            ColorToken(54, 68, 86), ColorToken(78, 98, 122), ColorToken(88, 184, 216),
            ColorToken(58, 74, 92), ColorToken(88, 184, 216), ColorToken(110, 205, 235),
            ColorToken(45, 56, 68, 200), ColorToken(224, 82, 82), ColorToken(88, 184, 216, 180),
            ColorToken(69, 194, 154, 220), ColorToken(50, 64, 80), ColorToken(0, 0, 0, 160),
            ColorToken(60, 120, 165, 220), ColorToken(165, 115, 75, 220), ColorToken(75, 125, 160, 220)
        },
        // Text
        {
            ColorToken(244, 248, 250), ColorToken(165, 185, 198), ColorToken(115, 132, 145),
            ColorToken(88, 184, 216), ColorToken(22, 27, 34), ColorToken(88, 184, 216),
            ColorToken(185, 145, 225), ColorToken(226, 167, 98), ColorToken(69, 194, 154),
            ColorToken(230, 175, 80), ColorToken(224, 82, 82)
        },
        // Signal
        {
            ColorToken(69, 194, 154), ColorToken(75, 90, 102), ColorToken(224, 82, 82),
            ColorToken(230, 75, 75), ColorToken(226, 167, 98), ColorToken(69, 194, 154),
            ColorToken(75, 90, 102), ColorToken(69, 194, 154), ColorToken(230, 175, 80),
            ColorToken(224, 82, 82), ColorToken(88, 184, 216), ColorToken(226, 167, 98),
            ColorToken(69, 194, 154), ColorToken(88, 184, 216), ColorToken(226, 167, 98)
        },
        // Cables
        {
            ColorToken(28, 48, 60, 110), ColorToken(95, 195, 225, 240), ColorToken(26, 34, 42, 90),
            ColorToken(85, 102, 115, 200), ColorToken(215, 245, 255), ColorToken(85, 102, 115, 80),
            ColorToken(26, 33, 42), ColorToken(95, 195, 225), ColorToken(28, 55, 75, 110),
            ColorToken(88, 184, 216, 240), ColorToken(75, 55, 35, 110), ColorToken(226, 167, 98, 240),
            ColorToken(88, 184, 216, 120)
        }
    },
    // 3: Vintage Console
    {
        ThemeId::VintageConsole, "Vintage Console", false,
        // Surfaces
        {
            ColorToken(236, 230, 218), ColorToken(242, 237, 226), ColorToken(248, 245, 238),
            ColorToken(240, 234, 220), ColorToken(232, 224, 206), ColorToken(228, 222, 210),
            ColorToken(250, 226, 224), ColorToken(226, 218, 202), ColorToken(216, 206, 188),
            ColorToken(204, 192, 172), ColorToken(245, 240, 230, 250), ColorToken(40, 34, 28, 180),
            ColorToken(230, 222, 206), ColorToken(220, 210, 192), ColorToken(208, 196, 176),
            ColorToken(218, 208, 190), ColorToken(206, 194, 174), ColorToken(192, 178, 156),
            ColorToken(228, 220, 204), ColorToken(216, 206, 188), ColorToken(244, 240, 230),
            ColorToken(230, 224, 210), ColorToken(198, 186, 166), ColorToken(180, 166, 144),
            ColorToken(160, 146, 124), ColorToken(244, 238, 226, 240), ColorToken(230, 236, 242, 235),
            ColorToken(215, 226, 236), ColorToken(245, 235, 226, 235), ColorToken(238, 220, 206),
            ColorToken(238, 232, 220, 250)
        },
        // Borders
        {
            ColorToken(196, 184, 164), ColorToken(156, 142, 120), ColorToken(184, 75, 24),
            ColorToken(185, 172, 152), ColorToken(184, 75, 24), ColorToken(150, 60, 18),
            ColorToken(200, 192, 178, 200), ColorToken(195, 45, 35), ColorToken(184, 75, 24, 160),
            ColorToken(38, 135, 68, 200), ColorToken(200, 188, 168), ColorToken(80, 65, 50, 60),
            ColorToken(130, 160, 190, 220), ColorToken(190, 130, 95, 220), ColorToken(160, 150, 135, 220)
        },
        // Text
        {
            ColorToken(36, 28, 22), ColorToken(95, 80, 68), ColorToken(145, 130, 115),
            ColorToken(184, 75, 24), ColorToken(248, 245, 238), ColorToken(30, 105, 165),
            ColorToken(145, 45, 140), ColorToken(184, 75, 24), ColorToken(38, 135, 68),
            ColorToken(184, 75, 24), ColorToken(195, 45, 35)
        },
        // Signal
        {
            ColorToken(38, 135, 68), ColorToken(150, 140, 128), ColorToken(195, 45, 35),
            ColorToken(200, 40, 30), ColorToken(195, 95, 20), ColorToken(38, 135, 68),
            ColorToken(150, 140, 128), ColorToken(38, 135, 68), ColorToken(205, 135, 25),
            ColorToken(200, 40, 30), ColorToken(40, 115, 175), ColorToken(195, 90, 30),
            ColorToken(38, 135, 68), ColorToken(40, 115, 175), ColorToken(195, 90, 30)
        },
        // Cables
        {
            ColorToken(210, 195, 175, 130), ColorToken(180, 80, 30, 240), ColorToken(215, 205, 190, 100),
            ColorToken(155, 145, 132, 200), ColorToken(100, 40, 10), ColorToken(155, 145, 132, 80),
            ColorToken(195, 180, 160), ColorToken(180, 80, 30), ColorToken(195, 210, 225, 130),
            ColorToken(40, 115, 175, 240), ColorToken(225, 205, 190, 130), ColorToken(195, 90, 30, 240),
            ColorToken(184, 75, 24, 120)
        }
    }
};

inline std::atomic<ThemeId> s_activeThemeId{ThemeId::ObsidianStudio};

} // namespace detail

inline const ThemeTokens& getThemeTokens(ThemeId id) noexcept {
    const size_t idx = static_cast<size_t>(id);
    if (idx >= static_cast<size_t>(ThemeId::Count)) {
        return detail::kThemes[0];
    }
    return detail::kThemes[idx];
}

inline const ThemeTokens& themeTokens() noexcept {
    return getThemeTokens(detail::s_activeThemeId.load(std::memory_order_relaxed));
}

inline void applyTheme(ThemeId id) {
    if (static_cast<size_t>(id) >= static_cast<size_t>(ThemeId::Count)) {
        id = ThemeId::ObsidianStudio;
    }
    detail::s_activeThemeId.store(id, std::memory_order_release);
    const ThemeTokens& tokens = getThemeTokens(id);

    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    // Apply standard geometry according to 4px/8px design grid
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;

    style.WindowPadding     = ImVec2(12.0f, 12.0f);
    style.FramePadding      = ImVec2(8.0f, 6.0f);
    style.ItemSpacing       = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 6.0f);
    style.IndentSpacing     = 16.0f;
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 10.0f;

    // Apply color tokens directly to ImGuiStyle Colors
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = tokens.text.primary;
    colors[ImGuiCol_TextDisabled]          = tokens.text.muted;
    colors[ImGuiCol_WindowBg]              = tokens.surfaces.windowBg;
    colors[ImGuiCol_ChildBg]               = tokens.surfaces.panelBg;
    colors[ImGuiCol_PopupBg]               = tokens.surfaces.popupBg;
    colors[ImGuiCol_Border]                = tokens.borders.subtle;
    colors[ImGuiCol_BorderShadow]          = tokens.borders.shadow;
    colors[ImGuiCol_FrameBg]               = tokens.surfaces.frameBg;
    colors[ImGuiCol_FrameBgHovered]        = tokens.surfaces.frameBgHovered;
    colors[ImGuiCol_FrameBgActive]         = tokens.surfaces.frameBgActive;
    colors[ImGuiCol_TitleBg]               = tokens.surfaces.windowBg;
    colors[ImGuiCol_TitleBgActive]         = tokens.surfaces.panelBg;
    colors[ImGuiCol_TitleBgCollapsed]      = tokens.surfaces.windowBg;
    colors[ImGuiCol_MenuBarBg]             = tokens.surfaces.windowBg;
    colors[ImGuiCol_ScrollbarBg]           = tokens.surfaces.scrollbarBg;
    colors[ImGuiCol_ScrollbarGrab]         = tokens.surfaces.scrollbarGrab;
    colors[ImGuiCol_ScrollbarGrabHovered]  = tokens.surfaces.scrollbarGrabHovered;
    colors[ImGuiCol_ScrollbarGrabActive]   = tokens.surfaces.scrollbarGrabActive;
    colors[ImGuiCol_CheckMark]             = tokens.signal.active;
    colors[ImGuiCol_SliderGrab]            = tokens.text.accent;
    colors[ImGuiCol_SliderGrabActive]      = tokens.signal.active;
    colors[ImGuiCol_Button]                = tokens.surfaces.buttonBg;
    colors[ImGuiCol_ButtonHovered]         = tokens.surfaces.buttonBgHovered;
    colors[ImGuiCol_ButtonActive]          = tokens.surfaces.buttonBgActive;
    colors[ImGuiCol_Header]                = tokens.surfaces.headerBg;
    colors[ImGuiCol_HeaderHovered]         = tokens.surfaces.headerBgHovered;
    colors[ImGuiCol_HeaderActive]          = tokens.surfaces.headerBgActive;
    colors[ImGuiCol_Separator]             = tokens.borders.separator;
    colors[ImGuiCol_SeparatorHovered]      = tokens.borders.strong;
    colors[ImGuiCol_SeparatorActive]       = tokens.borders.focus;
    colors[ImGuiCol_ResizeGrip]            = tokens.borders.subtle;
    colors[ImGuiCol_ResizeGripHovered]     = tokens.borders.strong;
    colors[ImGuiCol_ResizeGripActive]      = tokens.borders.focus;
    colors[ImGuiCol_Tab]                   = tokens.surfaces.tabBg;
    colors[ImGuiCol_TabHovered]            = tokens.surfaces.tabBgHovered;
    colors[ImGuiCol_TabActive]             = tokens.surfaces.tabBgActive;
    colors[ImGuiCol_TabUnfocused]          = tokens.surfaces.tabBg;
    colors[ImGuiCol_TabUnfocusedActive]    = tokens.surfaces.tabBgActive;
    colors[ImGuiCol_PlotLines]             = tokens.text.accent;
    colors[ImGuiCol_PlotLinesHovered]      = tokens.signal.active;
    colors[ImGuiCol_PlotHistogram]         = tokens.text.accent;
    colors[ImGuiCol_PlotHistogramHovered]  = tokens.signal.active;
    colors[ImGuiCol_TableHeaderBg]         = tokens.surfaces.headerBg;
    colors[ImGuiCol_TableBorderStrong]     = tokens.borders.strong;
    colors[ImGuiCol_TableBorderLight]      = tokens.borders.subtle;
    colors[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt]         = tokens.surfaces.frameBg;
    colors[ImGuiCol_TextSelectedBg]        = tokens.borders.focus;
    colors[ImGuiCol_DragDropTarget]        = tokens.text.accent;
    colors[ImGuiCol_NavHighlight]          = tokens.borders.focus;
    colors[ImGuiCol_NavWindowingHighlight] = tokens.borders.focus;
    colors[ImGuiCol_NavWindowingDimBg]     = tokens.surfaces.modalOverlay;
    colors[ImGuiCol_ModalWindowDimBg]      = tokens.surfaces.modalOverlay;
}

} // namespace praccy::ui
