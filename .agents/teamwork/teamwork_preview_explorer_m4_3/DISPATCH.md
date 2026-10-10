## 2026-10-07T12:24:19Z
You are Explorer 3 for Milestone 4 (Modular UI Refactoring & Practice Suite Overhaul - Requirement R4) of the Praccy v2.0 Architectural Blueprint project.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m4_3/.
Project root is f:/Projects/Praccy.

You MUST read these foundational documents before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/orchestrator/GATE_STATUS.md
4. f:/Projects/Praccy/src/tools/quick_looper.h, f:/Projects/Praccy/src/tools/audio_player.h, f:/Projects/Praccy/src/state/scene_manager.h

Your Mission:
Formulate the concrete implementation blueprint for Feature 24 (Quick Looper Circular Progress Ring), Feature 25 (WAV Drag-and-Drop `WM_DROPFILES`), Feature 26 (Click-Free `EqualPowerRamp` Preset Switching), and Feature 27 (Floating HUD Toast Notification):
1. Feature 24: Quick Looper Circular Progress Ring:
   - In `QuickLooper` rendering (`practice_tools_modal.cpp` or looper card):
     - Render smooth circular progress ring visualizing playback position ($\theta = 2\pi \cdot \frac{\text{currentSample}}{\text{loopLength}}$).
     - Color-coded state transitions: Recording (Red `tokens.signal.faulted`), Overdubbing (Amber `tokens.signal.accent`), Playing (Green `tokens.signal.active`), Stopped (Muted `tokens.text.muted`).
     - Render using Dear ImGui `ImDrawList::PathArcTo` and `ImDrawList::PathStroke`.
2. Feature 25: WAV File Drag-and-Drop (`WM_DROPFILES`):
   - In `src/main.cpp`:
     - Enable drag-and-drop on main window: `DragAcceptFiles(hwnd, TRUE)`.
     - In `WndProc`, handle `WM_DROPFILES`: query file path via `DragQueryFileW`, check if extension is `.wav`, load into `AudioPlayer` or `QuickLooper`, and call `DragFinish(hDrop)`.
3. Feature 26: 10ms Click-Free `EqualPowerRamp` Preset Crossfading:
   - In `src/audio/graph_engine.h` / `graph_engine.cpp` and `scene_manager.cpp`:
     - On scene recall (hotkeys 1–8), apply 10ms equal-power crossfade curve ($g_{\text{out}}(t) = \cos(\frac{\pi}{2}t)$, $g_{\text{in}}(t) = \sin(\frac{\pi}{2}t)$) over $0.010 \times \text{sampleRate}$ samples.
     - Prevent discontinuities or pop/click artifacts during plugin parameter or topology transition.
4. Feature 27: Floating HUD Toast Notification:
   - On scene preset recall (keys 1–8), trigger a floating HUD notification ("PRESET [N] RECALLED").
   - Render in center-top of main viewport using `ImDrawList` overlay with rounded background, `tokens.surfaces.cardBg`, and `tokens.text.primary`.
   - Alpha fade-out over 1.8 seconds with zero layout shift and non-blocking input (`ImGuiWindowFlags_NoInputs`).
5. Feature 28 & Unit Tests (`tests/test_praccy.cpp`):
   - Design automated tests verifying:
     - `EqualPowerRamp` energy conservation: $g_{\text{out}}^2 + g_{\text{in}}^2 \approx 1.0$ across all ramp points.
     - Quick Looper circular progress math ($\theta \in [0, 2\pi]$) and state transitions.
     - Drag-and-drop extension validation (`.wav` accepted, invalid rejected).
     - Floating HUD alpha decay computation.

Deliverable:
Author your implementation blueprint report at f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m4_3/handoff.md following the Handoff Protocol. Do NOT modify source code files. Update progress.md in your working directory and notify parent via send_message when complete.
