#include <cstdint>
#include <atomic>
#include <imgui.h>

namespace praccy::ui {

enum class ThemeId : uint8_t {
    ObsidianStudio = 0,
    CyberMidnight = 1,
    NordicSlate = 2,
    VintageConsole = 3,
    Count = 4
};

struct ColorToken {
    ImU32 u32{0};
    ImVec4 vec4{0.0f, 0.0f, 0.0f, 0.0f};

    constexpr ColorToken() = default;
    constexpr ColorToken(ImU32 u, ImVec4 v) : u32(u), vec4(v) {}
    constexpr ColorToken(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
        : u32(((static_cast<ImU32>(a)) << 24) |
              ((static_cast<ImU32>(b)) << 16) |
              ((static_cast<ImU32>(g)) << 8)  |
              ((static_cast<ImU32>(r))))
        , vec4(static_cast<float>(r) / 255.0f,
               static_cast<float>(g) / 255.0f,
               static_cast<float>(b) / 255.0f,
               static_cast<float>(a) / 255.0f) {}

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

const ThemeTokens& getThemeTokens(ThemeId id) noexcept;
const ThemeTokens& themeTokens() noexcept;
void applyTheme(ThemeId id);

} // namespace praccy::ui

int main() {
    return 0;
}
