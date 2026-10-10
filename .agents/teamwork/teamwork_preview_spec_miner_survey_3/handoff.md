# Specification Mining Report: UI Theming, Responsive Canvas, and Practice Suite Overhaul (Requirements R3, R4)

## Observation

1. **Hardcoded Color Usage in `src/ui/`**:
   - `src/ui/theme.h` lines 7–55 defines a single static function `applyPraccyTheme()` with fixed float colors in `style.Colors` (e.g. `colors[ImGuiCol_WindowBg] = ImVec4(0.11f, 0.12f, 0.14f, 1.00f)`). There is no token struct, no dynamic theme switching mechanism, and no support for alternate themes.
   - Exact count: `Select-String -Path "src/ui/rack_view.cpp" -Pattern "IM_COL32\("` returns **170** instances of raw `IM_COL32(...)` literals.
   - Examples of raw color usage in `src/ui/rack_view.cpp`:
     - Line 156: `IM_COL32(40, 48, 62, 220)`
     - Line 165: `IM_COL32(230, 175, 35, 255)`, `IM_COL32(255, 220, 75, 255)`
     - Line 184–189: `IM_COL32(35, 65, 105, 255)`, `IM_COL32(80, 130, 195, 255)`, `IM_COL32(245, 250, 255, 255)`
     - Line 216–221: `IM_COL32(165, 45, 45, 255)`, `IM_COL32(230, 90, 90, 255)`
     - Line 307–315: `IM_COL32(40, 95, 160, 90)`, `IM_COL32(110, 175, 255, 230)`
     - Line 501–511: `IM_COL32(250, 150, 40, 35)`, `IM_COL32(250, 155, 45, 230)`, `IM_COL32(255, 160, 45, 255)`
   - No `scripts/check_hardcoded_colors.py` exists in the repository.

2. **Typography and Windows Resource Architecture**:
   - `resources/praccy.rc` lines 1–6 contains only:
     ```rc
     #define IDI_APP_ICON 101
     IDI_APP_ICON ICON "icon.ico"
     1            ICON "icon.ico"
     ```
   - In `src/main.cpp` lines 88–109:
     ```cpp
     if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
         io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, &fontConfig, glyphRanges);
         ...
     } else {
         io.Fonts->AddFontDefault();
     }
     ```
     The host attempts to read Segoe UI directly from the host Windows directory. No custom font binaries (Inter or JetBrains Mono) are bundled or embedded into the binary via `RT_RCDATA`.

3. **Rack View Layout & Cable Math in `src/ui/rack_view.cpp`**:
   - `RackView::renderSignalRack()` lines 1017–1136 hardcodes static coordinates:
     - `float centerY = 120.0f;` (or `322.0f` if parallel blocks exist)
     - `float serialCardY = 8.0f;` (or `centerY - (cardH * 0.5f)` = `210.0f`)
     - `float currentX = 12.0f;` (fixed left-aligned cursor; completely uncentered regardless of window size)
   - Cable rendering in `drawRoutingWire()` lines 304–317 uses either a straight line or `dl->AddBezierCubic(p1, cp1, cp2, p2, ...)` where `cp1 = (p1.x + dx, p1.y)`, `cp2 = (p2.x - dx, p2.y)` with fixed `dx = (p2.x - p1.x) * 0.5f`.
   - `RackView::renderSignalCable()` lines 1138–1155 draws only a straight horizontal line `dl->AddLine(...)`.
   - There is no cubic Hermite spline evaluation, no distance-adaptive tangent calculation, and zero animated pulse dots.

4. **Monolithic Modals in `src/ui/rack_view.cpp`**:
   - Total lines in `src/ui/rack_view.cpp`: **3,204 lines**.
   - `renderPluginBrowserModal()` spans lines 2305–2610 (~306 lines).
   - `renderSettingsModal()` spans lines 2812–2985 (~174 lines).
   - `renderPracticeToolsModal()` spans lines 2986–3204 (~219 lines).
   - Modals share state directly inside `RackView` class fields (`m_showPluginBrowser`, `m_pluginSearchQuery`, `m_newPathBuffer`, `m_showSettingsModal`, `m_showPracticeToolsModal`, `m_audioFilePathBuffer`, etc.). No dedicated modal classes or translation units exist.

5. **Plugin Card Structure & Controls**:
   - Card dimensions are 240x224px (`cardWidth = 240.0f; cardHeight = 224.0f;`, lines 1226–1227).
   - Fallback faceplate (when no preview thumbnail is available) draws 3 non-functional decorative rotary knobs (lines 1495–1510):
     ```cpp
     for (int k = -1; k <= 1; ++k) {
         ImVec2 kCenter(midX + k * knobSpacing, midY - 2.0f);
         dl->AddCircleFilled(kCenter, knobRadius, knobBg);
         dl->AddCircle(kCenter, knobRadius, knobCol, 24, 1.5f);
         float angle = -2.356f + (k + 1) * 2.356f;
         ImVec2 pt(kCenter.x + std::cos(angle) * (knobRadius - 2.0f), kCenter.y + std::sin(angle) * (knobRadius - 2.0f));
         dl->AddLine(kCenter, pt, knobCol, 1.8f);
     }
     ```
     These knobs do not respond to clicks or drags and do not bind to DSP parameters.
   - Bypass button in lines 1578–1582 uses a standard rectangle push button (`CenteredButton(btnId, ImVec2(64, 24))`) rather than a sliding capsule pill switch.

6. **Plugin Browser & Search Paths**:
   - `renderPluginBrowserModal()` lines 2318–2324 uses basic `ImGui::InputTextWithHint` with simple substring search via `m_scanner.getFilteredPlugins`. It lacks keyboard navigation (arrow keys, Enter to insert) and is not a centered Spotlight command palette overlay.
   - Search paths configuration (lines 2591–2605) requires manual path text typing in `ImGui::InputTextWithHint("##NewPath", "e.g. D:\\AudioPlugins", m_newPathBuffer, ...)`. There is no Win32 `IFileOpenDialog` (`FOS_PICKFOLDERS`) integration.

7. **Quick Looper & Drag-and-Drop**:
   - `renderPracticeToolsModal()` lines 3037–3048 draws a linear `ImGui::ProgressBar(progress, ImVec2(..., 22.0f), ...)` for the Quick Looper playhead. No circular ring is present.
   - `QuickLooper` (`src/tools/quick_looper.h` / `.cpp`) only accepts live audio inputs in `process()`; it does not support WAV file loading.
   - `WndProc` in `src/main.cpp` lines 410–435 does not handle `WM_DROPFILES`, nor is `DragAcceptFiles(hwnd, TRUE)` invoked.

8. **Preset Hotkeys, Crossfading, and HUD Feedback**:
   - Preset hotkeys 1–8 in `rack_view.cpp` lines 428–437 trigger `m_scenes.applyScene(k, m_graph)` synchronously.
   - `SceneManager::applyScene` (`src/state/scene_manager.cpp` line 261) calls `graph.clearNodes()` abruptly, cutting the active DSP audio stream and causing audible pops/clicks.
   - `dsp_utils.h` lines 65–119 provides `EqualPowerRamp` with `getNextGains()`, but it is currently not integrated into `SceneManager` or `RackView` scene switches.
   - Scene feedback in `renderSceneBar()` lines 960–988 renders an inline capsule pill with `ImGui::SameLine(0, 12)` and `ImGui::Dummy(ImVec2(pillW, frameH))`. When active, it causes adjacent buttons to shift horizontally, and when the timer expires (2.5s), the layout jerks back to the left.

---

## Logic Chain

1. **Color Tokens & Verification Script**:
   - *Observation 1* establishes that 170 raw `IM_COL32` calls exist in `rack_view.cpp` and `theme.h` lacks semantic structuring.
   - *Requirement R3* mandates semantic token structs across 4 switchable themes (Obsidian Studio, Cyber/Midnight, Nordic Slate, Vintage Console).
   - *Acceptance Criteria* requires zero undeclared `IM_COL32` calls verified by `scripts/check_hardcoded_colors.py` and WCAG AA 4.5:1 contrast compliance.
   - *Therefore*: A dedicated `src/ui/design_tokens.h` must declare semantic token groups (`SurfaceTokens`, `BorderTokens`, `TextTokens`, `SignalStateTokens`, `CableTokens`), provide a global theme state (`applyTheme(ThemeId)`), and `scripts/check_hardcoded_colors.py` must scan `src/ui/` to ensure no `IM_COL32` calls exist outside token definitions.

2. **Typography Architecture**:
   - *Observation 2* shows font loading is unbundled and OS-dependent.
   - *Requirement R3* requires embedding Inter and JetBrains Mono as binary resources in `resources/praccy.rc` and loading via `AddFontFromMemoryTTF`.
   - *Therefore*: TrueType font files (`Inter-Regular.ttf` and `JetBrainsMono-Regular.ttf`) must be added to `resources/fonts/`, declared in `praccy.rc` as `RT_RCDATA` (`IDR_FONT_INTER`, `IDR_FONT_JETBRAINS_MONO`), and extracted in `src/main.cpp` using `FindResourceW`, `LoadResource`, `LockResource`, and `SizeofResource` before passing memory buffers to `io.Fonts->AddFontFromMemoryTTF`. Monospace font pointer `g_fontMono` must be accessible for numeric readouts.

3. **Responsive Viewport Centering & Spline Cables**:
   - *Observation 3* identifies hardcoded coordinates (`currentX = 12.0f; centerY = 120.0f;`) and simple straight/bezier lines.
   - *Requirement R3* specifies dynamic horizontal and vertical centering and cubic Hermite splines with distance-adaptive control tangents and animated pulse dots.
   - *Therefore*: `RackView::renderSignalRack()` must pre-calculate the total bounding footprint of the chain ($W_{\text{total}} = W_{\text{in}} + \sum W_{\text{wires}} + \sum W_{\text{nodes}} + W_{\text{insert}} + W_{\text{out}}$), determine available child window dimensions ($W_{\text{avail}}, H_{\text{avail}}$), and set $X_{\text{start}} = \max(16.0f, (W_{\text{avail}} - W_{\text{total}}) * 0.5f)$ and $Y_{\text{center}} = \max(\text{rackH} * 0.5f, H_{\text{avail}} * 0.5f)$. Cable rendering must use cubic Hermite tangent math $M = (\max(36.0f, \Delta x \times 0.55f + |\Delta y| \times 0.35f), 0)$ converted to cubic Bézier control points $C_0 = P_0 + \frac{1}{3}M, C_1 = P_1 - \frac{1}{3}M$, layered with outer sleeve, core, and moving pulse dots parameterized by $u = \text{fmod}(t \times \text{speed}, 1.0f)$.

4. **Modal Decoupling**:
   - *Observation 4* shows `rack_view.cpp` is a 3,204-line monolithic file containing inline implementations for all modal dialogs.
   - *Requirement R4* mandates splitting into dedicated translation units: `src/ui/modals/plugin_browser_modal.cpp`, `settings_modal.cpp`, and `practice_tools_modal.cpp`.
   - *Therefore*: Clean modal classes (`PluginBrowserModal`, `SettingsModal`, `PracticeToolsModal`) with separated `.h` and `.cpp` files in `src/ui/modals/` must be authored and added to `CMakeLists.txt`, decoupling UI state and simplifying `RackView`.

5. **Plugin Card Redesign**:
   - *Observation 5* demonstrates inert drawn rotary knobs on the fallback faceplate and rectangular bypass buttons.
   - *Requirement R4* requires 240x224px cards with parameter readout headers, thumbnail frames with border glows, active/bypass pill toggles, and elimination of inert knobs.
   - *Therefore*: Remove the 3 inert circles, replace with a clean DSP vector badge/waveform icon; display parameter readout headers in `g_fontMono`; add multi-layered outer glow border around thumbnail frames based on active/bypass/open states; implement a custom interactive sliding capsule pill toggle.

6. **Command Palette & Native Folder Picker**:
   - *Observation 6* reveals manual folder typing and lack of keyboard navigation in plugin selection.
   - *Requirement R4* requires a Spotlight-style keyboard command palette overlay with fuzzy search, arrow navigation, Enter/double-click insertion, and native Win32 `IFileOpenDialog` (`FOS_PICKFOLDERS`).
   - *Therefore*: Rebuild `PluginBrowserModal` as a centered, borderless modal overlay with automatic keyboard focus; implement fuzzy string scoring matching title, vendor, and category; handle Up/Down arrow selection and Enter key dispatch; replace `InputText` in search paths with `CoCreateInstance(CLSID_FileOpenDialog)` configuring `FOS_PICKFOLDERS`.

7. **Quick Looper Circular Ring & Drag-and-Drop**:
   - *Observation 7* shows linear progress bar and lack of drag-and-drop handling.
   - *Requirement R4* specifies a circular progress ring indicating states (Recording: Red, Overdubbing: Amber, Playing: Green) and WAV drag-and-drop via `WM_DROPFILES`.
   - *Therefore*: Draw circular ring using `ImDrawList::PathArcTo` from $-\pi/2$ to $-\pi/2 + 2\pi \times \text{playheadNormalized()}$ with state colors (`#FF3B30`, `#FF9500`, `#34C759`); add `loadWavFile()` / buffer setter to `QuickLooper`; call `DragAcceptFiles(hwnd, TRUE)` in `main.cpp` and parse `WM_DROPFILES` via `DragQueryFileW`.

8. **Preset Crossfading & Floating Toast**:
   - *Observation 8* reveals abrupt graph node clearing (clicks/pops) and horizontal layout jitter from inline feedback text.
   - *Requirement R4* specifies click-free crossfade ramping (`EqualPowerRamp`, 10ms) and a centered floating HUD toast fading after 1.8s.
   - *Therefore*: Wire `EqualPowerRamp` into preset transitions to smoothly attenuate existing audio and ramp up incoming audio over 480 samples; replace inline preset bar feedback with a non-intrusive floating toast rendered via `ImGui::GetForegroundDrawList()` at center-top with 1.8s duration and final 0.4s alpha fade.

---

## Features Discovered

| # | Category | Feature | Description | Inputs | Outputs | Error Behavior | Discovered Via |
|---|----------|---------|-------------|--------|---------|----------------|----------------|
| 1 | Design Tokens | 4 Switchable Production Themes | Obsidian Studio, Cyber/Midnight, Nordic Slate, Vintage Console themes defining surface, border, text, and signal colors | `ThemeId` enum selection | Updated `ImGuiStyle` and active token palette | Falls back to default Obsidian Studio on invalid enum | `ORIGINAL_REQUEST.md` R3; `src/ui/theme.h` |
| 2 | Design Tokens | Design Token Header (`design_tokens.h`) | Semantic structs: `SurfaceTokens`, `BorderTokens`, `TextTokens`, `SignalStateTokens`, `CableTokens` | Theme identifier | Immutable token reference `const ThemeTokens&` | Compile-time validation | `ORIGINAL_REQUEST.md` R3 |
| 3 | CI / Linter | Hardcoded Color Verification Script | `scripts/check_hardcoded_colors.py` verifies zero undeclared `IM_COL32` literals in `src/ui/` | File tree `src/ui/` | Exit code 0 (clean) or 1 (violations listed) | Emits filenames, line numbers, and lines | `ORIGINAL_REQUEST.md` Acceptance Criteria |
| 4 | Typography | Embedded Binary Fonts | Inter (UI) and JetBrains Mono (Readouts) compiled into executable via `praccy.rc` RT_RCDATA | Resource IDs `IDR_FONT_INTER`, `IDR_FONT_JETBRAINS_MONO` | Loaded `ImFont* g_fontUI`, `g_fontMono` | Fallback to `io.Fonts->AddFontDefault()` if resource missing | `ORIGINAL_REQUEST.md` R3; `src/main.cpp` |
| 5 | Responsive Canvas | Dynamic Rack Viewport Centering | Computes total bounding width and height of signal chain, auto-centering horizontally and vertically | Window / child dimensions, node count | Centered offset $(X_{\text{start}}, Y_{\text{center}})$ | Clamps to minimum left margin (16px) if footprint exceeds window | `ORIGINAL_REQUEST.md` R3; `src/ui/rack_view.cpp` |
| 6 | Responsive Canvas | High-DPI Canvas Scaling | Preserves legible text and layout proportions across 1080p (100%), 1440p (125%), and 4K (150%, 200%) | Windows DPI scale factor | Scaled font and element dimensions | Restricts minimum node card size to prevent text overlap | `ORIGINAL_REQUEST.md` Acceptance Criteria |
| 7 | Cable Rendering | Cubic Hermite Spline Cables | Computes smooth connection cables with distance-adaptive horizontal tangents: $M = (\max(36.0f, 0.55\Delta x + 0.35\lvert\Delta y\rvert), 0)$ | Socket positions $P_0, P_1$ | Multi-layered Bézier curve on DrawList | Straight line fallback if $\Delta x < 2.0f$ | `ORIGINAL_REQUEST.md` R3; `src/ui/rack_view.cpp` |
| 8 | Cable Rendering | Animated Signal Pulse Dots | Glowing pulse circles traveling along Hermite cable paths indicating active audio flow | Pulse speed, elapsed time, bypass state | DrawList circle primitives | Halts animation or dims dots if node is bypassed/muted | `ORIGINAL_REQUEST.md` R3; `src/ui/rack_view.cpp` |
| 9 | Modular UI | Modal Translation Unit Decoupling | Splits monolithic `rack_view.cpp` into dedicated compilation units in `src/ui/modals/` | User interaction / trigger flags | Rendered modal dialogs | Isolated component compile boundaries | `ORIGINAL_REQUEST.md` R4; `src/ui/rack_view.cpp` |
| 10 | Modular UI | Plugin Browser Modal (`plugin_browser_modal.cpp`) | Dedicated compilation unit for plugin browsing, category navigation, and search | Scanner instance, target slot | Modal state & plugin insertion | Catches invalid plugin load and logs error | `ORIGINAL_REQUEST.md` R4 |
| 11 | Modular UI | Settings Modal (`settings_modal.cpp`) | Dedicated compilation unit for ASIO device selection, buffer size, latency readout, and theme selector | ASIO manager, scanner, config | Modifies driver state & active theme | Validates driver load success before persistence | `ORIGINAL_REQUEST.md` R4 |
| 12 | Modular UI | Practice Tools Modal (`practice_tools_modal.cpp`) | Dedicated compilation unit housing Quick Looper and Backing Track Player | Looper, audio player, timing | Tool operation UI & visualization | Validates WAV file path and format | `ORIGINAL_REQUEST.md` R4 |
| 13 | Plugin Cards | 240x224px Card Redesign | Uniform cards with format tag, vendor, parameter readouts, preview window, and trim slider | `PluginSlot*` state | Rendered ImGui Child frame | Gracefully truncates long plugin names | `ORIGINAL_REQUEST.md` R4; `src/ui/rack_view.cpp` |
| 14 | Plugin Cards | Elimination of Inert Knobs | Removes 3 dummy non-functional rotary knobs from DSP fallback faceplate | N/A | Vector DSP / Waveform icon | Clean visual indicator without inert drag traps | `ORIGINAL_REQUEST.md` R4; `src/ui/rack_view.cpp` |
| 15 | Plugin Cards | Parameter Readout Header | Live numeric readouts of active parameters (Dry/Wet %, Trim dB, format) in monospace font | Slot parameters | Rendered text header | Formats NaN/overflow to default strings | `ORIGINAL_REQUEST.md` R4 |
| 16 | Plugin Cards | Thumbnail Frame Border Glow | Layered glowing border around thumbnail preview reflecting active, bypass, and window-open states | Slot bypass & window focus | Layered `AddRect` glow effect | Dims glow completely when bypassed | `ORIGINAL_REQUEST.md` R4; `src/ui/rack_view.cpp` |
| 17 | Plugin Cards | Sliding Capsule Pill Toggle | Tactile capsule switch (e.g. 44x22px) with animated sliding dot for Active/Bypass | Click on pill toggle | Flips `slot->setBypassed(!bypassed)` | Visual toggle state strictly synchronized with DSP | `ORIGINAL_REQUEST.md` R4 |
| 18 | Plugin Navigation | Spotlight Command Palette | Centered keyboard-focused overlay with fuzzy filtering across title, vendor, and category | Keyboard input string, Up/Down keys, Enter | Selected plugin inserted at target | Shows "No matching plugins" if query unmatched | `ORIGINAL_REQUEST.md` R4; `src/ui/rack_view.cpp` |
| 19 | Plugin Navigation | Native Win32 Folder Picker | Replaces manual text path input with Win32 `IFileOpenDialog` (`FOS_PICKFOLDERS`) | User folder selection dialog | Adds folder to scanner search paths | User cancel leaves search paths unchanged | `ORIGINAL_REQUEST.md` R4 |
| 20 | Practice Suite | Quick Looper Circular Progress Ring | Circular ring visualizing loop progress; colored by state (Recording: Red, Overdubbing: Amber, Playing: Green) | `LooperState`, `playheadNormalized()` | Rendered circular progress arc and time readouts | Grey / inactive ring when Empty or Stopped | `ORIGINAL_REQUEST.md` R4; `src/tools/quick_looper.h` |
| 21 | Practice Suite | WAV File Drag-and-Drop | Dragging `.wav` files onto window automatically loads audio into Looper / Backing Player | Win32 `WM_DROPFILES` message | Loaded loop / track ready for playback | Non-WAV or malformed files safely rejected with message | `ORIGINAL_REQUEST.md` R4; `src/main.cpp` |
| 22 | Preset System | Click-Free Crossfade Ramping | Smooth 10ms equal-power crossfade during preset hotkey switches (1–8) eliminating audio clicks | Scene hotkey event, active audio stream | Blended output audio signal | Handles rapid preset re-triggering without popping | `ORIGINAL_REQUEST.md` R4; `src/audio/dsp_utils.h` |
| 23 | Preset System | Centered Floating HUD Toast | Non-layout-shifting floating notification displayed at center-top upon scene switch, fading after 1.8s | Preset switch event | Rendered foreground HUD toast overlay | Zero layout shift on adjacent controls | `ORIGINAL_REQUEST.md` R4; `src/ui/rack_view.cpp` |

---

## Edge Cases

| # | Feature | Input | Observed Behavior |
|---|---------|-------|-------------------|
| 1 | Viewport Centering | Rack chain width exceeds window client width | $X_{\text{start}}$ calculation results in negative value; without clamp, left edge (Input Node) would be clipped offscreen. Must clamp $X_{\text{start}} = \max(16.0f, (W_{\text{avail}} - W_{\text{total}}) * 0.5f)$ and enable horizontal scrollbar. |
| 2 | Viewport Centering | Signal chain contains zero plugin nodes (only Input -> Insert Slot -> Output) | Total footprint is $\approx 424px$. On a 1920px wide display, center offset is $\approx 738px$, correctly positioning the minimal chain dead-center in the window without distortion. |
| 3 | Viewport Centering | Parallel block with uneven branches (e.g. Branch A has 4 slots, Branch B has 0 slots) | Envelope width scales with $\max(w_0, w_1, 460px)$. Centering must use total envelope width so entry/exit wires and combiner node align symmetrically. |
| 4 | Hermite Splines | Input and output sockets at identical vertical coordinate ($\Delta y = 0$) | Tangents remain purely horizontal ($M = (T_{\text{mag}}, 0)$). Spline renders as a perfectly straight collinear wire without vertical bulge or curvature artifact. |
| 5 | Hermite Splines | Sockets separated by very short horizontal distance ($\Delta x < 8px$) | Adaptive tangent formula $T_{\text{mag}} = \max(36.0f, \dots)$ could cause spline loop-back if $\Delta x$ is tiny. Tangent must be clamped: $T_{\text{mag}} = \min(T_{\text{mag}}, \Delta x \times 0.5f)$ when $\Delta x$ is small. |
| 6 | Pulse Dot Animation | Plugin slot is bypassed while audio engine is running | Dot animation along that cable segment stops moving or dims alpha to 30%, visually confirming signal interruption. |
| 7 | Command Palette | User presses Enter with an empty search query or zero matching items | No plugin is selected; press must be ignored without crashing, attempting null dereference, or closing dialog unexpectedly. |
| 8 | Command Palette | User navigates list using Up arrow when top item (index 0) is already highlighted | Index clamps to 0 (or wraps to bottom); does not trigger out-of-bounds indexing in filtered results vector. |
| 9 | Command Palette | User types special characters, regex tokens, or symbols (e.g. `[`, `*`, `\`) into fuzzy search | Fuzzy matcher treats query as literal character sequence; no regex syntax exceptions or parsing failures occur. |
| 10 | Native Folder Picker | User invokes `IFileOpenDialog` and presses "Cancel" | `pFileOpen->Show(hwnd)` returns `HRESULT` `HRESULT_FROM_WIN32(ERROR_CANCELLED)` / `0x800704C7`. Scanner search paths must remain unchanged. |
| 11 | Native Folder Picker | User selects a directory already present in custom search paths | Scanner deduplicates path; duplicate path is not added to `AppConfig` or search list. |
| 12 | Quick Looper WAV Drop | User drops a corrupted, non-WAV, or 0-byte file via `WM_DROPFILES` | Header parser detects invalid RIFF/WAVE tag or 0 frames, rejects file, leaves existing looper buffer intact, and displays toast error. |
| 13 | Quick Looper WAV Drop | User drops a 44.1 kHz WAV file while ASIO driver is running at 48 kHz or 96 kHz | Sample rate mismatch must be handled via linear interpolation resampling to match `m_sampleRate`, preventing pitch/tempo drift. |
| 14 | Quick Looper Ring | Loop length is 0 (looper in Empty state) | Progress is 0.0f; ring renders subtle neutral background track with "EMPTY" status in center without division-by-zero. |
| 15 | Preset Crossfade Ramp | User rapidly presses hotkeys 1, 2, 3 within a single 10ms window | Active ramp re-anchors its starting gain from the current instantaneous crossfade gain rather than resetting to 1.0f/0.0f, preventing click discontinuities. |
| 16 | Floating HUD Toast | Toast is active (fading out) and user switches to another preset | Toast timer resets to 1.8s, text updates immediately to new preset name, and alpha resets to 1.0 without layout flickering or stutter. |
| 17 | Theme Switching | Switching themes dynamically while plugin GUI windows are open | ImGui styles and token palette update immediately on the next frame; open Win32 plugin windows remain undisturbed. |
| 18 | WCAG AA Contrast | Text rendering on dark surface cards in all 4 themes | Evaluated contrast ratio for primary text tokens ($L_1 + 0.05) / (L_2 + 0.05)$ must strictly exceed 4.5:1 against card and panel background tokens. |

---

## Caveats

- **Audio Engine Callback Thread Safety**: Crossfade ramping during preset switching requires coordination between the UI thread (which initiates the scene change) and the real-time ASIO audio callback thread. Pre-allocating parallel scene slots or ramping buffers avoids dynamic allocation on the audio thread.
- **Font Licensing**: Inter and JetBrains Mono are open-source fonts (SIL Open Font License 1.1 / Apache 2.0). Bundling their TTF binaries in `resources/fonts/` is legally compliant with appropriate attribution in the repository.
- **COM Initialization**: `IFileOpenDialog` requires COM initialized on the calling thread (`CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`).praccy's main thread must verify COM initialization before invoking the dialog.

---

## Conclusion

The UI and practice suite components of Praccy currently contain architectural gaps that violate the Praccy v2.0 Blueprint:
1. **170 hardcoded `IM_COL32` literals** and a monolithic single-theme implementation require replacement with a tokenized design system (`src/ui/design_tokens.h`) supporting 4 WCAG AA-compliant switchable themes and enforced via `scripts/check_hardcoded_colors.py`.
2. **Typography** relies on unbundled Windows system fonts rather than embedded Inter and JetBrains Mono resources in `resources/praccy.rc`.
3. **Signal chain rendering** is strictly left-aligned with static coordinates and simple straight wires; it requires a dynamic viewport auto-centering algorithm and cubic Hermite splines with distance-adaptive tangents and animated pulse dots.
4. **Monolithic UI structure**: `src/ui/rack_view.cpp` (3,204 lines) must be modularized by extracting modal dialogs into `src/ui/modals/`.
5. **Plugin cards** feature inert drawn knobs and basic buttons, needing redesign to 240x224px cards with parameter readout headers, thumbnail border glows, and sliding capsule pill toggles.
6. **Practice Suite & Navigation**: The plugin browser must transition to a keyboard-driven Spotlight command palette with Win32 `IFileOpenDialog` (`FOS_PICKFOLDERS`), Quick Looper requires a circular ring with `WM_DROPFILES` WAV drop support, and scene presets need click-free 10ms `EqualPowerRamp` crossfading and centered floating HUD toast notifications.

The feature specifications and edge cases documented above provide the complete blueprint required for implementation in Milestones 3 and 4.

---

## Verification Method

1. **Verify Hardcoded Color Count**:
   Execute in PowerShell:
   ```pwsh
   Select-String -Path "src/ui/rack_view.cpp" -Pattern "IM_COL32\(" | Measure-Object | Select-Object -ExpandProperty Count
   ```
   *Expected Current Output*: `170`.
   *Pass Condition after R3 implementation*: `0` undeclared `IM_COL32` occurrences when running `python scripts/check_hardcoded_colors.py`.

2. **Verify Windows Resource Script**:
   Inspect `resources/praccy.rc` to verify absence of font declarations:
   ```pwsh
   Get-Content resources/praccy.rc
   ```
   *Pass Condition after R3 implementation*: Contains declarations for `IDR_FONT_INTER RCDATA "fonts/Inter-Regular.ttf"` and `IDR_FONT_JETBRAINS_MONO RCDATA "fonts/JetBrainsMono-Regular.ttf"`.

3. **Verify Modal File Layout**:
   Inspect `src/ui/` directory:
   ```pwsh
   Test-Path "src/ui/modals"
   ```
   *Expected Current Output*: `False`.
   *Pass Condition after R4 implementation*: `True`, with `plugin_browser_modal.cpp`, `settings_modal.cpp`, and `practice_tools_modal.cpp` present and compiled via `CMakeLists.txt`.

4. **Verify Build & Unit Tests**:
   Build the test suite and verify clean compilation:
   ```pwsh
   cd f:\Projects\Praccy\build
   cmake --build . --config Release --target test_praccy
   ctest --output-on-failure
   ```
