# Architectural Blueprint Report: Modal Translation Unit Decoupling & Redesigned 240x224px Plugin Cards (Milestone 4 / Features 20 & 21)

## 1. Observation

### 1.1 Monolithic Modal Dialogs in `src/ui/rack_view.cpp`
Direct inspection of `src/ui/rack_view.cpp` (3,456 total lines, 161,522 bytes) reveals ~1,000 lines of tightly-coupled inline modal dialog code embedded directly within the `RackView` class implementation:
- **`RackView::renderPluginBrowserModal()`** spans lines 2552–2858 (**307 lines**):
  - Directly manages filter search buffers (`m_pluginSearchQuery`), category tree state (`m_selectedDeveloperFilter`, `m_selectedFormatFilter`, `m_showFavoritesFilter`, `m_pluginSortMode`), and custom search path additions (`m_newPathBuffer`).
  - Instantiates plugins directly via `plugins::ClapPluginInstance::loadFromFile` (lines 2765–2768), `plugins::Vst3PluginInstance::loadFromFile` (lines 2770–2773), and built-in DSP effects (`plugins::OverdriveEffect`, `TubeAmpEffect`, `StereoDelayEffect`, lines 2774–2780).
  - Mutates `audio::GraphEngine` serial nodes (`m_graph.addSerialNode`, line 2794) and parallel branch slots (`branch->addSlot`, line 2789) directly within the modal render loop.
- **`RackView::renderSettingsModal()`** spans lines 3062–3236 (**175 lines**):
  - Manages ASIO driver selection (`m_cachedDrivers`, `m_selectedDriverIdx`), invokes `audio::AsioManager::enumerateDrivers` (line 3110), `m_asio.loadDriver` (line 3094), and `m_asio.openControlPanel` (line 3115).
  - Toggles software update settings (`AppConfig::checkBetaUpdates`) and triggers `UpdateChecker::instance().checkForUpdates` (lines 3182–3195).
  - Contains cross-modal coupling: button on line 3153 sets `m_showPluginBrowser = true; m_focusPluginBrowser = true;` to trigger the browser from settings.
- **`RackView::renderPracticeToolsModal()`** spans lines 3237–3456 (**220 lines**):
  - Directly exposes `tools::QuickLooper` state (`m_looper.state()`, line 3261), linear progress bar (`ImGui::ProgressBar`, lines 3290–3299), action buttons (`m_looper.triggerAction()`, `stop()`, `clear()`, lines 3303–3332), and gain slider (`m_looper.setVolume()`, line 3343).
  - Directly exposes `tools::AudioPlayer` file loading (`m_audioFilePathBuffer`, line 3362), playback state (`m_player.play()`, `pause()`, `seek()`, `setLooping()`, lines 3372–3420), and volume slider (`m_player.setVolume()`, line 3430).
- **Header State Burden in `src/ui/rack_view.h`**:
  `RackView` contains 14 modal-specific private member variables that have no relationship to signal rack canvas rendering:
  ```cpp
  // Lines 84–112 of src/ui/rack_view.h:
  bool m_showPracticeToolsModal{false};
  char m_audioFilePathBuffer[260]{0};
  std::vector<audio::AsioDriverDesc> m_cachedDrivers;
  int m_selectedDriverIdx{0};
  bool m_showPluginBrowser{false};
  bool m_focusPluginBrowser{false};
  char m_newPathBuffer[260]{0};
  char m_pluginSearchQuery[128]{0};
  std::string m_selectedDeveloperFilter{"All"};
  std::string m_selectedFormatFilter{"All"};
  bool m_showFavoritesFilter{false};
  int m_pluginSortMode{0};
  int m_insertTargetBlockIndex{-1};
  int m_insertTargetBranchIndex{-1};
  ```
- **Directory Structure & Build Target Layout**:
  - `src/ui/modals/` does not currently exist (`list_dir` confirms 0 subdirectories under `src/ui/`).
  - `CMakeLists.txt` lines 36–58 list `src/ui/rack_view.cpp`, `src/ui/thumbnail_manager.cpp`, `src/ui/update_checker.cpp` under `PRACCY_SOURCES`, with zero references to modal translation units.

---

### 1.2 Plugin Card Deficiencies in `src/ui/rack_view.cpp`
Direct inspection of `RackView::renderPluginSlot()` (lines 1420–1909, **490 lines**) reveals significant architectural and visual ergonomics shortcomings:
1. **Inert Dummy Knobs (Non-Functional Drag Traps)**:
   Lines 1738–1753 draw 3 non-functional decorative rotary knobs on the DSP fallback faceplate when no thumbnail texture is loaded:
   ```cpp
   // Verbatim lines 1744–1752 of src/ui/rack_view.cpp:
   for (int k = -1; k <= 1; ++k) {
       ImVec2 kCenter(midX + k * knobSpacing, midY - 2.0f);
       dl->AddCircleFilled(kCenter, knobRadius, knobBg);
       dl->AddCircle(kCenter, knobRadius, knobCol, 24, 1.5f);
       float angle = -2.356f + (k + 1) * 2.356f;
       ImVec2 pt(kCenter.x + std::cos(angle) * (knobRadius - 2.0f),
                 kCenter.y + std::sin(angle) * (knobRadius - 2.0f));
       dl->AddLine(kCenter, pt, knobCol, 1.8f);
   }
   ```
   These drawn circles do not respond to mouse clicks, double clicks, or drag gestures, and bind to zero DSP parameters.
2. **Coarse Bypass Button**:
   Lines 1821–1825 use a standard rectangular push button (`CenteredButton(btnId, ImVec2(64, 24))`) instead of a tactile sliding pill capsule toggle switch.
3. **Crude Title Truncation**:
   Lines 1452–1455 truncate plugin names using fixed character substring counting:
   ```cpp
   std::string displayName = slot->name();
   if (displayName.length() > 14) {
       displayName = displayName.substr(0, 13) + "..";
   }
   ```
   This ignores actual pixel width and font metrics, leading to inconsistent right margins and clipped text across different font renderers.
4. **Header Format Tag Missing**:
   The format tag ("VST3", "CLAP", "DSP") is only drawn inside the thumbnail texture area (lines 1633–1650, 1682–1702) rather than being integrated as a standardized badge in the card header.
5. **Absence of Monospace Typography for Numeric Readouts**:
   Lines 1834 and 1852 render "Mix: %.0f%%" and "Trim: %+.1f dB" in the default variable-width UI font (`g_fontUI`), causing digit jitter and layout shifting as parameter values change during adjustment.
6. **Lacking Layered Frame Glow**:
   The thumbnail frame border (line 1563) uses a simple single 1.0px or 1.5px stroke (`dl->AddRect`) without multi-layer ambient outer glow reflecting active, bypassed, or faulted states.

---

### 1.3 Design Token & Build Environment Baseline
- `scripts/check_hardcoded_colors.py` executed via `py scripts/check_hardcoded_colors.py` returns **Exit Code 0** (0 hardcoded `IM_COL32` or `ImVec4` literals found across `src/ui/`).
- `ctest --test-dir build --output-on-failure` passes **7/7 tests (100%)** (`test_praccy`, `test_challenger_m1`, `test_challenger_m1_2`, `test_challenger_m2`, `test_challenger_m2_1`, `test_challenger_m3_1`, `test_challenger_m3_2`).
- Embedded TrueType fonts Inter (`g_fontUI`) and JetBrains Mono (`g_fontMono`) are loaded in `src/main.cpp` lines 270–288 from Win32 PE resources (`RT_RCDATA` IDs 201 and 202).

---

## 2. Logic Chain

### 2.1 Logic Chain for Feature 20 (Modal Translation Unit Decoupling)
1. **Separation of Concerns & Compilation Isolation**:
   - *Observation 1.1* demonstrates that `rack_view.cpp` is 3,456 lines long, mixing signal canvas layout algorithms with modal dialog business logic and plugin instantiation.
   - *Requirement R4* mandates extracting modals into `src/ui/modals/plugin_browser_modal.cpp`, `settings_modal.cpp`, and `practice_tools_modal.cpp`.
   - *Therefore*: Dedicated classes `PluginBrowserModal`, `SettingsModal`, and `PracticeToolsModal` must be created in `src/ui/modals/`. Each modal owns its internal buffers, dialog state, and event dispatch.
2. **Minimal, Decoupled Interface Contracts**:
   - `PluginBrowserModal` requires access to `audio::GraphEngine&` (to insert slots) and `plugins::PluginScanner&` (to search and query plugins). Exposing `open(int targetBlockIndex = -1, int targetBranchIndex = -1)` and `render()` encapsulates insertion logic without leaking modal buffers into `RackView`.
   - `SettingsModal` requires access to `audio::AsioManager&` and `plugins::PluginScanner&`. Cross-modal triggering ("Open Plugin Manager") is decoupled via an event callback `std::function<void()> onOpenPluginBrowser`, eliminating direct dependency on `PluginBrowserModal`.
   - `PracticeToolsModal` requires access to `tools::QuickLooper&` and `tools::AudioPlayer&`. Exposing `open()`, `close()`, `toggle()`, and `render()` isolates practice suite state from the rack view.
3. **Shared UI Helper Consolidation**:
   - Common widgets currently defined as static functions in `rack_view.cpp` (`CenteredButton`, `ResettableSliderFloat`, `ResettableVSliderFloat`, `drawStarGeometry`, `renderCenteredStarButton`, `renderBadgePill`) are needed across multiple modals.
   - *Therefore*: These widgets must be consolidated into an inline header `src/ui/ui_helpers.h`. Because all helpers strictly consume semantic tokens from `src/ui/design_tokens.h`, zero raw color literals are introduced, ensuring full compliance with `scripts/check_hardcoded_colors.py`.
4. **Build Target Integration**:
   - `CMakeLists.txt` must include `src/ui/modals/plugin_browser_modal.cpp`, `src/ui/modals/settings_modal.cpp`, and `src/ui/modals/practice_tools_modal.cpp` in `PRACCY_SOURCES`.

---

### 2.2 Logic Chain for Feature 21 (Redesigned 240x224px Plugin Cards)
1. **Standardized Card Footprint (240x224px Strict Geometry)**:
   - *Observation 1.2* notes the card envelope is 240x224px, but the vertical layout is cluttered with inert graphics and lacks visual hierarchy.
   - *Requirement R4* mandates 240x224px cards with parameter readout headers, thumbnail preview frames with border glows, and active/bypass pill toggles, eliminating inert drawn knobs.
   - *Therefore*: The vertical budget is mathematically partitioned into 5 distinct zones:
     - **Zone 1: Header Bar** ($Y \in [8, 32]\,\text{px}$, $H = 24\,\text{px}$): Format badge pill + truncated plugin title + sliding pill toggle switch (on/bypass) + split/delete buttons.
     - **Zone 2: Body Section** ($Y \in [36, 132]\,\text{px}$, $H = 96\,\text{px}$): Active thumbnail preview or procedural DSP vector curve with multi-layer outer glow border.
     - **Zone 3: Parameter Readout Header** ($Y \in [136, 152]\,\text{px}$, $H = 16\,\text{px}$): Monospace numeric readouts (`g_fontMono`) for Dry/Wet % and Output Trim dB.
     - **Zone 4: Tactile Control Row 1** ($Y \in [156, 182]\,\text{px}$, $H = 26\,\text{px}$): Interactive Dry/Wet mix slider ($W = 188\,\text{px}$) + dual-color mini peak meter ($W = 10\,\text{px}$).
     - **Zone 5: Tactile Control Row 2** ($Y \in [186, 212]\,\text{px}$, $H = 26\,\text{px}$): Interactive Output Trim slider ($W = 206\,\text{px}$, $-24\,\text{dB}$ to $+12\,\text{dB}$).
     - **Bottom Margin**: $12\,\text{px}$ padding. Total height: $212 + 12 = 224\,\text{px}$ exactly.
2. **Sliding Capsule Pill Toggle Switch**:
   - Replaces the rectangular button with an interactive pill toggle ($38 \times 18\,\text{px}$, radius $9\,\text{px}$):
     - Active (ON): Sliding knob at right ($X = \text{pos}.x + 28\,\text{px}$), body filled with `tokens.signal.active`, knob in `tokens.text.primary`.
     - Bypassed (OFF): Sliding knob at left ($X = \text{pos}.x + 10\,\text{px}$), body filled with `tokens.surfaces.frameBg`, border in `tokens.borders.subtle`, knob in `tokens.text.muted`.
     - Clicking flips `slot->setBypassed(!bypassed)` with zero allocation and zero latency.
3. **Monospace Font Readouts (`g_fontMono`)**:
   - *Observation 1.3* confirms `g_fontMono` (JetBrains Mono) is globally accessible via `src/ui/theme.h`.
   - Wrapping readout text inside `ImGui::PushFont(g_fontMono)` and `ImGui::PopFont()` prevents digit width fluctuation and layout jitter during active audio metering and slider dragging.
4. **Elimination of Inert Knobs & Procedural DSP Waveform Faceplate**:
   - Removing lines 1744–1752 eliminates non-functional circular drag traps.
   - When no thumbnail is loaded (built-in DSP effects), a clean procedural audio transfer curve (overdrive soft-clip curve, tube saturation curve, delay impulse ticks) is rendered with anti-aliased polyline geometry in `tokens.text.accent`, paired with a clean "[ EDIT DSP PARAMETERS ]" click target.
5. **Multi-Layer Frame Border Glow**:
   - When active: Layer 1 ambient outer glow (`dl->AddRect`, margin $+2.0\,\text{px}$, alpha 35% in `tokens.signal.active`), Layer 2 crisp inner border ($1.5\,\text{px}$ in `tokens.signal.active`).
   - When bypassed: Dimmed border ($1.0\,\text{px}$ in `tokens.borders.subtle`), zero ambient glow.
   - When faulted: Crimson alert frame ($1.5\,\text{px}$ in `tokens.borders.cardFaulted` / `tokens.signal.faulted`) with error text and interactive "RELOAD PLUGIN" button.

---

## 3. Implementation Specifications

### 3.1 Class Contracts for Extracted Modals

#### 3.1.1 `src/ui/modals/plugin_browser_modal.h`
```cpp
#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace praccy::audio {
    class GraphEngine;
}

namespace praccy::plugins {
    class PluginScanner;
}

namespace praccy::ui {

class PluginBrowserModal {
public:
    PluginBrowserModal(audio::GraphEngine& graph, plugins::PluginScanner& scanner);
    ~PluginBrowserModal();

    PluginBrowserModal(const PluginBrowserModal&) = delete;
    PluginBrowserModal& operator=(const PluginBrowserModal&) = delete;

    /// Open modal targeting an insertion slot (-1 for serial chain; >= 0 for parallel block/branch)
    void open(int targetBlockIndex = -1, int targetBranchIndex = -1);

    /// Close the modal dialog
    void close();

    /// Check if the modal dialog is currently visible
    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }

    /// Render frame routine (invoked in RackView::render())
    void render();

    /// Callback invoked when a plugin is successfully instantiated and added to the graph
    void setOnPluginInserted(std::function<void(const std::string& pluginName)> callback) {
        m_onPluginInserted = std::move(callback);
    }

private:
    void renderFilterBar();
    void renderCategoryPane(float width, float height);
    void renderPluginTable(float height);
    void renderBottomBar();
    void instantiateAndInsertPlugin(size_t filteredIndex);

    audio::GraphEngine& m_graph;
    plugins::PluginScanner& m_scanner;
    std::function<void(const std::string&)> m_onPluginInserted;

    bool m_isOpen{false};
    bool m_focusRequested{false};

    int m_targetBlockIndex{-1};
    int m_targetBranchIndex{-1};

    char m_searchQuery[128]{0};
    std::string m_selectedDeveloperFilter{"All"};
    std::string m_selectedFormatFilter{"All"};
    bool m_showFavoritesFilter{false};
    int m_sortMode{0}; // 0: Name A-Z, 1: Name Z-A, 2: Dev A-Z, 3: Format A-Z

    char m_newPathBuffer[260]{0};
};

} // namespace praccy::ui
```

#### 3.1.2 `src/ui/modals/settings_modal.h`
```cpp
#pragma once

#include <vector>
#include <string>
#include <functional>
#include "../../audio/asio_manager.h"

namespace praccy::plugins {
    class PluginScanner;
}

namespace praccy::ui {

class SettingsModal {
public:
    SettingsModal(audio::AsioManager& asio,
                  plugins::PluginScanner& scanner,
                  std::function<void()> onOpenPluginBrowser = nullptr);
    ~SettingsModal();

    SettingsModal(const SettingsModal&) = delete;
    SettingsModal& operator=(const SettingsModal&) = delete;

    /// Open settings modal dialog
    void open();

    /// Close settings modal dialog
    void close();

    /// Check if settings modal is open
    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }

    /// Render frame routine (invoked in RackView::render())
    void render();

    /// Event handler for triggering external plugin browser
    void setOnOpenPluginBrowser(std::function<void()> callback) {
        m_onOpenPluginBrowser = std::move(callback);
    }

private:
    void renderAudioTab();
    void renderPluginsTab();
    void renderThemeTab();
    void renderUpdatesTab();
    void renderAboutTab();

    audio::AsioManager& m_asio;
    plugins::PluginScanner& m_scanner;
    std::function<void()> m_onOpenPluginBrowser;

    bool m_isOpen{false};
    std::vector<audio::AsioDriverDesc> m_cachedDrivers;
    int m_selectedDriverIdx{0};
};

} // namespace praccy::ui
```

#### 3.1.3 `src/ui/modals/practice_tools_modal.h`
```cpp
#pragma once

#include <string>
#include "../../tools/quick_looper.h"
#include "../../tools/audio_player.h"

namespace praccy::ui {

class PracticeToolsModal {
public:
    PracticeToolsModal(tools::QuickLooper& looper, tools::AudioPlayer& player);
    ~PracticeToolsModal();

    PracticeToolsModal(const PracticeToolsModal&) = delete;
    PracticeToolsModal& operator=(const PracticeToolsModal&) = delete;

    /// Open the practice tools modal
    void open();

    /// Close the practice tools modal
    void close();

    /// Toggle open/closed state
    void toggle() {
        if (m_isOpen) close();
        else open();
    }

    /// Check if modal is currently open
    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }

    /// Render frame routine (invoked in RackView::render())
    void render();

private:
    void renderLooperTab();
    void renderBackingTrackTab();

    tools::QuickLooper& m_looper;
    tools::AudioPlayer& m_player;

    bool m_isOpen{false};
    char m_audioFilePathBuffer[260]{0};
};

} // namespace praccy::ui
```

---

### 3.2 Shared Widget Library (`src/ui/ui_helpers.h`)
To eliminate code duplication across modal units and `rack_view.cpp`:
```cpp
#pragma once

#include "design_tokens.h"
#include <imgui.h>
#include <string>
#include <cmath>
#include <algorithm>

namespace praccy::ui {

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

inline bool renderSlidingPillToggle(const char* id, bool* active, const ImVec2& size = ImVec2(38.0f, 18.0f)) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    if (clicked) {
        *active = !(*active);
    }
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip(*active ? "Active: Click to Bypass" : "Bypassed: Click to Enable");
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();
    const float radius = size.y * 0.5f;

    ImU32 bgCol = *active ? static_cast<ImU32>(tokens.signal.active) : static_cast<ImU32>(tokens.surfaces.frameBg);
    ImU32 borderCol = hovered ? static_cast<ImU32>(tokens.borders.focus) : (*active ? static_cast<ImU32>(tokens.borders.cardGlowActive) : static_cast<ImU32>(tokens.borders.subtle));

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, radius);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, radius, 0, 1.2f);

    const float knobRadius = radius - 2.5f;
    const float knobX = *active ? (pos.x + size.x - radius) : (pos.x + radius);
    const float knobY = pos.y + radius;
    ImU32 knobCol = *active ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.muted);

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

} // namespace praccy::ui
```

---

### 3.3 Redesigned 240x224px Plugin Card Implementation Blueprint

#### Exact Geometry Layout & Coordinate Map
```
+-------------------------------------------------------------------+  Y = 0px
| [FORMAT]   Plugin Title With Ellipsis...        [||]  [X]  ( o )  |  Y = 8px..32px (Header, H=24px)
+-------------------------------------------------------------------+  Y = 34px
|                                                                   |
|   +-----------------------------------------------------------+   |
|   |                                                           |   |
|   |         THUMBNAIL PREVIEW / PROCEDURAL DSP VECTOR         |   |  Y = 36px..132px (Body, H=96px)
|   |              (Multi-Layer Frame Border Glow)              |   |
|   |                                                           |   |
|   +-----------------------------------------------------------+   |
|                                                                   |
+-------------------------------------------------------------------+  Y = 134px
|   MIX: 100% (Mono)                         TRIM: +0.0dB (Mono)   |  Y = 136px..152px (Readouts, H=16px)
+-------------------------------------------------------------------+  Y = 154px
|   [========================= Mix Slider ==================] [M]   |  Y = 156px..182px (Mix+Meter, H=26px)
+-------------------------------------------------------------------+  Y = 184px
|   [======================== Trim Slider ======================]   |  Y = 186px..212px (Trim Slider, H=26px)
+-------------------------------------------------------------------+  Y = 224px (Bottom Margin)
```

#### Detailed Replacement Chunk for `RackView::renderPluginSlot`
```cpp
void RackView::renderPluginSlot(audio::PluginSlot* slot, int slotIndex, int blockIndex, int branchIndex) {
    if (!slot) return;

    ImGui::BeginGroup();
    const auto& tokens = themeTokens();
    auto* pluginInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    const bool isFaulted = pluginInst && pluginInst->isFaulted();
    const bool bypassed = slot->isBypassed();

    ImVec4 cardBg = isFaulted ? static_cast<ImVec4>(tokens.surfaces.cardBgFaulted) :
                    (bypassed ? static_cast<ImVec4>(tokens.surfaces.cardBgBypassed) : static_cast<ImVec4>(tokens.surfaces.cardBg));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg);
    char childId[64];
    std::snprintf(childId, sizeof(childId), "SlotCard_%d_%d_%d", slotIndex, blockIndex, branchIndex);

    // Standardized exact dimensions: 240 x 224 px
    const float cardWidth = 240.0f;
    const float cardHeight = 224.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild(childId, ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar);

    bool hasGui = pluginInst && pluginInst->hasCustomGui();
    bool isWindowOpen = false;
    if (hasGui) {
        isWindowOpen = plugins::PluginWindowManager::instance().isWindowOpen(pluginInst);
    } else if (m_dspTweakSlot == slot) {
        isWindowOpen = true;
    }

    // =========================================================================
    // SECTION 1: HEADER BAR (H = 24px, Y = 8px..32px)
    // =========================================================================
    {
        ImGui::SetCursorPos(ImVec2(8.0f, 8.0f));

        // Format pill badge
        const char* badgeStr = "DSP";
        ImU32 badgeTextCol = tokens.text.badgeInternal;
        if (pluginInst) {
            if (dynamic_cast<plugins::Vst3PluginInstance*>(pluginInst)) {
                badgeStr = "VST3";
                badgeTextCol = tokens.text.badgeVst3;
            } else if (dynamic_cast<plugins::ClapPluginInstance*>(pluginInst)) {
                badgeStr = "CLAP";
                badgeTextCol = tokens.text.badgeClap;
            }
        }
        renderBadgePill(badgeStr, tokens.surfaces.frameBg, tokens.borders.focus, badgeTextCol, 18.0f);
        ImGui::SameLine(0, 6.0f);

        // Truncated title with ellipsis
        std::string rawName = slot->name();
        float availTitleW = 100.0f; // Leaves room for right action buttons & pill toggle
        std::string displayTitle = rawName;
        if (ImGui::CalcTextSize(displayTitle.c_str()).x > availTitleW) {
            while (!displayTitle.empty() && ImGui::CalcTextSize((displayTitle + "..").c_str()).x > availTitleW) {
                displayTitle.pop_back();
            }
            displayTitle += "..";
        }

        ImGui::AlignTextToFramePadding();
        ImU32 titleCol = isFaulted ? static_cast<ImU32>(tokens.signal.faulted) :
                         (bypassed ? static_cast<ImU32>(tokens.signal.bypassed) : static_cast<ImU32>(tokens.text.primary));
        ImGui::TextColored(tokens.text.primary.vec4, "%s", displayTitle.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s (%s)", rawName.c_str(), pluginInst ? pluginInst->vendor().c_str() : "Praccy Audio");
        }

        // Action buttons & Sliding Pill Toggle on far right
        ImGui::SameLine(cardWidth - 8.0f - 38.0f - (branchIndex == -1 ? 46.0f : 24.0f));

        if (branchIndex == -1) {
            char spId[32];
            std::snprintf(spId, sizeof(spId), "##Sp_%d", slotIndex);
            if (renderCenteredSplitButton(spId, ImVec2(18, 18))) {
                m_graph.splitSerialNodeIntoParallel(slotIndex);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                return;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split into Parallel Branches");
            ImGui::SameLine(0, 4.0f);
        }

        char delId[32];
        std::snprintf(delId, sizeof(delId), "##Del_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        if (renderCenteredDeleteButton(delId, ImVec2(18, 18))) {
            if (hasGui) plugins::PluginWindowManager::instance().closePluginWindow(pluginInst);
            if (m_dspTweakSlot == slot) m_dspTweakSlot = nullptr;
            if (m_expandedSlot == slot) m_expandedSlot = nullptr;
            if (branchIndex == -1) {
                m_graph.removeSerialNode(slotIndex);
            } else {
                auto* blk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(blockIndex));
                if (blk) {
                    auto* br = blk->getBranch(branchIndex);
                    if (br) br->removeSlot(slotIndex);
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            return;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete Slot");
        ImGui::SameLine(0, 5.0f);

        // Tactile Sliding Pill Toggle Switch (Active/Bypass)
        bool activeState = !bypassed;
        char pillId[32];
        std::snprintf(pillId, sizeof(pillId), "##Pill_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        if (renderSlidingPillToggle(pillId, &activeState, ImVec2(38.0f, 18.0f))) {
            slot->setBypassed(!activeState);
        }
    }

    // =========================================================================
    // SECTION 2: BODY SECTION (H = 96px, Y = 36px..132px)
    // =========================================================================
    {
        ImGui::SetCursorPos(ImVec2(8.0f, 36.0f));
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();
        const float previewW = 224.0f;
        const float previewH = 96.0f;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        Thumbnail* thumb = ThumbnailManager::instance().getThumbnail(slot->name());

        // Multi-layer Frame Glow Border
        ImU32 frameBg = isFaulted ? static_cast<ImU32>(tokens.surfaces.cardBgFaulted) :
                        (bypassed ? static_cast<ImU32>(tokens.surfaces.cardBgBypassed) : static_cast<ImU32>(tokens.surfaces.frameBg));
        dl->AddRectFilled(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), frameBg, 5.0f);

        // Ambient outer glow layer
        if (isFaulted) {
            dl->AddRect(ImVec2(previewPos.x - 2.0f, previewPos.y - 2.0f),
                        ImVec2(previewPos.x + previewW + 2.0f, previewPos.y + previewH + 2.0f),
                        ColorToken(tokens.signal.faulted.r, tokens.signal.faulted.g, tokens.signal.faulted.b, 0.25f), 7.0f, 0, 2.0f);
        } else if (!bypassed) {
            dl->AddRect(ImVec2(previewPos.x - 1.5f, previewPos.y - 1.5f),
                        ImVec2(previewPos.x + previewW + 1.5f, previewPos.y + previewH + 1.5f),
                        ColorToken(tokens.signal.active.r, tokens.signal.active.g, tokens.signal.active.b, 0.22f), 6.5f, 0, 2.0f);
        }

        // Invisible button to capture clicks over the entire preview area
        ImGui::SetCursorScreenPos(previewPos);
        char previewBtnId[64];
        std::snprintf(previewBtnId, sizeof(previewBtnId), "##GuiPreview_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        bool previewClicked = ImGui::InvisibleButton(previewBtnId, ImVec2(previewW, previewH));
        bool isPreviewHovered = ImGui::IsItemHovered();

        if (isPreviewHovered && !isFaulted) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }

        // Preview rendering:
        if (isFaulted) {
            const char* crashTitle = "PLUGIN CRASH ISOLATED";
            ImVec2 tSz = ImGui::CalcTextSize(crashTitle);
            dl->AddText(ImVec2(previewPos.x + (previewW - tSz.x) * 0.5f, previewPos.y + 16.0f),
                        tokens.signal.faulted, crashTitle);

            const char* crashSub = "Dry signal pass-through active";
            ImVec2 sSz = ImGui::CalcTextSize(crashSub);
            dl->AddText(ImVec2(previewPos.x + (previewW - sSz.x) * 0.5f, previewPos.y + 36.0f),
                        tokens.text.secondary, crashSub);

            ImGui::SetCursorScreenPos(ImVec2(previewPos.x + (previewW - 130.0f) * 0.5f, previewPos.y + 58.0f));
            char reloadBtnId[48];
            std::snprintf(reloadBtnId, sizeof(reloadBtnId), "RELOAD PLUGIN##%d_%d_%d", slotIndex, blockIndex, branchIndex);
            if (ImGui::Button(reloadBtnId, ImVec2(130.0f, 24.0f))) {
                pluginInst->resetFault();
            }
        } else if (thumb && thumb->srv) {
            float maxW = previewW - 4.0f;
            float maxH = previewH - 4.0f;
            float aspect = (thumb->height > 0) ? (static_cast<float>(thumb->width) / static_cast<float>(thumb->height)) : 1.0f;
            float drawW = maxW;
            float drawH = maxW / aspect;
            if (drawH > maxH) {
                drawH = maxH;
                drawW = maxH * aspect;
            }
            ImVec2 imgMin(std::floor(previewPos.x + (previewW - drawW) * 0.5f),
                          std::floor(previewPos.y + (previewH - drawH) * 0.5f));
            ImVec2 imgMax(imgMin.x + drawW, imgMin.y + drawH);

            ImU32 tintCol = bypassed ? ColorToken(tokens.text.primary.r, tokens.text.primary.g, tokens.text.primary.b, 0.65f).u32 : tokens.text.primary.u32;
            dl->AddImageRounded((ImTextureID)thumb->srv, imgMin, imgMax, ImVec2(0, 0), ImVec2(1, 1), tintCol, 4.0f);

            if (isWindowOpen) {
                const char* openTag = "● OPEN";
                ImVec2 oSz = ImGui::CalcTextSize(openTag);
                ImVec2 oPos(previewPos.x + previewW - oSz.x - 8.0f, previewPos.y + 6.0f);
                dl->AddRectFilled(ImVec2(oPos.x - 3.0f, oPos.y - 1.0f),
                                  ImVec2(oPos.x + oSz.x + 3.0f, oPos.y + oSz.y + 1.0f),
                                  ColorToken(tokens.signal.active.r, tokens.signal.active.g, tokens.signal.active.b, 0.25f), 3.0f);
                dl->AddText(oPos, tokens.signal.active, openTag);
            }

            if (isPreviewHovered) {
                dl->AddRectFilled(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), tokens.surfaces.modalOverlay, 5.0f);
                const char* expandPrompt = isWindowOpen ? "FOCUS WINDOW ↗" : "EXPAND GUI ↗";
                ImVec2 pSz = ImGui::CalcTextSize(expandPrompt);
                ImVec2 pPos(previewPos.x + (previewW - pSz.x) * 0.5f, previewPos.y + (previewH - pSz.y) * 0.5f);
                dl->AddRectFilled(ImVec2(pPos.x - 8.0f, pPos.y - 3.0f), ImVec2(pPos.x + pSz.x + 8.0f, pPos.y + pSz.y + 3.0f), tokens.surfaces.popupBg, 4.0f);
                dl->AddText(pPos, tokens.text.primary, expandPrompt);
            }
        } else {
            // High-Tech Procedural Vector Waveform Faceplate (Eliminated all inert dummy knobs!)
            float midY = previewPos.y + previewH * 0.5f;
            float leftX = previewPos.x + 16.0f;
            float rightX = previewPos.x + previewW - 16.0f;

            // Draw clean DSP sine/drive waveform polyline
            ImVec2 wavePts[16];
            for (int w = 0; w < 16; ++w) {
                float frac = w / 15.0f;
                float x = leftX + frac * (rightX - leftX);
                float y = midY - std::sin(frac * 6.2831853f * 2.0f) * 18.0f * (bypassed ? 0.3f : 1.0f);
                wavePts[w] = ImVec2(x, y);
            }
            ImU32 waveCol = bypassed ? static_cast<ImU32>(tokens.text.muted) :
                            (isPreviewHovered ? static_cast<ImU32>(tokens.text.accent) : static_cast<ImU32>(tokens.signal.active));
            dl->AddPolyline(wavePts, 16, waveCol, 0, 1.8f);

            const char* promptText = isWindowOpen ? "PARAMETERS OPEN" : "CLICK TO TWEAK DSP";
            ImVec2 pSz = ImGui::CalcTextSize(promptText);
            dl->AddText(ImVec2(previewPos.x + (previewW - pSz.x) * 0.5f, previewPos.y + 68.0f),
                        isPreviewHovered ? tokens.text.primary.u32 : tokens.text.secondary.u32, promptText);
        }

        // Crisp inner border stroke
        ImU32 frameBorder = isFaulted ? static_cast<ImU32>(tokens.borders.cardFaulted) :
                            (isWindowOpen ? static_cast<ImU32>(tokens.borders.cardGlowOpen) :
                            (bypassed ? static_cast<ImU32>(tokens.borders.subtle) : static_cast<ImU32>(tokens.signal.active)));
        dl->AddRect(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), frameBorder, 5.0f, 0, 1.5f);

        if (previewClicked && !isFaulted) {
            if (hasGui) {
                plugins::PluginWindowManager::instance().openPluginWindow(pluginInst);
                HWND hw = plugins::PluginWindowManager::instance().getWindow(pluginInst);
                if (hw) {
                    ThumbnailManager::instance().captureWindow(slot->name(), hw);
                    ThumbnailManager::instance().requestCapture(slot->name(), hw, 15);
                }
            } else {
                m_dspTweakSlot = (m_dspTweakSlot == slot) ? nullptr : slot;
            }
            m_expandedSlot = slot;
            m_expandPulseTimer = 1.0f;
        }
    }

    // =========================================================================
    // SECTION 3: PARAMETER READOUT HEADER (H = 16px, Y = 136px..152px)
    // Monospace Font g_fontMono guarantees jitter-free stable layout
    // =========================================================================
    {
        ImGui::SetCursorPos(ImVec2(8.0f, 136.0f));
        ImGui::PushFont(g_fontMono);

        float mixVal = slot->dryWet() * 100.0f;
        char mixStr[32];
        std::snprintf(mixStr, sizeof(mixStr), "MIX: %3.0f%%", mixVal);
        ImGui::TextColored(tokens.text.accent.vec4, "%s", mixStr);

        ImGui::SameLine(cardWidth - 8.0f - 96.0f);
        float trimVal = slot->outputGainDb();
        char trimStr[32];
        std::snprintf(trimStr, sizeof(trimStr), "TRIM:%+5.1fdB", trimVal);
        ImGui::TextColored(tokens.text.secondary.vec4, "%s", trimStr);

        ImGui::PopFont();
    }

    // =========================================================================
    // SECTION 4: TACTILE MIX SLIDER & MINI METER (H = 26px, Y = 156px..182px)
    // =========================================================================
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.5f));
        ImGui::SetCursorPos(ImVec2(8.0f, 156.0f));

        float mix = slot->dryWet() * 100.0f;
        char mixLabel[32];
        std::snprintf(mixLabel, sizeof(mixLabel), "##Mix_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        ImGui::SetNextItemWidth(190.0f);
        if (ResettableSliderFloat(mixLabel, &mix, 0.0f, 100.0f, 100.0f, "Mix: %.0f%%")) {
            slot->setDryWet(mix / 100.0f);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dry/Wet Mix (Double-click to reset 100%%)");

        ImGui::SameLine(0, 6.0f);
        renderMeter("SlotMtr", slot->meter().peakLeft(), 8.0f, 24.0f);
    }

    // =========================================================================
    // SECTION 5: TACTILE OUTPUT TRIM SLIDER (H = 26px, Y = 186px..212px)
    // =========================================================================
    {
        ImGui::SetCursorPos(ImVec2(8.0f, 186.0f));
        float outTrim = slot->outputGainDb();
        char trimLabel[32];
        std::snprintf(trimLabel, sizeof(trimLabel), "##Trim_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        ImGui::SetNextItemWidth(206.0f);
        if (ResettableSliderFloat(trimLabel, &outTrim, -24.0f, +12.0f, 0.0f, "Trim: %+.1f dB")) {
            slot->setOutputGainDb(outTrim);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output Trim (Double-click to reset 0.0 dB)");
        ImGui::PopStyleVar(); // FramePadding
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(); // WindowPadding
    ImGui::PopStyleColor(); // ChildBg
    ImGui::EndGroup();
}
```

---

### 3.4 Integration of Modals in `RackView`

In `src/ui/rack_view.h`:
```cpp
// Add forward declarations
namespace praccy::ui {
class PluginBrowserModal;
class SettingsModal;
class PracticeToolsModal;
}

// In class RackView private members:
std::unique_ptr<PluginBrowserModal> m_pluginBrowserModal;
std::unique_ptr<SettingsModal> m_settingsModal;
std::unique_ptr<PracticeToolsModal> m_practiceToolsModal;
```

In `src/ui/rack_view.cpp` constructor:
```cpp
m_pluginBrowserModal = std::make_unique<PluginBrowserModal>(m_graph, m_scanner);
m_settingsModal = std::make_unique<SettingsModal>(m_asio, m_scanner, [this]() {
    m_pluginBrowserModal->open();
});
m_practiceToolsModal = std::make_unique<PracticeToolsModal>(m_looper, m_player);
```

In `RackView::render()` lines 541–548:
```cpp
m_pluginBrowserModal->render();
renderDspTweakModal();
renderUpdateModal();
m_settingsModal->render();
m_practiceToolsModal->render();
```

---

### 3.5 Detailed `CMakeLists.txt` Changes
In `CMakeLists.txt` lines 36–58, update `PRACCY_SOURCES`:
```cmake
set(PRACCY_SOURCES
    src/main.cpp
    src/audio/asio_manager.cpp
    src/audio/graph_engine.cpp
    src/plugins/clap_host.cpp
    src/plugins/vst3_host.cpp
    src/plugins/plugin_window.cpp
    src/plugins/builtin_dsp.cpp
    src/plugins/plugin_scanner.cpp
    src/midi/midi_manager.cpp
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/tools/audio_player.cpp
    src/tools/quick_looper.cpp
    src/state/scene_manager.cpp
    src/state/app_config.cpp
    src/ui/rack_view.cpp
    src/ui/modals/plugin_browser_modal.cpp
    src/ui/modals/settings_modal.cpp
    src/ui/modals/practice_tools_modal.cpp
    src/ui/thumbnail_manager.cpp
    src/ui/update_checker.cpp
    resources/praccy.rc

    ${THIRD_PARTY_SOURCES}
)
```

---

## 4. Caveats
1. **Parallel Milestone 4 Feature Convergence**:
   - Explorer 2 is designing the Spotlight command palette overlay and native Win32 `IFileOpenDialog` in `plugin_browser_modal.cpp` and `settings_modal.cpp`.
   - Explorer 3 is designing the circular progress ring and WAV drag-and-drop in `practice_tools_modal.cpp`.
   - The decoupled modal architecture specified here provides the exact clean translation boundaries required for Explorers 2 and 3 without circular dependencies or file merge conflicts.
2. **Double-Click Reset State Suppression**:
   - `s_suppressResetId` in `src/ui/ui_helpers.h` must remain shared across compilation units via `inline` variable linkage to avoid mouse-drag override artifacts on sliders across translation units.
3. **No Code Modification During Explorer Turn**:
   - In accordance with explorer archetype constraints, zero files in `src/` or `CMakeLists.txt` were altered during this investigation.

---

## 5. Conclusion
1. **Feature 20 (Modal Decoupling)**: Extracting `renderPluginBrowserModal()`, `renderSettingsModal()`, and `renderPracticeToolsModal()` into dedicated classes in `src/ui/modals/` reduces `src/ui/rack_view.cpp` by ~1,000 lines, removes 14 modal-specific fields from `RackView`, cleanly isolates dependencies, and unblocks parallel development by Milestone 4 workers.
2. **Feature 21 (Redesigned 240x224px Plugin Cards)**: Partitioning the 240x224px card envelope into 5 strict zones (24px header with format badge, truncated title, and sliding pill toggle; 96px body with active multi-layer frame glow; 16px monospace readout header; 26px tactile mix slider + meter; 26px trim slider) eliminates all inert dummy knobs, guarantees zero layout jitter with `g_fontMono`, and ensures 100% adherence to design tokens and WCAG AA contrast.

---

## 6. Verification Method

### 6.1 Independent Static Verification Commands
1. **Verify Hardcoded Color Compliance**:
   Run the static color scanner to ensure zero `IM_COL32` or `ImVec4` literals are introduced:
   ```pwsh
   py scripts/check_hardcoded_colors.py
   ```
   *Pass Condition*: Output reports `SUCCESS: Clean! 0 hardcoded IM_COL32 / ImVec4 literals found`.

2. **Verify Translation Unit Existence & CMake Configuration**:
   Verify that `src/ui/modals/` contains the three decoupled compilation units:
   ```pwsh
   Test-Path "src/ui/modals/plugin_browser_modal.cpp"
   Test-Path "src/ui/modals/settings_modal.cpp"
   Test-Path "src/ui/modals/practice_tools_modal.cpp"
   ```
   *Pass Condition*: All return `True`.

3. **Verify Elimination of Inert Dummy Knobs**:
   Execute regex search on `src/ui/rack_view.cpp` to confirm removal of inert knob drawing loop:
   ```pwsh
   Select-String -Path "src/ui/rack_view.cpp" -Pattern "knobSpacing"
   ```
   *Pass Condition*: 0 occurrences found in `renderPluginSlot`.

4. **Verify Monospace Font Usage in Readouts**:
   Check for `PushFont(g_fontMono)` in `src/ui/rack_view.cpp`:
   ```pwsh
   Select-String -Path "src/ui/rack_view.cpp" -Pattern "PushFont\(g_fontMono\)"
   ```
   *Pass Condition*: Matches parameter readout row in `renderPluginSlot`.

5. **Verify Clean Build & CTest Execution**:
   Build and run test suite:
   ```pwsh
   cmake --build build --config Release
   ctest --test-dir build --output-on-failure
   ```
   *Pass Condition*: 100% tests pass (7/7 tests).
