#pragma once

#include <imgui.h>
#include "design_tokens.h"
#include <algorithm>

namespace praccy::ui {

/**
 * @brief Centralized UI Design Specification and Standardisation System.
 * All design variables, heights, paddings, roundings, and vertical alignment
 * metrics are declared here so that editing any metric updates the entire application.
 */
namespace Spec {

    // =========================================================================
    // 1. Core Widget Heights (Dynamic DPI-aware & Standard constants)
    // =========================================================================
    inline float buttonHeight()        { return ImGui::GetFrameHeight(); }
    inline float inputHeight()         { return ImGui::GetFrameHeight(); }
    inline float comboHeight()         { return ImGui::GetFrameHeight(); }
    inline float pillHeight()          { return ImGui::GetFrameHeight(); }
    inline float defaultWidgetHeight() { return ImGui::GetFrameHeight(); }
    inline float smallButtonHeight()   { return ui::px(24.0f); }

    inline constexpr float kButtonHeight       = 29.0f; // Matches ImGui::GetFrameHeight() at standard DPI
    inline constexpr float kSmallButtonHeight  = 24.0f; // Inline micro actions (cards/headers)
    inline constexpr float kInputHeight        = 29.0f;
    inline constexpr float kPillHeight         = 29.0f;
    inline constexpr float kFooterHeight       = 44.0f;

    // =========================================================================
    // 2. Padding and Spacing Metrics
    // =========================================================================
    inline constexpr float kPillPadX           = 10.0f;
    inline constexpr float kModalWindowPadding = 18.0f;
    inline constexpr float kCardPaddingX       = 14.0f;
    inline constexpr float kCardPaddingY       = 12.0f;
    inline constexpr float kItemSpacingX       = 8.0f;
    inline constexpr float kItemSpacingY       = 8.0f;

    // =========================================================================
    // 3. Corner Rounding Radiuses
    // =========================================================================
    inline constexpr float kCornerRadius       = 6.0f;
    inline constexpr float kCardCornerRadius   = 6.0f;
    inline constexpr float kButtonCornerRadius = 4.0f;
    inline constexpr float kModalCornerRadius  = 8.0f;
    inline float pillCornerRadius(float height = 0.0f) {
        float h = (height > 0.0f) ? height : pillHeight();
        return h * 0.5f;
    }

    // =========================================================================
    // 4. Standardised Table Metrics & Alignment
    // =========================================================================
    inline constexpr float kTableCellPaddingX  = 8.0f;
    inline constexpr float kTableCellPaddingY  = 4.0f;
    inline ImVec2 tableCellPadding()           { return ImVec2(ui::px(kTableCellPaddingX), ui::px(kTableCellPaddingY)); }

    // Standard row height for tables containing standard controls (combos, buttons, pills)
    inline float tableRowHeight()              { return ui::px(30.0f); } // Standard table row height (compact, fits badges/icons)
    inline float tableCompactRowHeight()       { return ui::px(24.0f); } // Compact table row height (hotkeys, etc.)
    inline float tableBadgeHeight()            { return ui::px(20.0f); }
    inline constexpr float kTableBadgeHeight   = 20.0f;

    /**
     * @brief Vertically centers an item (text, button, pill) within a table cell of the given row height.
     * Call this at the start of a column before rendering content.
     */
    inline void alignTableCellCenter(float itemHeight = 0.0f, float rowHeight = 0.0f) {
        if (itemHeight <= 0.0f) {
            itemHeight = ImGui::GetFontSize();
        }
        if (rowHeight <= 0.0f) {
            rowHeight = tableRowHeight();
        }
        const float cellAvailH = rowHeight - ImGui::GetStyle().CellPadding.y * 2.0f;
        if (cellAvailH > itemHeight) {
            const float extra = (cellAvailH - itemHeight) * 0.5f;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + extra);
        }
    }

} // namespace Spec

// Backwards-compatible alias namespace Metrics -> Spec
namespace Metrics {
    inline float buttonHeight()        { return Spec::buttonHeight(); }
    inline float inputHeight()         { return Spec::inputHeight(); }
    inline float defaultWidgetHeight() { return Spec::defaultWidgetHeight(); }
    inline float pillHeight()          { return Spec::pillHeight(); }
    inline float smallButtonHeight()   { return Spec::smallButtonHeight(); }
    inline float tableRowHeight()      { return Spec::tableRowHeight(); }
    inline float tableCompactRowHeight(){ return Spec::tableCompactRowHeight(); }
    inline float tableBadgeHeight()    { return Spec::tableBadgeHeight(); }
    inline ImVec2 tableCellPadding()   { return Spec::tableCellPadding(); }
    inline void alignTableCellCenter(float itemH = 0.0f, float rowH = 0.0f) { Spec::alignTableCellCenter(itemH, rowH); }

    inline constexpr float kButtonHeight       = Spec::kButtonHeight;
    inline constexpr float kSmallButtonHeight  = Spec::kSmallButtonHeight;
    inline constexpr float kInputHeight        = Spec::kInputHeight;
    inline constexpr float kPillHeight         = Spec::kPillHeight;
    inline constexpr float kTableBadgeHeight   = Spec::kTableBadgeHeight;
    inline constexpr float kModalWindowPadding = Spec::kModalWindowPadding;
    inline constexpr float kCardPaddingX       = Spec::kCardPaddingX;
    inline constexpr float kCardPaddingY       = Spec::kCardPaddingY;
    inline constexpr float kItemSpacingX       = Spec::kItemSpacingX;
    inline constexpr float kItemSpacingY       = Spec::kItemSpacingY;
    inline constexpr float kFooterHeight       = Spec::kFooterHeight;
    inline constexpr float kCornerRadius       = Spec::kCornerRadius;
}

} // namespace praccy::ui
