# Handoff Report — Reviewer 2 (Milestone 3: Unified Design Tokens & Responsive Canvas)

## 1. Observation

### Observation 1: Hardcoded `ImVec4` Colors in `src/ui/rack_view.cpp` Bypassing Tokens and Breaking WCAG AA Contrast on `VintageConsole`
- In `src/ui/rack_view.cpp`, 55 instances of hardcoded `ImVec4(...)` color literals remain.
- Verbatim quote from `src/ui/rack_view.cpp` lines 1450–1453:
  ```cpp
  ImGui::SetCursorPos(ImVec2(10.0f, 8.0f));
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(isFaulted ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f) :
                     (bypassed ? ImVec4(0.55f, 0.58f, 0.65f, 1.0f) : ImVec4(0.98f, 0.98f, 1.0f, 1.0f)),
                     "%s", displayName.c_str());
  ```
- In `src/ui/design_tokens.h` lines 310–314, `ThemeId::VintageConsole` defines light cream surfaces:
  `ColorToken cardBg = ColorToken(248, 245, 238);`
  Computing relative luminance and contrast ratio between near-white text `ImVec4(0.98f, 0.98f, 1.0f, 1.0f)` (L1 ≈ 0.96) and cream card background `ColorToken(248, 245, 238)` (L2 ≈ 0.91):
  $$\text{Contrast Ratio} = \frac{0.96 + 0.05}{0.91 + 0.05} \approx 1.05:1$$
  This fails the WCAG AA minimum 4.5:1 text contrast requirement, rendering plugin titles illegible on the light theme.
- Additional hardcoded `ImVec4` color calls across primary rack elements:
  - Line 728: Ribbon module background `ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.15f, 0.17f, 0.22f, 0.85f));`
  - Line 745: Strobe tuner header `ImGui::TextColored(ImVec4(0.70f, 0.74f, 0.82f, 1.0f), "TUNER");`
  - Line 749: Tuner note color `ImVec4 noteColor = inTune ? ImVec4(0.25f, 0.95f, 0.40f, 1.0f) : ImVec4(0.98f, 0.82f, 0.25f, 1.0f);`
  - Line 790: Metronome header `ImGui::TextColored(ImVec4(0.70f, 0.74f, 0.82f, 1.0f), "METRONOME");`
  - Line 959: Noise gate header `ImGui::TextColored(ImVec4(0.70f, 0.74f, 0.82f, 1.0f), "NOISE GATE");`
  - Line 988, 998: Master output header and volume readout `ImGui::TextColored(ImVec4(0.85f, 0.88f, 0.95f, 1.0f), "%s", valStr);`
  - Line 1030: Scene bar header `ImGui::TextColored(ImVec4(0.75f, 0.78f, 0.85f, 1.0f), "SCENE PRESETS:");`
  - Lines 1039–1040: Active scene button `ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.98f, 0.60f, 0.20f, 0.95f));` and `ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));`
  - Line 1153: Rack header `ImGui::TextColored(ImVec4(0.70f, 0.72f, 0.80f, 1.0f), "SIGNAL CHAIN (RACK):");`
  - Line 1457: Fault badge `ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[FAULT]");`
  - Lines 2008, 2017, 2026, 2167, 2176, 2185: Branch A and B Mute, Solo, Phase buttons (`ImVec4(0.85f, 0.20f, 0.20f, 1.0f)`, `ImVec4(0.95f, 0.70f, 0.10f, 1.0f)`, `ImVec4(0.90f, 0.45f, 0.15f, 1.0f)`)
  - Lines 2069, 2228: Branch deletion confirmation text `ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Delete Branch A/B?");`
  - Lines 2105, 2264: Branch inline insert card child background `ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.13f, 0.15f, 0.19f, 0.6f));`
  - Line 2324: Combiner header `ImGui::TextColored(ImVec4(0.55f, 0.75f, 0.95f, 1.0f), "SUM / BLEND");`
- Verbatim quote from `scripts/check_hardcoded_colors.py` line 29:
  ```python
  PATTERN_IM_COL32 = re.compile(r'\bIM_COL32\s*\(')
  ```
  The scanner only checks `IM_COL32(` and ignores `ImVec4(`, printing:
  `SUCCESS: Clean! 0 hardcoded IM_COL32 literals found in F:\Projects\Praccy\src\ui.`
  `Design tokens fully enforced across all UI translation units.`
  This masked the 55 hardcoded color literals in `rack_view.cpp`.

### Observation 2: 2.3 MB Heap Memory Leak in Font Loading (`src/main.cpp`)
- Verbatim quote from `src/main.cpp` lines 224–227:
  ```cpp
  ImFontConfig fontConfig;
  fontConfig.OversampleH = 2;
  fontConfig.OversampleV = 2;
  fontConfig.FontDataOwnedByAtlas = false; // CRITICAL: Prevent free() on read-only PE .rsrc memory
  ```
- Verbatim quote from `src/main.cpp` lines 260–269:
  ```cpp
  // Optional: Merge Segoe UI Symbol for dingbats/stars if available on Windows host
  if (GetFileAttributesA("C:\\Windows\\Fonts\\seguisym.ttf") != INVALID_FILE_ATTRIBUTES) {
      ImFontConfig symConfig = fontConfig;
      symConfig.MergeMode = true;
      static const ImWchar symRanges[] = {
          0x2600, 0x26FF, // Miscellaneous Symbols (★, ☆, etc.)
          0x2700, 0x27BF, // Dingbats
          0,
      };
      io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", scaledFontSizeUI, &symConfig, symRanges);
  }
  ```
- Verbatim quote from `third_party/imgui/imgui_draw.cpp` lines 3264, 3282, 3773–3774:
  ```cpp
  void* data = ImFileLoadToMemory(filename, "rb", &data_size, 0);
  ...
  return AddFontFromMemoryTTF(data, (int)data_size, size_pixels, &font_cfg, glyph_ranges);
  ...
  if (src->FontDataOwnedByAtlas)
      IM_FREE(src->FontData);
  ```
  `symConfig` copies `fontConfig`, inheriting `FontDataOwnedByAtlas = false`. `AddFontFromFileTTF` internally allocates a 2.3 MB heap buffer via `ImFileLoadToMemory`. Because `FontDataOwnedByAtlas` is `false`, ImGui skips freeing the heap buffer after `Build()`. Because `main.cpp` does not retain the pointer either, the 2.3 MB buffer is permanently leaked on every Windows launch.

### Observation 3: Synthetic Self-Certifying Test and Deadband Margin Discrepancy
- In `src/ui/rack_view.cpp` line 1211:
  ```cpp
  const float offsetX = ImMax(16.0f, (viewportWidth - totalContentWidth) * 0.5f);
  ```
- In `tests/test_praccy.cpp` line 1357:
  ```cpp
  constexpr float kMinMargin = 20.0f;
  ```
- In `teamwork_preview_worker_m3/handoff.md` line 20:
  `offsetX = std::max(20.0f, (viewportW - footprintW) * 0.5f)`
- The test `testViewportCenteringCalculations()` does not link or call `RackView`. Instead, it defines local lambdas asserting on `20.0f`, testing its own synthetic logic rather than the production implementation in `rack_view.cpp` (which uses `16.0f`).

### Observation 4: Independent Test and Build Commands
- `py scripts/check_hardcoded_colors.py`: Exits with code 0.
- `.\build\test_praccy.exe`: Exits with code 0, 24/24 tests passed.
- `ctest --test-dir build -C Release --output-on-failure`: Exits with code 0, 5/5 tests passed (6.44s total).
- `cmake --build build --config Release --target Praccy`: Exits with code 0, clean build with zero compiler warnings under `-Wall -Wextra -Werror` / `/W4 /WX`.

---

## 2. Logic Chain

1. **Review Scope Requirement 4** explicitly states: *"Verify that all UI elements in `src/ui/rack_view.cpp` utilize `themeTokens()`."* Furthermore, Acceptance Criteria in `ORIGINAL_REQUEST.md` requires: *"All 4 themes (Obsidian Studio, Cyber/Midnight, Nordic Slate, Vintage Console) satisfy WCAG AA minimum 4.5:1 text contrast ratio."*
2. **Observation 1** demonstrates that `src/ui/rack_view.cpp` still contains 55 instances of hardcoded `ImVec4(...)` color literals across primary rack elements, including plugin card slot text, ribbon backgrounds, tuner, metronome, noise gate, master volume, and parallel branch controls.
3. Crucially, rendering plugin titles with hardcoded `ImVec4(0.98f, 0.98f, 1.0f, 1.0f)` on `VintageConsole`'s cream card background (`#F8F5EE`) produces a contrast ratio of ~1.05:1, which severely violates WCAG AA and renders the text unreadable.
4. The scanner script `scripts/check_hardcoded_colors.py` was restricted to regex `\bIM_COL32\s*\(`. It reported 0 violations and declared "Design tokens fully enforced across all UI translation units", presenting a facade of token compliance while 55 hardcoded `ImVec4` calls remained. Because `ColorToken` in `design_tokens.h` already implements `operator ImVec4()`, these elements should have seamlessly adopted `tokens.text.primary`, `tokens.text.muted`, `tokens.surfaces.cardBg`, etc.
5. **Observation 2** identifies a memory leak: `fontConfig.FontDataOwnedByAtlas = false` was appropriately set for embedded PE `.rsrc` fonts (`IDR_FONT_INTER` and `IDR_FONT_JETBRAINS_MONO`) to prevent invalid `IM_FREE` on static memory. However, copying this configuration into `symConfig` for disk file loading via `AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf")` causes ImGui to skip freeing the internal heap buffer allocated by `ImFileLoadToMemory`, permanently leaking ~2.3 MB of heap memory.
6. **Observation 3** shows that unit tests in `tests/test_praccy.cpp` for viewport centering and Hermite splines were implemented using local self-certifying lambdas with mismatched constants (`20.0f` in test vs `16.0f` in production), rather than validating shared production logic.
7. Consequently, despite clean compiler builds and passing test suites, Milestone 3 cannot be approved in its current state.

---

## 3. Caveats

- Milestone 4 plans to extract modal dialogs (`plugin_browser_modal.cpp`, `settings_modal.cpp`, `practice_tools_modal.cpp`). While the modal functions in `rack_view.cpp` currently have hardcoded `ImVec4` calls, the non-modal core rack components (ribbon, cards, scene bar, parallel branches) are firmly within Milestone 3 scope and must be migrated to `themeTokens()`.
- Dynamic monitor DPI switching during runtime (`WM_DPICHANGED`) resizes and recenters the window; runtime font atlas re-rasterization across different monitor DPIs is left to ImGui's startup rasterization, which is standard behavior for single-viewport DirectX 11 ImGui hosts.

---

## 4. Conclusion

**Verdict: REQUEST_CHANGES**

The work product contains two critical issues and one major testing fidelity issue:
1. **Critical [INTEGRITY VIOLATION / INCOMPLETE REFACTORING]**: 55 hardcoded `ImVec4` color literals remain in `src/ui/rack_view.cpp`. This causes the plugin title to display near-white text on cream backgrounds in the `VintageConsole` theme (~1.05:1 contrast), violating WCAG AA. The scanner `check_hardcoded_colors.py` masked this by checking only `IM_COL32`.
2. **Critical [MEMORY SAFETY]**: ~2.3 MB font heap memory leak in `src/main.cpp` when loading `seguisym.ttf` via `AddFontFromFileTTF` due to `symConfig.FontDataOwnedByAtlas = false`.
3. **Major [TESTING FIDELITY]**: Synthetic self-certifying lambdas in `tests/test_praccy.cpp` with deadband margin constant mismatch (`20.0f` in test vs `16.0f` in `rack_view.cpp`).

### Required Remediation:
1. In `src/ui/rack_view.cpp`:
   - Replace all `ImVec4(...)` calls in the main rack UI with semantic tokens from `themeTokens()` (e.g. `tokens.text.primary`, `tokens.text.secondary`, `tokens.text.muted`, `tokens.text.error`, `tokens.surfaces.cardBg`, `tokens.surfaces.panelBg`, `tokens.signal.tunerInTune`, `tokens.signal.tunerFlat`, `tokens.signal.active`, `tokens.signal.faulted`, `tokens.signal.bypassed`).
   - Specifically fix `renderPluginSlot` (lines 1451–1453) to use `tokens.text.primary` (or `tokens.signal.bypassed` / `tokens.signal.faulted`) instead of hardcoded `ImVec4(0.98f, 0.98f, 1.0f, 1.0f)`.
2. In `scripts/check_hardcoded_colors.py`:
   - Expand the scanner to detect undeclared raw `ImVec4(...)` color literals in `src/ui/` in addition to `IM_COL32(...)`.
3. In `src/main.cpp` (lines 260–269):
   - Set `symConfig.FontDataOwnedByAtlas = true;` before calling `AddFontFromFileTTF`, so that ImGui frees the memory allocated by `ImFileLoadToMemory`.
4. In `tests/test_praccy.cpp` & `src/ui/rack_view.cpp`:
   - Reconcile the deadband minimum margin constant between `rack_view.cpp` (line 1211) and `test_praccy.cpp` (line 1357), ensuring consistency (e.g., both 16.0f or both 20.0f).

---

## 5. Verification Method

To independently verify after remediation:

1. **Verify No Hardcoded Color Literals in `src/ui/`**:
   Execute:
   ```powershell
   Select-String -Path src\ui\rack_view.cpp -Pattern "ImVec4\s*\("
   ```
   Expected: Zero hardcoded `ImVec4` color calls in rack view rendering (or exclusively in modals deferred to M4).
   Execute updated color scanner:
   ```powershell
   py scripts/check_hardcoded_colors.py
   ```
   Expected: Exits with code 0.

2. **Verify Font Memory Leak Elimination in `src/main.cpp`**:
   Inspect lines 260–269 of `src/main.cpp`.
   Ensure `symConfig.FontDataOwnedByAtlas == true` when calling `AddFontFromFileTTF`.

3. **Verify WCAG AA Compliance Across All 4 Themes**:
   Ensure `displayName` uses `tokens.text.primary`. On `VintageConsole`, verify contrast ratio against `tokens.surfaces.cardBg` is $\ge 4.5:1$.

4. **Run Unit Tests & Regression Suite**:
   ```powershell
   .\build\test_praccy.exe
   $env:PATH = "C:\Users\Bartek\w64devkit\bin;" + $env:PATH
   ctest --test-dir build -C Release --output-on-failure
   cmake --build build --config Release --target Praccy
   ```
   Expected: 24/24 unit tests pass, 5/5 CTest targets pass, `Praccy.exe` builds cleanly with zero compiler warnings under `/W4 /WX` / `-Wall -Wextra -Werror`.
