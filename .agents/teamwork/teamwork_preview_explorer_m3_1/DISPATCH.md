## 2026-10-07T07:15:15Z
You are an Explorer subagent for Milestone 3 (Unified Design Tokens & Responsive Canvas - Requirement R3) of the Praccy v2.0 Architectural Blueprint project.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m3_1/.
Project root is f:/Projects/Praccy.

You MUST read these foundational documents before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_spec_miner_survey_3/handoff.md

Your mission:
Formulate the concrete architectural implementation blueprint for Feature 14 (Unified Design Tokens & 4 WCAG AA Themes) and Feature 15 (Static Hardcoded Color Scanner):
1. Design `src/ui/design_tokens.h`:
   - Declare token structs: `SurfaceTokens`, `BorderTokens`, `TextTokens`, `SignalStateTokens`, `CableTokens`, `ThemeTokens`.
   - Provide an enum `ThemeId { ObsidianStudio, CyberMidnight, NordicSlate, VintageConsole }`.
   - Formulate exact 32-bit `ImU32` / `ImVec4` color values for all 4 themes.
   - Calculate and document mathematical WCAG AA contrast compliance (>= 4.5:1 for normal text, >= 3.0:1 for graphical UI elements) for each theme:
     - Obsidian Studio (charcoal/black with studio amber/warm accents)
     - Cyber / Midnight (neon cyan/magenta on midnight navy)
     - Nordic Slate (muted slate blue, soft frost white, scandinavian minimal)
     - Vintage Console (warm analog parchment/cream, bakelite knob accents, vintage tape saturation hues)
   - Expose global thread-safe token access: `const ThemeTokens& themeTokens()` and `void applyTheme(ThemeId id)`.
   - In `applyTheme(ThemeId)`, map tokens directly to `ImGui::GetStyle().Colors[...]` so standard ImGui controls naturally inherit theme aesthetics.
2. Design `scripts/check_hardcoded_colors.py`:
   - Design a Python script that traverses `src/ui/` (and subdirectories), detects any occurrences of raw `IM_COL32(...)` literals outside `src/ui/design_tokens.h` or token definitions.
   - Exits with 0 on clean, non-zero on violations, printing filename, line number, and offending code snippet.
3. Map out the systematic refactoring of existing raw `IM_COL32` calls in `src/ui/theme.h` and `src/ui/rack_view.cpp` (~170 instances) to use semantic tokens (`themeTokens().surfaces.*`, `themeTokens().borders.*`, etc.).

Deliverable:
Author your comprehensive implementation blueprint at f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m3_1/handoff.md following the Handoff Protocol (Observation, Logic Chain, Caveats, Conclusion, Verification Method). Do NOT modify source code files. Update progress.md in your working directory and notify parent via send_message when complete.
