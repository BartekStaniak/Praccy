# BRIEFING — 2026-10-06T18:35:00Z

## Mission
Survey SecOps, Crash Isolation, and Core Infrastructure (Requirement R2 and associated ACs) for Praccy v2.0 Architectural Blueprint.

## 🔒 My Identity
- Archetype: explorer
- Roles: codebase investigation, SecOps, crash isolation, core infrastructure survey
- Working directory: f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_2
- Original parent: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Milestone: Praccy v2.0 Architectural Blueprint Survey

## 🔒 Key Constraints
- Read-only investigation — do NOT implement
- Deliver structured handoff.md following 5-component protocol (Observation, Logic Chain, Caveats, Conclusion, Verification Method)

## Current Parent
- Conversation ID: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Updated: not yet

## Investigation State
- **Explored paths**:
  - `src/ui/update_checker.cpp` & `update_checker.h`
  - `src/plugins/vst3_host.cpp`, `vst3_host.h`, `clap_host.cpp`, `clap_host.h`, `plugin_window.cpp`
  - `src/audio/graph_engine.cpp`, `graph_engine.h`
  - `src/state/scene_manager.cpp`, `scene_manager.h`, `app_config.cpp`, `app_config.h`
  - `src/main.cpp`
  - `CMakeLists.txt`, `tests/test_praccy.cpp`, `tests/test_asio_driver.cpp`
- **Key findings**:
  - `std::system` invokes `tar` (line 331) and `powershell.exe` (line 337) in `update_checker.cpp`; `cmd.exe` in line 419. `miniz` or Win32 Shell API resolves extraction; updater restart should eliminate `cmd.exe`.
  - VST3/CLAP `process()` and GUI calls lack SEH isolation. MSVC requires leaf functions without local C++ unwind objects (error C2712). MinGW GCC requires VEH (`AddVectoredExceptionHandler`).
  - Throwing `std::stoul` (lines 28, 540, 560, 571) and `std::stof` in `scene_manager.cpp` cause unhandled crashes on malformed `presets.ini`; `std::from_chars` resolves this.
  - Window geometry is hardcoded in `main.cpp:64` and not persisted. Win32 `GetWindowPlacement`/`SetWindowPlacement` needed.
  - Build environment is MinGW-w64 GCC 16.2.0 + GNU Make 4.4.1. `CMakeLists.txt` lacks `/WX` for MSVC, needs `SYSTEM` includes for third-party headers, and needs warning fixes in `asio_manager.cpp` and `vst3_host.cpp`.
- **Unexplored areas**: None for survey scope.

## Key Decisions Made
- Fully documented all 5 survey dimensions with exact source code lines, compiler verification outputs, and concrete implementation blueprints.

## Artifact Index
- f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_2/DISPATCH.md — Received dispatch instructions
- f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_2/BRIEFING.md — Working memory
- f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_2/progress.md — Liveness heartbeat & task progress
- f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_2/handoff.md — 5-component survey handoff report
