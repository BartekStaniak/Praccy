## 2026-10-07T12:24:19Z

Sender: 6d04231a-d33e-4b26-b49d-7f9feca2b265 (parent)
Priority: MESSAGE_PRIORITY_HIGH

You are Explorer 1 for Milestone 4 (Modular UI Refactoring & Practice Suite Overhaul - Requirement R4) of the Praccy v2.0 Architectural Blueprint project.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m4_1/.
Project root is f:/Projects/Praccy.

You MUST read these foundational documents before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/orchestrator/GATE_STATUS.md
4. f:/Projects/Praccy/src/ui/rack_view.h and f:/Projects/Praccy/src/ui/rack_view.cpp

Your Mission:
Formulate the concrete implementation blueprint for Feature 20 (Modal Translation Unit Decoupling) and Feature 21 (Redesigned 240x224px Plugin Cards):
1. Feature 20: Modal Translation Unit Decoupling:
   - Analyze the current monolithic modals in `src/ui/rack_view.cpp` (~1,000 lines of modal code).
   - Design extraction into dedicated translation units under `src/ui/modals/`:
     - `src/ui/modals/plugin_browser_modal.h` & `plugin_browser_modal.cpp`
     - `src/ui/modals/settings_modal.h` & `settings_modal.cpp`
     - `src/ui/modals/practice_tools_modal.h` & `practice_tools_modal.cpp`
   - Define minimal, clean interface contracts between `RackView` and each modal (passing references/context needed for state recall, scanning, settings, and playback).
   - Detail `CMakeLists.txt` changes to add `src/ui/modals/*.cpp` to both `Praccy` and any test targets.
2. Feature 21: Redesigned 240x224px Plugin Cards:
   - Standardize rack card dimensions to exactly 240x224px:
     - Header bar (height ~28px): format badge (VST3 / CLAP pill with `tokens.borders.focus`), plugin title with text truncation/ellipsis, sliding pill toggle switch (on/bypass).
     - Body section: active thumbnail/DSP frame glow (`tokens.signal.active` when playing, dimmed when bypassed, crimson `tokens.signal.faulted` when faulted).
     - Parameter readouts in monospace font (`g_fontMono`, JetBrains Mono).
     - Remove all inert dummy knobs; replace with functional or clean tactile controls (e.g. wet/dry mix slider or quick bypass/remove buttons).
   - Ensure 100% adherence to design tokens (`themeTokens()`) and WCAG AA contrast.

Deliverable:
Author your implementation blueprint report at f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m4_1/handoff.md following the Handoff Protocol. Do NOT modify source code files. Update progress.md in your working directory and notify parent via send_message when complete.
