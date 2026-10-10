# Execution Plan: Praccy v2.0 Architectural Blueprint

## Goal
Deliver complete, verified implementation of Praccy v2.0 Architectural Blueprint (PRAC-2026-V2-SPEC), fulfilling R1, R2, R3, R4 and passing 100% of headless/unit/E2E test suites with zero warnings and strict forensic integrity.

## Phase Overview

### Phase 0: Survey & Scope Mapping
- Dispatch 3 Explorers / Spec Miners in parallel:
  1. `explorer_survey_dsp`: Investigate audio DSP subsystem, ParallelBranch, InstrumentTuner, ASIO manager, third_party/readerwriterqueue, build configuration, tests.
  2. `explorer_survey_secops`: Investigate update_checker, VST3/CLAP hosting exception handling, scene_manager/app_config string parsing, window placement.
  3. `spec_miner_ui`: Investigate design tokens, fonts/resources, rack_view layout/splines, modal dialogs, plugin browser, looper, preset hotkeys.
- Synthesize findings into `PROJECT.md` at orchestrator directory/project root.

### Phase 1: Dual Track Setup & E2E Testing Infra
- Establish test harness and build verification commands via test writer/worker.
- Formulate Tier 1-4 tests according to PRAC-2026-V2-SPEC acceptance criteria.

### Phase 2: Milestone 1 — Audio DSP Concurrency & Real-Time Engine (R1)
- Lock-free SPSC command queue for `ParallelBranch::m_slots` via `readerwriterqueue`.
- Decoupled `InstrumentTuner` YIN pitch detection (60Hz thread + lock-free ring buffer).
- 24-bit unpack routines (`ASIOSTInt24LSB`, `ASIOSTInt32LSB24`) & `AvSetMmThreadCharacteristicsW`.
- Pre-allocated tuner buffers in `prepare()`, zero dynamic allocations in audio callback.
- Full verification: Reviewers, Challengers, Forensic Auditor.

### Phase 3: Milestone 2 — SecOps, Hardening & Crash Isolation (R2)
- Remove `std::system` (tar/powershell) in update checker; use in-process extraction.
- Win32 SEH (`__try`/`__except`) wrapping VST3/CLAP `process()` and GUI calls.
- Non-throwing `std::from_chars` in `scene_manager.cpp` and `app_config.cpp`.
- Window placement persistence via `GetWindowPlacement`/`SetWindowPlacement`.
- Full verification: Reviewers, Challengers, Forensic Auditor.

### Phase 4: Milestone 3 — Unified Design Tokens & Responsive Canvas (R3)
- `src/ui/design_tokens.h` with 4 production themes (Obsidian Studio, Cyber/Midnight, Nordic Slate, Vintage Console).
- Inter and JetBrains Mono embedded binary resources via `resources/praccy.rc` and loaded in `src/main.cpp`.
- Dynamic viewport centering in `RackView::renderSignalRack()`.
- Cubic Hermite spline cables with distance-adaptive tangents and animated pulse dots.
- Full verification: Reviewers, Challengers, Forensic Auditor.

### Phase 5: Milestone 4 — Modular UI Refactoring & Practice Suite Overhaul (R4)
- Split modal dialogs into `plugin_browser_modal.cpp`, `settings_modal.cpp`, `practice_tools_modal.cpp`.
- Redesign plugin cards (240x224px, parameter readouts, thumbnail frames, pill toggles).
- Spotlight-style command palette plugin browser with fuzzy search.
- Native Win32 `IFileOpenDialog` (`FOS_PICKFOLDERS`) for search paths.
- Circular progress ring looper + WAV drag-and-drop (`WM_DROPFILES`).
- Click-free crossfade ramping (10ms EqualPowerRamp) + floating HUD toast (1.8s fade).
- Full verification: Reviewers, Challengers, Forensic Auditor.

### Phase 6: Final Milestone & Adversarial Hardening
- Run complete headless audio regression harness (20,000 blocks, mock ASIO, zero corruption/NaN/deadlocks).
- Check zero hardcoded `IM_COL32` literals, WCAG AA contrast compliance, `/W4 /WX` build cleanliness.
- Adversarial coverage audit & hardening.
- Report completion to Sentinel.
