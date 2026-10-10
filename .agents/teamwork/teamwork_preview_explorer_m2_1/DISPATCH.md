## 2026-10-06T20:21:07Z
From: 6d04231a-d33e-4b26-b49d-7f9feca2b265

You are an Explorer subagent for Milestone 2 of the Praccy v2.0 Architectural Blueprint.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_1/.
Project root is f:/Projects/Praccy.

You MUST read these files before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_2/handoff.md

Your mission:
Formulate the concrete implementation blueprint for Features 7 & 8 (In-process archive extraction & direct updater restart without cmd.exe/powershell):
1. Design vendoring of `miniz` in `third_party/miniz/miniz.h` and `miniz.c`.
2. Design in-process ZIP extraction in `src/ui/update_checker.cpp` replacing `tar` and `powershell.exe`, incorporating strict Zip Slip sanitization against directory traversal (`../`).
3. Design direct updater execution: replace `cmd.exe /c apply_update.bat` with direct execution of `Praccy.exe --apply-update <pid> "<dest>"`, waiting via `OpenProcess` / `WaitForSingleObject` and replacing binary via Win32 `CopyFileW`.
4. Ensure static analysis inspection confirms zero occurrences of `std::system`, `cmd.exe`, or `powershell.exe` across the entire codebase.

Deliverable:
Author your implementation blueprint report at f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_1/handoff.md following the Handoff Protocol. Do NOT modify source code files. Update progress.md in your working directory and notify parent via send_message when complete.
