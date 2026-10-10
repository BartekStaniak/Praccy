## 2026-10-06T20:34:24Z
You are the Implementation Worker for Milestone 2 (SecOps, Hardening & Crash Isolation) of the Praccy v2.0 Architectural Blueprint project.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m2/.
Project root is f:/Projects/Praccy.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

You MUST read these foundational and architectural blueprint files before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_1/handoff.md (Features 7 & 8: miniz in-process extraction & direct restart blueprint)
4. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_2/handoff.md (Feature 9: Win32 SEH/VEH plugin crash isolation blueprint)
5. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_3/handoff.md (Features 10, 11, 12, 13: std::from_chars parsing, window placement, compiler hardening blueprint)

Exclusive Write Ownership:
- third_party/miniz/miniz.h
- third_party/miniz/miniz.c
- src/ui/update_checker.h
- src/ui/update_checker.cpp
- src/plugins/crash_isolation.h
- src/plugins/plugin_base.h
- src/plugins/vst3_host.cpp
- src/plugins/clap_host.cpp
- src/utils/parse_utils.h
- src/state/scene_manager.cpp
- src/state/app_config.h
- src/state/app_config.cpp
- src/audio/asio_manager.cpp
- src/plugins/plugin_scanner.cpp
- src/ui/rack_view.cpp
- src/main.cpp
- CMakeLists.txt
- tests/test_praccy.cpp

Implementation Tasks:

1. Features 7 & 8 (In-Process ZIP Extraction & Direct Updater Execution):
   - Vendor standard amalgamated `miniz` in `third_party/miniz/miniz.h` and `third_party/miniz/miniz.c`.
   - In `src/ui/update_checker.h` and `src/ui/update_checker.cpp`:
     - Implement `sanitizeZipEntryPath()` with rigorous Zip Slip defense: normalize backslashes to forward slashes; reject leading slashes, drive letters, and UNC prefixes; reject `.` and `..` path tokens; reject invalid Windows filename chars (`<`, `>`, `:`, `"`, `|`, `?`, `*`) and control characters; reject DOS device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`); verify destination prefix containment via `std::filesystem::weakly_canonical`.
     - Implement `extractZipArchive()` using `miniz` APIs and `_wfopen(..., L"rb")`. Enforce Zip Bomb guardrails (reject single file > 250 MB, total size > 500 MB, file count > 10,000).
     - Refactor `applyUpdateAndRestart()`: completely eliminate `std::system()`, `tar`, `powershell.exe`, `apply_update.bat`, and `cmd.exe`. Launch the extracted binary directly with Win32 `CreateProcessW`: `"Praccy.exe" --apply-update <currentPid> "<currentExePath>"`.
   - In `src/main.cpp`:
     - In `WinMain`, parse command-line arguments using `CommandLineToArgvW` (link `shell32`).
     - If `--apply-update <oldPid> "<destExe>"` is received:
       - Wait for old process exit using `OpenProcess(SYNCHRONIZE, FALSE, oldPid)` and `WaitForSingleObject(hProcess, 15000)`.
       - Overwrite destination binary using `CopyFileW` with a 20-attempt retry loop (100ms sleep between attempts).
       - Recursively sync `resources/` to destination using `std::filesystem::copy(..., copy_options::recursive | copy_options::overwrite_existing)`.
       - Relaunch the updated application at `destExe` with Win32 `CreateProcessW` and exit immediately via `ExitProcess(0)`.
   - Confirm ZERO occurrences of `std::system`, `cmd.exe`, or `powershell.exe` remain in the code.

2. Feature 9 (Win32 SEH/VEH Plugin Crash Isolation):
   - Create `src/plugins/crash_isolation.h`:
     - On MSVC (`_MSC_VER`): Isolate `__try` / `__except` into non-template leaf function `sehExecuteLeaf(void (*fn)(void*), void* arg, DWORD* outCode)` with zero C++ local objects/destructors, strictly complying with MSVC rule C2712.
     - On MinGW GCC (`!_MSC_VER`): Use Win32 Vectored Exception Handling (`AddVectoredExceptionHandler`) registered once with an intrusive thread-local context stack (`t_currentCrashContext`) and `setjmp`/`longjmp`. Real-time safe, <15ns overhead, zero heap allocation.
   - In `src/plugins/plugin_base.h`, add `std::atomic<bool> m_faulted{false}`, `std::string m_faultReason`, `bool isFaulted() const`, and `void resetFault()`.
   - Wrap untrusted plugin boundaries in `src/plugins/vst3_host.cpp` and `src/plugins/clap_host.cpp`:
     - Wrap `process()`. If crash occurs: latch `m_faulted = true`, immediately copy input to output (`ctx.output.copyFrom(ctx.input)`). On all subsequent blocks, bypass DSP immediately and pass dry audio.
     - Wrap `openGui()`, `closeGui()`, parameter setting, and state serialization.
   - In `src/ui/rack_view.cpp`:
     - Visually flag faulted plugin slot: render crimson accent / alert badge and `[RELOAD PLUGIN]` button.
   - In `tests/test_praccy.cpp`:
     - Add `CrashingMockPlugin` and unit test verifying that a hardware access violation (`0xC0000005`) is trapped cleanly, host does not crash, and dry audio pass-through is active.

3. Features 10, 11, 12 & 13 (Non-Throwing Parsing, Window Placement, Compiler Hardening & Tests):
   - Create `src/utils/parse_utils.h` providing non-throwing string-to-number functions (`parseInt`, `parseFloat`, `parseHexByte`, etc.) using C++20 `std::from_chars` with whitespace trimming, optional leading `+` handling, and fallback defaults.
   - Replace all throwing `std::stoul`, `std::stof`, `std::stoi`, `std::atoi`, `std::atof` in `src/state/scene_manager.cpp` and `src/state/app_config.cpp`.
   - Window placement persistence:
     - Persist `WINDOWPLACEMENT` in `src/state/app_config.h` and `src/state/app_config.cpp`.
     - Validate geometry using `MonitorFromRect(..., MONITOR_DEFAULTTONULL)`, falling back to primary monitor work area.
     - In `src/main.cpp`, restore window placement via `SetWindowPlacement` after swapchain pre-rendering to eliminate white window flash.
   - Compiler warning hardening:
     - In `CMakeLists.txt`, enable `/W4 /WX` on MSVC, `-Wall -Wextra -Werror` on GCC.
     - Isolate third-party libraries (`third_party`) as `SYSTEM PRIVATE` and compile third-party files (`miniz.c`, `funknown.cpp`, `imgui.cpp`) with warning suppression (`/W0`, `-w`).
     - Link `shell32` in `CMakeLists.txt`.
     - Fix first-party latent warnings: `snprintf` in `asio_manager.cpp:48`, double-cast `FARPROC` through `void*` in `vst3_host.cpp` and `plugin_scanner.cpp`.
   - In `tests/test_praccy.cpp`:
     - Add `testCorruptedPresetsIni()` verifying safe non-throwing degradation across corrupted numeric values, broken syntax, invalid hex strings, and missing fields.

4. Build and Test Verification:
   - Build all targets (`test_praccy`, `Praccy`).
   - Run `test_praccy.exe` and assert all tests pass with exit code 0.
   - Ensure the build compiles with zero warnings under the strict compiler flags.
