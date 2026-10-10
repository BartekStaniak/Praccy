# BRIEFING — 2026-10-06T20:30:00Z

## Mission
Formulate concrete implementation blueprint for Features 10, 11, 12 & 13 (Non-throwing parsing, window placement persistence, compiler warning hardening, unit tests).

## 🔒 My Identity
- Archetype: Teamwork explorer
- Roles: Explorer, Architect, Blueprint Formulator
- Working directory: f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_3/
- Original parent: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Milestone: Milestone 2 (Praccy v2.0 Architectural Blueprint - Features 10, 11, 12, 13)

## 🔒 Key Constraints
- Read-only investigation — do NOT implement / modify source code files
- All findings, blueprint, code snippets, diffs must be documented in reports under working directory
- Write handoff.md following 5-Component Handoff Protocol
- Keep progress.md updated
- Notify parent via send_message when complete

## Current Parent
- Conversation ID: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Updated: not yet

## Investigation State
- **Explored paths**:
  - `f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md`
  - `f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md`
  - `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_2/handoff.md`
  - `src/state/scene_manager.h`, `src/state/scene_manager.cpp`
  - `src/state/app_config.h`, `src/state/app_config.cpp`
  - `src/main.cpp`
  - `src/audio/asio_manager.cpp`
  - `src/plugins/vst3_host.cpp`, `src/plugins/plugin_scanner.cpp`, `src/plugins/clap_host.cpp`
  - `third_party/vst3_pluginterfaces/base/funknown.cpp`
  - `CMakeLists.txt`
  - `tests/test_praccy.cpp`
- **Key findings**:
  - Feature 10: `scene_manager.cpp` has 4 `std::stoul` calls and 8 `std::stof` calls, plus `hexToBytes` using `std::stoul`. `app_config.cpp` has 1 `std::atoi` and 3 `std::atof`. All throwing/unchecked conversions are fully replaceable with non-throwing C++20 `std::from_chars`. Designed and verified `praccy::utils::parseInteger`, `parseFloat`, `parseDouble`, `hexToBytes` with trimming, leading `+` handling, and fallback defaults.
  - Feature 11: `AppConfig` has no window coordinate persistence. Win32 `GetWindowPlacement` and `SetWindowPlacement` provide decoupling of maximized state and restored rectangle. Designed `saveWindowPlacement(HWND)` and `restoreWindowPlacement(HWND)` with multi-monitor `MonitorFromRect(..., MONITOR_DEFAULTTONULL)` and fallback to primary monitor work area. Integration point in `src/main.cpp` replaces hardcoded `ShowWindow(hwnd, SW_SHOWDEFAULT)` without causing white window flash.
  - Feature 12: Tested all 15 first-party files under `-O3 -Wall -Wextra -Werror -Wno-unused-parameter`. Discovered that first-party code is 100% warning-clean except for: 1) `strncpy` in `asio_manager.cpp:48` (fixed via `snprintf`); 2) `FARPROC` casts in `vst3_host.cpp:176,181,317` and `plugin_scanner.cpp:265` (fixed by casting through `void*`). Discovered that third-party `funknown.cpp` has `%X` vs `long unsigned int` format warnings under strict flags, requiring `SYSTEM PRIVATE` include isolation and `-w`/`/W0` compile properties on third-party sources.
  - Feature 13: Designed comprehensive `testCorruptedPresetsIni()` in `tests/test_praccy.cpp` covering corrupted integer fields, garbage floats, truncated syntax, invalid hex strings, missing keys, and total file corruption with default scene fallback.
- **Unexplored areas**: None for Features 10-13 scope.

## Key Decisions Made
- Standardize non-throwing parsing utility in header-only `src/utils/parse_utils.h`.
- Protect `parse_utils.h` against C++ standard limitation where `std::from_chars` rejects leading `+`.
- Use `void*` intermediary cast to cleanly eliminate `FARPROC` `-Wcast-function-type` warnings under `-Wextra`.
- Use `MonitorFromRect` with `MONITOR_DEFAULTTONULL` plus `GetMonitorInfoW` fallback to ensure windows never restore off-screen on disconnected monitors.
- Recommend CMake configuration isolating third-party headers via `SYSTEM PRIVATE` and third-party sources via `COMPILE_OPTIONS` `/W0` and `-w`.

## Artifact Index
- `BRIEFING.md` — Working memory
- `DISPATCH.md` — Dispatch log
- `progress.md` — Progress tracking
- `handoff.md` — Final implementation blueprint deliverable
