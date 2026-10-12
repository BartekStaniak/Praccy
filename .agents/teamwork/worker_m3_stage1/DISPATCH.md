## 2026-10-12T18:48:50Z
You are the UI Ergonomics & Tactile Rack Physics Worker (Milestone 3) for Praccy V2.4 Stage 1.
Your working directory is F:\Projects\Praccy\.agents\teamwork\worker_m3_stage1.

MANDATORY: Read the authoritative user request at:
F:\Projects\Praccy\.agents\teamwork\ORIGINAL_REQUEST.md
Also read the project architecture and interface contracts at:
F:\Projects\Praccy\.agents\teamwork\PROJECT.md
And the UI survey report at:
F:\Projects\Praccy\.agents\teamwork\explorer_survey_ui\survey_report.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Your write ownership (exclusive):
- src/ui/rack_view.h
- src/ui/rack_view.cpp
- src/ui/modals/settings_modal.h
- src/ui/modals/settings_modal.cpp
- src/ui/modals/practice_tools_modal.h
- src/ui/modals/practice_tools_modal.cpp
- src/state/scene_manager.h
- src/state/scene_manager.cpp

Objectives for Milestone 3:
1. Rotary Knob Renderer (`src/ui/rack_view.cpp`):
   - Implement `renderRotaryKnob(const char* label, float* value, float minVal, float maxVal, float radius, const char* format, float defaultVal)`.
   - Double-click resets to `defaultVal`. Clean arc drawing using theme tokens.
2. Front-Panel Macro Mini-Knobs (4 per card in `renderPluginSlot()`):
   - Heuristic discovery matching parameter names:
     - Macro 0: `Gain/Drive` (regex `gain|drive|dist|input|preamp`)
     - Macro 1: `Tone/EQ` (regex `tone|treble|bass|mid|eq|filter|color|bright`)
     - Macro 2: `Volume/Master` (regex `volume|level|master|output|post`)
     - Macro 3: `Mix/Blend` (regex `mix|blend|dry|wet`)
   - Right-click context menu on mini-knob allowing manual reassignment from all available parameters of the plugin.
   - Record snapshot to `UndoManager` upon `ImGui::IsItemDeactivatedAfterEdit()`.
3. Horizontal Drag-and-Drop Card Reordering:
   - ImGui drag-and-drop physics using payload `"RACK_CARD_PAYLOAD"` on cards.
   - Ghost card preview during dragging.
   - On drop, execute reordering at audio block boundaries with micro-fade and push snapshot to `UndoManager`.
4. Global MIDI & Key Map Mode (`Ctrl+M`, `Ctrl+K`):
   - Toggle map mode with pulsating violet canvas border (`tokens.signal.active` or border tokens with pulsating alpha).
   - Click-to-learn for UI controls.
   - MANDATORY: All keyboard shortcuts (presets 1-8, map mode, undo/redo, riff capture, card shortcuts) MUST be strictly guarded by `!ImGui::GetIO().WantTextInput` so text editing in modals/inputs NEVER fires hotkeys.
5. Focused Card Shortcuts:
   - `[` / `]` for adjacent preset cycling with 10ms crossfade.
   - `Q` for opening Spotlight pre-filtered to current plugin.
   - `0` for bypass toggle.
6. Interactive 800-Point Overview Waveform Strip in Backing Track (`practice_tools_modal.cpp`):
   - Downsample audio to 800 peak min/max points, render overview strip with click-to-seek and draggable loop points.
7. Retroactive Riff UI Controls:
   - In `SettingsModal`: add duration selection (1, 2, 5, 10 min) and live RAM footprint HUD (`minutes * 60 * sampleRate * 2 * sizeof(float)`).
   - In `RackView` bottom dock: add `[★ Save Last Riff]` button and global `Ctrl+Shift+R` shortcut, saving to `%USERPROFILE%\Documents\Praccy\Riffs\` with floating HUD toast.
8. Instant A/B Tone Snapshot Comparison & Undo/Redo:
   - In `SceneBar`: add A/B toggle button switching seamlessly with 10ms click-free crossfade (`crossfadeToNodes`).
   - Wire `Ctrl+Z` (Undo) and `Ctrl+Y` / `Ctrl+Shift+Z` (Redo) into `UndoManager` with floating HUD toasts.
9. Verification:
   - Verify design token strictness: `py scripts/check_hardcoded_colors.py` must pass with 0 violations.
   - Build project: `cmake --build build --config Release`.
   - Run CTest: `ctest --output-on-failure` in `build/`.

Document all changes and test outputs in `handoff.md` in your working directory.
When finished, send a completion message with handoff details to your orchestrator.

## 2026-10-12T19:10:13Z
**Context**: Milestone 3 UI Ergonomics & Tactile Rack Physics Implementation
**Content**: Checking in on your progress. How is the implementation of the rotary knobs, front-panel macros, card drag-and-drop, map mode, and waveform strip proceeding?
**Action**: Please provide a brief status update on your current step and estimated completion.
