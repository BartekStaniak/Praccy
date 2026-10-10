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

} // namespace praccy::ui
