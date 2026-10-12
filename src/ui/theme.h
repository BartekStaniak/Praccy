#pragma once

#include "design_tokens.h"

// Forward declaration for Dear ImGui font handles
struct ImFont;

// Global typography handles
extern ImFont* g_fontUI;
extern ImFont* g_fontMono;

namespace praccy::ui {

inline void applyPraccyTheme() {
    applyTheme(ThemeId::ObsidianStudio);
}

// Helpers for visual hierarchy polish and standardized styling
inline ImVec2 getRibbonFramePadding() noexcept {
    return ribbonFramePadding();
}

inline const ColorToken& getActiveSceneBg() noexcept {
    return themeTokens().surfaces.sceneActiveEmerald;
}

inline const ColorToken& getActiveSceneBorder() noexcept {
    return themeTokens().borders.focus;
}

inline const ColorToken& getLooperIdleTextColor() noexcept {
    return themeTokens().text.looperIdle;
}

inline const ColorToken& getMapModeVioletBorder() noexcept {
    return themeTokens().borders.mapModeViolet;
}

inline const ColorToken& getMeterOptimalColor() noexcept {
    return themeTokens().signal.meterOptimal;
}

} // namespace praccy::ui
