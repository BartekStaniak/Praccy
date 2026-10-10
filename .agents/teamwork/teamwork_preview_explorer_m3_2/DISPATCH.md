## 2026-10-07T07:15:15Z
[Message] timestamp=2026-10-07T07:15:15Z sender=6d04231a-d33e-4b26-b49d-7f9feca2b265 priority=MESSAGE_PRIORITY_HIGH content=You are an Explorer subagent for Milestone 3 (Unified Design Tokens & Responsive Canvas - Requirement R3) of the Praccy v2.0 Architectural Blueprint project.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m3_2/.
Project root is f:/Projects/Praccy.

You MUST read these foundational documents before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_spec_miner_survey_3/handoff.md

Your mission:
Formulate the concrete architectural implementation blueprint for Feature 16 (Embedded Typography Engine via Win32 Resources):
1. Typography Strategy & Resource Embedding:
   - Investigate TTF font bundling: Inter (for UI text/headers) and JetBrains Mono (for numeric parameters, DSP readouts, and monospace values). Check if font TTF files or compressed binary buffers can be provided in `resources/fonts/`.
   - Update `resources/praccy.rc` with `RT_RCDATA` resource definitions:
     `IDR_FONT_INTER RCDATA "fonts/Inter-Regular.ttf"`
     `IDR_FONT_JETBRAINS_MONO RCDATA "fonts/JetBrainsMono-Regular.ttf"`
   - Define resource IDs (`IDR_FONT_INTER 201`, `IDR_FONT_JETBRAINS_MONO 202`) in a shared header (`resources/resource.h` or similar).
2. Resource Extraction & Atlas Integration:
   - Design Win32 resource loading in `src/main.cpp`: invoke `FindResourceW`, `LoadResource`, `LockResource`, and `SizeofResource`.
   - Pass memory pointers to `io.Fonts->AddFontFromMemoryTTF(data, size, fontSize, &fontConfig, glyphRanges)`. Set `fontConfig.FontDataOwnedByAtlas = false` to prevent ImGui from attempting to `free()` read-only PE resource memory.
   - Expose global font pointers: `ImFont* g_fontUI` (Inter at ~16-17px) and `ImFont* g_fontMono` (JetBrains Mono at ~14-15px).
   - Provide robust fallback: if resource loading fails (e.g. running in headless test harness or non-Windows build), gracefully fall back to `io.Fonts->AddFontDefault()` without crash.
3. High-DPI Canvas Scaling:
   - Blueprint DPI scaling integration: calculate monitor DPI scale factor in `src/main.cpp` via `GetDpiForWindow` / `GetDpiForMonitor` and scale font sizes, frame padding, and item spacing proportionally across 1080p (100%), 1440p (125%), and 4K (150%, 200%).

Deliverable:
Author your comprehensive implementation blueprint at f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m3_2/handoff.md following the Handoff Protocol (Observation, Logic Chain, Caveats, Conclusion, Verification Method). Do NOT modify source code files. Update progress.md in your working directory and notify parent via send_message when complete.
