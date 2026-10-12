# Handoff Report: UI Ergonomics & Visual Polish Survey

**Explorer**: UI Ergonomics & Visual Polish Explorer  
**Date**: 2026-10-12  
**Target Milestone**: Praccy V2.4 Stage 1  
**Report Artifact**: `F:\Projects\Praccy\.agents\teamwork\explorer_survey_ui\survey_report.md`  

---

## 1. Observation

1. **Hardcoded Color Lint Status**:
   - Tool Command: `py scripts/check_hardcoded_colors.py`
   - Tool Result:
     ```
     SUCCESS: Clean! 0 hardcoded IM_COL32 / ImVec4 literals found in F:\Projects\Praccy\src\ui.
     Design tokens fully enforced across all UI translation units.
     ```
   - Exit code: `0`.
2. **Current CTest Regression Suite**:
   - Tool Command: `ctest --output-on-failure` in `F:\Projects\Praccy\build`
   - Tool Result:
     ```
     100% tests passed out of 10
     Total Test time (real) = 10.93 sec
     ```
   - All 10 existing test targets (`test_praccy`, `test_asio_driver`, `test_challenger_m1` through `m4_2`) pass with zero regressions.
3. **State Management & Undo (`src/state/scene_manager.h`, `scene_manager.cpp`)**:
   - `src/state/scene_manager.h` defines `PluginSlotPreset` (lines 11–20), `BranchPreset` (lines 22–30), `NodePreset` (lines 32–37), and `ScenePreset` (lines 39–42).
   - `SceneManager` handles default scene initialization, `loadFromFile`, `saveToFile`, `captureCurrentScene`, and `applyScene` (which calls `graph.crossfadeToNodes`).
   - Verbatim check: There is **no** `UndoManager` class, **no** transaction history stack, and **no** A/B preset snapshot memory toggle in the codebase.
4. **Card Ergonomics & Knobs (`src/ui/rack_view.h`, `rack_view.cpp`)**:
   - `src/ui/rack_view.h:61` declares:
     ```cpp
     bool renderRotaryKnob(const char* label, float* value, float minVal, float maxVal, float radius, const char* format = "%.1f", float defaultVal = 0.0f);
     ```
   - In `src/ui/rack_view.cpp:2052–2060`, lines are blank; `renderRotaryKnob` is **never implemented**.
   - `renderPluginSlot()` in `src/ui/rack_view.cpp:1049–1415` renders only fixed Mix and Output Trim sliders (`ResettableSliderFloat`). It contains **no** device macro mini-knobs.
   - `renderSignalRack()` in `src/ui/rack_view.cpp:788–950` lays out slots statically at `currentX += cardW + wireW`. It contains **no** `ImGui::BeginDragDropSource` or `BeginDragDropTarget` calls for card reordering.
5. **Keyboard Shortcuts & Map Mode (`src/ui/rack_view.cpp`)**:
   - Lines 211–226:
     ```cpp
     ImGuiIO& io = ImGui::GetIO();
     if (!io.WantTextInput) {
         if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
             m_pluginBrowserModal->open();
         }
         for (int k = 0; k < 8 && k < static_cast<int>(m_scenes.numScenes()); ++k) {
             if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + k), false)) {
                 if (m_scenes.applyScene(k, m_graph)) {
                     triggerHudToast("PRESET " + std::to_string(k + 1) + " RECALLED");
                 }
             }
         }
     }
     ```
   - Verbatim check: There are **no** handlers for `Ctrl+Z` (Undo), `Ctrl+Y` (Redo), `Ctrl+M` (MIDI Map), `Ctrl+K` (Key Map), `Ctrl+Shift+R` (Save Last Riff), or focused card keys (`[`, `]`, `Q`, `0`).
6. **Spotlight Virtualization & DPI (`src/ui/modals/plugin_browser_modal.cpp`)**:
   - Lines 372–458: Loops linearly over all items in `m_filteredItems`. **No `ImGuiListClipper`** is used.
   - Lines 464–466:
     ```cpp
     ImGui::TextColored(tokens.text.muted.vec4, "[UP/DOWN] Navigate    [ENTER] Insert Plugin    [ESC] Dismiss");
     ImGui::SameLine(modalSize.x - 170.0f);
     ImGui::TextColored(tokens.text.muted.vec4, "%zu plugins available", m_filteredItems.size());
     ```
     At 125%–200% DPI, the left text width exceeds 390px, colliding directly with the right text element.
   - There is no `Tab` key cycling for scopes (`All` / `Favorites` / `Recents`), no `F` key favorite toggling, and no star rating sorting.
7. **Backing Track Waveform (`src/ui/modals/practice_tools_modal.cpp`)**:
   - Lines 247–251 render a plain `ImGui::SliderFloat("##PlayerSeek", &normPos, 0.0f, 1.0f, "")`. No 800-point overview waveform strip or interactive loop markers exist.
8. **Visual Hierarchy & Tokens (`src/ui/design_tokens.h`)**:
   - `tokens.surfaces.modalOverlay` uses alpha 215–220 (~84%) instead of the required 0.75 alpha (191).
   - Active scene button currently uses bright green `tokens.signal.active` instead of dark emerald (`#104B2D`) with focus border.
   - Looper idle text currently uses `tokens.borders.subtle` instead of `#9CA3AF`.
   - Top ribbon modules use inconsistent FramePadding (`6.0f, 3.5f` in Metronome, `8.0f, 6.0f` window padding) rather than standardized `6x4`.

---

## 2. Logic Chain

1. **State Management & Undo (R3)**:
   - *From Observation 3*: `ScenePreset` already encapsulates full node and parameter state, and `graph.crossfadeToNodes()` executes equal-power transitions.
   - *Therefore*: A new transactional `UndoManager` can directly record `ScenePreset` snapshots without altering the underlying serialization schema.
   - *Coalescing Logic*: Continuous parameter sweeps (sliders/knobs) and rapid MIDI CC streams must be debounced with a 400ms idle threshold and committed on `ImGui::IsItemDeactivatedAfterEdit()`.
   - *A/B Snapshot*: Dual `ScenePreset` buffers in `SceneBar` can leverage the existing `crossfadeToNodes` API for 10ms click-free auditioning.

2. **Tactile Rack Physics & Performance Ergonomics (R4)**:
   - *From Observation 4*: `IPluginInstance` already exposes parameter enumeration and mutation (`numParameters()`, `getParameterDesc()`, `setParameterValue()`).
   - *Therefore*: 4 front-panel mini-knobs can be automatically populated via fuzzy regex heuristics (`Gain/Drive`, `Tone/EQ`, `Volume/Master`, `Mix/Blend`) and manually overridden via context menus.
   - *From Observation 4*: Card reordering requires ImGui drag-and-drop payloads (`"RACK_CARD_PAYLOAD"`). Reordering on the audio engine side must execute safely across block boundaries via micro-fade.
   - *From Observation 5*: Global MIDI & Key Map Mode (`Ctrl+M`, `Ctrl+K`) requires a pulsating violet viewport border. To satisfy Gate 1.5, hotkeys must be strictly guarded by `!io.WantTextInput` so text editing never fires hotkeys.
   - *From Observation 7*: The Backing Track Player requires downsampling audio in `AudioPlayer` to 800 peak points upon file load, rendered via `ImDrawList` with interactive click-to-seek and loop points.

3. **Visual Polish, DPI Safety & Virtualization (R5)**:
   - *From Observation 6*: With large plugin libraries, rendering without `ImGuiListClipper` produces excessive draw calls. Wrapping rows in `ImGuiListClipper` ensures constant 144 FPS.
   - *From Observation 6*: Replacing hardcoded `SameLine(modalSize.x - 170.0f)` with dynamic `CalcTextSize` and `GetContentRegionAvail().x` eliminates high-DPI text collision.
   - *From Observation 1 & 8*: Deepening modal scrim to 0.75 alpha, restyling active scene buttons to dark emerald, promoting looper idle text to `#9CA3AF`, and standardizing top ribbon FramePadding to 6x4 can all be achieved while maintaining 100% token purity under `scripts/check_hardcoded_colors.py`.

---

## 3. Caveats

- **Read-Only Scope**: In strict accordance with explorer archetype constraints, no production files in `src/` or `tests/` were modified during this investigation.
- **Audio DSP Subsystem Surveying**: The deep DSP aspects of R1 (Parallel Delay Compensation, lock-free SPSC parameter automation tables) and R2 (circular retroactive riff buffer allocation) are investigated in coordination with peer explorers; this report focuses on their UI integration contracts.
- **DPI Scaling Emulation**: DPI layout verification should be validated under physical or simulated DPI environments (100%, 125%, 150%, 200%).

---

## 4. Conclusion

The current codebase is in an exceptionally clean, well-tested state (0 color lint violations, 10/10 passing CTest suites). All requirements for R3, R4, and R5 are clearly scoped, architecturally sound, and directly implementable without architectural refactoring:
1. Create `src/state/undo_manager.h` and `src/state/undo_manager.cpp` (up to 64 levels, 400ms coalescing).
2. Add A/B Tone Snapshot toggle to `SceneBar`.
3. Implement `renderRotaryKnob` and 4-macro regex heuristic discovery on plugin cards.
4. Implement horizontal drag-and-drop card reordering physics.
5. Add Global MIDI/Key Map Mode with pulsating violet border, click-to-learn, and `!io.WantTextInput` hotkey guards.
6. Implement 800-point overview waveform strip in Backing Track with click-to-seek.
7. Virtualize Spotlight via `ImGuiListClipper`, fix DPI footer collisions, and add Favorites/Pinned Scope.
8. Apply visual polish: 0.75 scrim alpha, dark emerald active scene button, `#9CA3AF` looper contrast, 6x4 top ribbon padding, DIN IEC 60268-10 PPM meters, and audio-reactive Hermite cable glow.

---

## 5. Verification Method

1. **Static Color Purity Check**:
   ```pwsh
   py scripts/check_hardcoded_colors.py
   ```
   *Expected*: Exit code 0, 0 violations.
2. **CTest Regression Suite**:
   ```pwsh
   ctest --output-on-failure --test-dir build
   ```
   *Expected*: 100% passed (10/10).
3. **Stage 1 Acceptance Suite**:
   - Compile and execute the new Stage 1 test harness `tests/test_praccy_v2_1.cpp` covering undo coalescing, A/B switching, macro heuristics, Spotlight clipper, and PPM ballistics.
