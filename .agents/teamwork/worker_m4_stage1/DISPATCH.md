## 2026-10-12T18:40:34Z
You are the Visual Polish, Design Tokens & List Virtualization Worker (Milestone 4) for Praccy V2.4 Stage 1.
Your working directory is F:\Projects\Praccy\.agents\teamwork\worker_m4_stage1.

MANDATORY: Read the authoritative user request at:
F:\Projects\Praccy\.agents\teamwork\ORIGINAL_REQUEST.md
Also read the project architecture and interface contracts at:
F:\Projects\Praccy\.agents\teamwork\PROJECT.md
And the UI survey report at:
F:\Projects\Praccy\.agents\teamwork\explorer_survey_ui\survey_report.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Your write ownership (exclusive):
- src/ui/modals/plugin_browser_modal.h
- src/ui/modals/plugin_browser_modal.cpp
- src/ui/design_tokens.h
- src/ui/theme.h
- src/ui/theme.cpp

Objectives for Milestone 4:
1. Implement Spotlight List Virtualization (`src/ui/modals/plugin_browser_modal.cpp`):
   - Wrap row rendering in `ImGuiListClipper` (`clipper.Begin(static_cast<int>(m_filteredItems.size())); while (clipper.Step()) { for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) { ... } }`).
   - Maintain constant 144 FPS across 1,500+ plugins.
2. Resolve High-DPI Footer Layout text collisions:
   - Replace hardcoded `SameLine(modalSize.x - 170.0f)` with dynamic `CalcTextSize` and `GetContentRegionAvail().x` measurement.
   - Eliminate all footer text collisions across 100%, 125%, 150%, and 200% DPI.
3. Implement Spotlight Favorites & Pinned Scope:
   - Tab key cycles scopes: `All` -> `Favorites` -> `Recents`.
   - `F` key toggles star / favorite status for the highlighted plugin.
   - Zero-query pinned sorting and search score boosting for favorites.
4. Polish Visual Hierarchy & Design Tokens (`src/ui/design_tokens.h`, `theme.h`):
   - Eliminate VST3 double-badging and suffix clutter in plugin browser rows.
   - Deepen modal window dimming scrim to 0.75 alpha (`IM_COL32(0, 0, 0, 191)`).
   - Restyle active scene button to dark emerald background (`#104B2D`, `IM_COL32(16, 75, 45, 255)`) with focus border.
   - Promote looper idle text contrast to `#9CA3AF` (`IM_COL32(156, 163, 175, 255)`).
   - Standardize top ribbon modules to FramePadding 6x4.
5. Strict Design Token Compliance:
   - ALL colors must be sourced from `design_tokens.h` semantic token structures.
   - Run `py scripts/check_hardcoded_colors.py` -> MUST report 0 violations.
   - Run `py scripts/wcag_audit.py` -> MUST pass.
   - Build project with `cmake --build build --config Release`.

Document all changes and test outputs in `handoff.md` in your working directory.
When finished, send a completion message with handoff details to your orchestrator.
