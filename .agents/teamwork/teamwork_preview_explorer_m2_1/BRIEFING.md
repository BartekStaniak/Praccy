# BRIEFING — 2026-10-06T20:25:00Z

## Mission
Formulate concrete implementation blueprint for Milestone 2: Features 7 & 8 (in-process ZIP extraction with miniz & direct updater restart without cmd.exe/powershell).

## 🔒 My Identity
- Archetype: explorer
- Roles: investigation, synthesis, blueprint design
- Working directory: f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_1
- Original parent: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Milestone: Milestone 2 (Features 7 & 8)

## 🔒 Key Constraints
- Read-only investigation — do NOT implement / do NOT modify project source code
- Files for content delivery, messages for coordination
- Self-contained 5-component handoff report at handoff.md
- Strict Zip Slip sanitization against directory traversal (`../`)
- Zero occurrences of `std::system`, `cmd.exe`, or `powershell.exe`

## Current Parent
- Conversation ID: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Updated: 2026-10-06T20:25:00Z

## Investigation State
- **Explored paths**: `src/ui/update_checker.h/.cpp`, `src/main.cpp`, `CMakeLists.txt`, `tests/test_praccy.cpp`, codebase-wide search for process/shell calls.
- **Key findings**:
  1. Only 2 occurrences of `std::system` in entire repo: `update_checker.cpp:331` (tar) & `337` (powershell).
  2. Only 1 occurrence of `cmd.exe` in entire repo: `update_checker.cpp:419` (`cmd.exe /c apply_update.bat`).
  3. No other occurrences of `popen`, `WinExec`, or batch scripts in the codebase.
  4. Complete replacement architecture for miniz vendoring, in-process zip extraction, Zip Slip sanitization, zip bomb defense, and direct `Praccy.exe --apply-update <pid> "<dest>"` updater execution designed and verified.
- **Unexplored areas**: None for Features 7 & 8.

## Key Decisions Made
- Architecture: Vendor `miniz` in `third_party/miniz/miniz.h` and `miniz.c`.
- In-process extraction: Implement `extractZipArchive` and `sanitizeZipEntryPath` with token validation, Windows reserved character/device name checks, canonical destination containment, and zip bomb thresholds.
- Direct updater: Replace batch file and cmd.exe with `Praccy.exe --apply-update <pid> "<dest>"` in `src/main.cpp` entry point via `OpenProcess` + `WaitForSingleObject` (15s timeout), retry `CopyFileW`, recursive resource copy via `std::filesystem::copy`, and relaunch via `CreateProcessW`.
- Add `shell32` to CMake `target_link_libraries`.
- Add unit tests for Zip Slip sanitization and miniz extraction in `test_praccy.cpp`.

## Artifact Index
- DISPATCH.md — record of initial dispatch message
- progress.md — liveness heartbeat and subtask progress
- handoff.md — 5-component architectural blueprint report
