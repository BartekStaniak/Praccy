## 2026-10-07T12:03:55Z
You are the Remediation Worker for Milestone 3 (Unified Design Tokens & Responsive Canvas - Requirement R3) of the Praccy v2.0 Architectural Blueprint project.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m3_fix/.
Project root is f:/Projects/Praccy.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

You MUST read these foundational and audit review documents before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_reviewer_m3_2/handoff.md (Details the 55 ImVec4 literals, VintageConsole contrast, and font memory leak)
4. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_challenger_m3_2/handoff.md (Details the 16px vs 20px deadband clamp and NaN/Inf sanitization)

Exclusive Write Ownership:
- src/ui/rack_view.cpp
- scripts/check_hardcoded_colors.py
- src/main.cpp
- tests/test_praccy.cpp
- tests/test_challenger_m3_2.cpp

Remediation Tasks:

1. Complete Elimination of Hardcoded `ImVec4(...)` Literals in `src/ui/rack_view.cpp`:
   - Replace all remaining raw `ImVec4(...)` calls across `src/ui/rack_view.cpp` with semantic tokens from `themeTokens()`:
     - In `renderPluginSlot` (lines 1450–1453):
       ```cpp
       const ImVec4 titleColor = isFaulted ? tokens.signal.faulted.vec4 :
                                 (bypassed ? tokens.signal.bypassed.vec4 : tokens.text.primary.vec4);
       ImGui::TextColored(titleColor, "%s", displayName.c_str());
       ```
       This ensures plugin titles on `ThemeId::VintageConsole` cream cards (`#F8F5EE`) use dark text (`tokens.text.primary`), satisfying WCAG AA (>= 4.5:1 contrast).
     - Ribbon module background (use `tokens.surfaces.panelBg.vec4`).
     - Tuner header (`tokens.text.muted.vec4`), note color (`inTune ? tokens.signal.tunerInTune.vec4 : tokens.signal.tunerFlat.vec4`).
     - Metronome header (`tokens.text.muted.vec4`).
     - Noise gate header (`tokens.text.muted.vec4`).
     - Master output header and volume readout (`tokens.text.primary.vec4`).
     - Scene presets bar header and active buttons (`tokens.text.secondary.vec4`, `tokens.signal.active.vec4`, `tokens.text.primary.vec4`).
     - Rack header (`tokens.text.muted.vec4`).
     - Fault badge (`tokens.signal.faulted.vec4`).
     - Branch A and B Mute, Solo, Phase buttons (`tokens.signal.faulted.vec4`, `tokens.signal.active.vec4`, `tokens.signal.accent.vec4`).
     - Branch delete text (`tokens.text.error.vec4`).
     - Branch insert child background (`tokens.surfaces.cardBg.vec4`).
     - Combiner header (`tokens.text.accent.vec4`).

2. Expand Scanner (`scripts/check_hardcoded_colors.py`):
   - Update `scripts/check_hardcoded_colors.py` to scan for raw `ImVec4\s*\(` color literals in addition to `IM_COL32\s*\(` in `src/ui/` (excluding `design_tokens.h`).
   - Run `python scripts/check_hardcoded_colors.py` and verify it reports 0 violations across `src/ui/`.

3. Eliminate Font Heap Memory Leak in `src/main.cpp`:
   - In lines 260–269 (`seguisym.ttf` loading via `AddFontFromFileTTF`), set:
     `symConfig.FontDataOwnedByAtlas = true;`
     before calling `AddFontFromFileTTF`. Because `AddFontFromFileTTF` internally allocates a 2.3 MB heap buffer via `ImFileLoadToMemory`, `FontDataOwnedByAtlas = true` ensures Dear ImGui frees this buffer in `ImFontAtlas::ClearInputData()`, completely eliminating the 2.3 MB heap leak. (Keep `fontConfig.FontDataOwnedByAtlas = false` for the PE `.rsrc` embedded fonts Inter and JetBrains Mono).

4. Viewport Deadband Clamp & NaN/Inf Sanitization in `src/ui/rack_view.cpp`:
   - In `RackView::renderSignalRack()` (lines 1211–1212):
     ```cpp
     const float safeW = (std::isnan(viewportWidth) || viewportWidth <= 0.0f) ? 0.0f : viewportWidth;
     const float safeH = (std::isnan(viewportHeight) || viewportHeight <= 0.0f) ? 0.0f : viewportHeight;
     const float offsetX = std::max(20.0f, (safeW - totalContentWidth) * 0.5f);
     const float offsetY = std::max(16.0f, (safeH - totalContentHeight) * 0.5f);
     ```
     Clamp minimum horizontal deadband to 20.0f (aligning with specification and `tests/test_praccy.cpp` line 1357) and sanitize against NaN/Inf.
   - In `drawCubicHermiteCable`:
     ```cpp
     const float safePeak = (!std::isfinite(signalPeak) || signalPeak < 0.0f) ? 0.0f : std::clamp(signalPeak, 0.0f, 1.0f);
     const float safeTime = (!std::isfinite(animTime)) ? 0.0f : animTime;
     ```
     Sanitize `signalPeak` and `animTime` against NaN/Inf so Dear ImGui draw lists receive finite coordinates and radii.

5. Build and Verify:
   - Run `python scripts/check_hardcoded_colors.py` (assert 0 violations).
   - Build all targets: `make test_praccy`, `make Praccy`, `make test_challenger_m3_2`.
   - Run `.\build\test_praccy.exe` (assert 24/24 pass).
   - Run `.\build\test_challenger_m3_2.exe` (assert 0 deadband mismatches and all tests pass).
   - Run `ctest --test-dir build -C Release --output-on-failure` (assert 100% pass).
   - Ensure clean compilation with zero warnings under `-Wall -Wextra -Werror` / `/W4 /WX`.

Deliverable:
Author your completion report at f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m3_fix/handoff.md following the Handoff Protocol (Observation, Logic Chain, Caveats, Conclusion, Verification Method). Include build and test execution results. Update progress.md in your working directory and notify parent via send_message when complete.
