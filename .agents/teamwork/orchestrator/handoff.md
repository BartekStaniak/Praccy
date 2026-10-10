# Orchestrator Soft Handoff: Gen 1 -> Gen 2

**Predecessor**: Project Orchestrator Gen 1 (`orchestrator`)  
**Parent Conversation ID**: `666c14ba-a01d-4446-a221-7bb1f52aada8` (Sentinel)  
**Date**: 2026-10-06T20:33:00Z  
**Reason for Succession**: Reached cumulative spawn threshold (16 / 16 spawns completed). All subagents have finished and reported back.

---

## 1. Observation (State of the Project)

### 1.1 Milestone 1: Audio DSP Concurrency & Real-Time Engine (R1) — [DONE & GATE PASSED]
- **Lock-free SPSC Queue**: `ParallelBranch::m_slots` concurrency implemented with `third_party/readerwriterqueue/readerwriterqueue.h`. Command queue for add/remove and garbage return queue (`collectReclaimedSlots()`) guarantees plugin destructors never execute on the audio callback thread. Shadow registry rollback on queue saturation verified.
- **InstrumentTuner Decoupling**: Pitch detection runs on a dedicated 60 Hz background thread consuming from `AudioRingBuffer` via wait-free `pushSamples`. UI reads thread-safe snapshot via seqlock atomics (`currentResult()`). All heap allocations in the audio callback eliminated. Buffer size dynamically scales with sample rate (up to 192 kHz) to reliably detect low E (82.4 Hz) across all sample rates.
- **24-bit Formats & MMCSS**: Bit-exact `ASIOSTInt24LSB` (packed 3-byte) and `ASIOSTInt32LSB24` (4-byte container, 24-bit LSB-aligned) unpacking and packing routines implemented with NaN protection (NaN mapped to clean 0.0f). MMCSS `AvSetMmThreadCharacteristicsW(L"Pro Audio", ...)` registered on start and reverted on stop.
- **Verification**: All 14 tests in `test_praccy.exe` pass (100%). Stress test `test_challenger_m1.exe` passed with 0 audio thread allocations across 10,000+ blocks and 0 destructions. Multi-rate test `test_challenger_m1_2.exe` passed across 44.1k–192kHz. Gate status recorded as **PASS** in `GATE_STATUS.md` and `PROJECT.md`.

### 1.2 Milestone 2: SecOps, Hardening & Crash Isolation (R2) — [EXPLORATION COMPLETE]
Three specialized Explorers have fully surveyed the codebase and authored complete, self-contained implementation blueprints:

1. **Features 7 & 8 (In-Process ZIP Extraction & Direct Updater Execution)**:
   - **Report**: `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_1/handoff.md`
   - **Vendoring `miniz`**: Place standard single-header `miniz.h` and `miniz.c` into `third_party/miniz/`. Include as `SYSTEM` in CMake to avoid warning pollution.
   - **In-Process ZIP Extraction**: In `src/ui/update_checker.cpp`, replace `std::system("tar ...")` and PowerShell `Expand-Archive` with `extractZipArchive()` using `miniz` and `_wfopen`.
   - **Zip Slip Defense**: Multi-layer sanitization in `sanitizeZipEntryPath()`: normalizes backslashes, rejects absolute/drive/UNC paths, rejects `.`/`..` tokens, disallows invalid characters `<>:\"|?*`, forbids DOS device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`), and verifies `weakly_canonical` destination containment. Rejects ZIP bombs (>250MB single file, >500MB total, >10,000 files).
   - **Direct Updater Restart**: Completely eliminates `apply_update.bat`, `cmd.exe`, `powershell.exe`, and `std::system`. Extracted binary is executed directly via Win32 `CreateProcessW`: `"Praccy.exe" --apply-update <pid> "<destExe>"`. Top of `WinMain` in `src/main.cpp` intercepts this flag via `CommandLineToArgvW` (links `shell32`), waits up to 15s for the old process via `OpenProcess` + `WaitForSingleObject`, overwrites destination binary with a 20-attempt `CopyFileW` retry loop, syncs `resources/` via `std::filesystem::copy`, relaunches permanent executable, and exits cleanly.

2. **Feature 9 (Win32 SEH/VEH Plugin Crash Isolation)**:
   - **Report**: `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_2/handoff.md`
   - **Portable `crash_isolation.h`**: Located at `src/plugins/crash_isolation.h`.
     - **MSVC (`_MSC_VER`)**: Isolate `__try` / `__except` into non-template leaf function `sehExecuteLeaf(void (*fn)(void*), void* arg, DWORD* outCode)` with zero C++ objects/destructors, strictly complying with MSVC compiler rule C2712.
     - **MinGW GCC (`!_MSC_VER`)**: Win32 Vectored Exception Handling (`AddVectoredExceptionHandler`) registered once globally with an intrusive thread-local context stack (`t_currentCrashContext`) and `setjmp`/`longjmp`. Verified on GCC 16.2.0 with <15ns overhead, zero memory allocations, and complete real-time safety.
   - **Integration Points**: Wrap `process()` in `vst3_host.cpp` and `clap_host.cpp`. Also wrap GUI open/close, parameter changes, and state loading.
   - **Fault State Handling**: `IPluginInstance` maintains `std::atomic<bool> m_faulted{false}`. When trapped, set `m_faulted = true`, immediately copy input to output (`ctx.output.copyFrom(ctx.input)`), and bypass DSP on all subsequent blocks. In `RackView`, render faulted slot with crimson alert badge and `[RELOAD PLUGIN]` button.
   - **Unit Tests**: Implement `CrashingMockPlugin` in `tests/test_praccy.cpp` asserting that memory access violations (`0xC0000005`) are trapped cleanly, host does not crash, and output falls back to dry pass-through.

3. **Features 10, 11, 12 & 13 (Non-Throwing Parsing, Window Placement, Compiler Hardening & Tests)**:
   - **Report**: `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_3/handoff.md`
   - **Non-Throwing Parsing (`parse_utils.h`)**: Create `src/utils/parse_utils.h` wrapping C++20 `std::from_chars` for integers, floats, and hex conversions with fallback defaults. Replace all throwing `std::stoul`, `std::stof`, `std::stoi`, `std::atoi`, `std::atof` in `src/state/scene_manager.cpp` and `src/state/app_config.cpp`.
   - **Window Placement Persistence**: Persist `WINDOWPLACEMENT` (flags, showCmd, ptMinPosition, ptMaxPosition, rcNormalPosition) in `src/state/app_config.h` and `src/state/app_config.cpp`. Validate bounds via `MonitorFromRect(..., MONITOR_DEFAULTTONULL)` falling back to primary monitor work area to prevent off-screen windows on disconnected monitors. Restore placement in `src/main.cpp` after swapchain pre-rendering to eliminate white window flash.
   - **Compiler Warning Hardening**: Configure `/W4 /WX` on MSVC and `-Wall -Wextra -Werror` on GCC in `CMakeLists.txt`. Isolate third-party libraries (`third_party`) as `SYSTEM PRIVATE` and compile third-party files (`imgui`, `miniz`, Steinberg) with warning suppression (`/W0`, `-w`). Fix first-party latent warnings: `snprintf` for `strncpy` truncation in `asio_manager.cpp:48`, and double-cast `FARPROC` through `void*` in `vst3_host.cpp` and `plugin_scanner.cpp`.
   - **Unit Test**: Implement `testCorruptedPresetsIni()` in `tests/test_praccy.cpp` asserting graceful degradation on malformed/corrupted files.

---

## 2. Logic Chain & Immediate Next Steps for Successor

### Immediate Action Plan: Milestone 2 Implementation & Verification
1. **Initialize Workspace**: Successor reads `handoff.md`, `BRIEFING.md`, `PROJECT.md`, `ORIGINAL_REQUEST.md`, and starts its heartbeat cron (`*/10 * * * *`).
2. **Dispatch Worker (`worker_m2`)**:
   - Archetype: `teamwork_preview_worker`
   - Working directory: `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m2/`
   - Mandatory integrity warning included in prompt verbatim.
   - Attach all 3 explorer handoff reports:
     - `teamwork_preview_explorer_m2_1/handoff.md`
     - `teamwork_preview_explorer_m2_2/handoff.md`
     - `teamwork_preview_explorer_m2_3/handoff.md`
   - Exclusive write ownership:
     - `third_party/miniz/miniz.h`, `third_party/miniz/miniz.c`
     - `src/ui/update_checker.h`, `src/ui/update_checker.cpp`
     - `src/plugins/crash_isolation.h`, `src/plugins/plugin_base.h`, `src/plugins/vst3_host.cpp`, `src/plugins/clap_host.cpp`
     - `src/utils/parse_utils.h`, `src/state/scene_manager.cpp`, `src/state/app_config.h`, `src/state/app_config.cpp`
     - `src/audio/asio_manager.cpp`, `src/plugins/plugin_scanner.cpp`, `src/ui/rack_view.cpp`
     - `src/main.cpp`
     - `CMakeLists.txt`
     - `tests/test_praccy.cpp`
3. **Dispatch Verification Squad**:
   - `reviewer_m2_1` (`teamwork_preview_reviewer`): Code quality, memory safety, C2712 compliance, zero-shell verification.
   - `reviewer_m2_2` (`teamwork_preview_reviewer`): Error handling, warning clean compilation, regression safety.
   - `challenger_m2_1` (`teamwork_preview_challenger`): Stress tests for in-process miniz extraction, zip slip path fuzzing, and corrupted presets parsing.
   - `challenger_m2_2` (`teamwork_preview_challenger`): Empirically test plugin crash isolation on audio and GUI threads under high block rates.
   - `auditor_m2` (`teamwork_preview_auditor`): Forensic integrity verification. Verify zero occurrences of `std::system`, `cmd.exe`, `powershell.exe`. Verify genuine crash isolation and genuine non-throwing parsing. Binary veto rule applies.
4. **Gate Milestone 2**:
   - Verify all pass. Record verdicts in `GATE_STATUS.md` and mark M2 DONE in `PROJECT.md`.
5. **Proceed to Milestone 3 (Design Tokens & Responsive Canvas - R3)**:
   - Survey/blueprint and implement `design_tokens.h`, WCAG AA contrast (Obsidian Studio, Cyber/Midnight, Nordic Slate, Vintage Console), `scripts/check_hardcoded_colors.py` (0 raw `IM_COL32`), embedded Inter/JetBrains Mono in `praccy.rc`, dynamic canvas centering, and Hermite spline cables.

---

## 3. Caveats & Hard Constraints

- **DISPATCH-ONLY Orchestrator**: You MUST NOT write source code or run build/test commands directly. Delegate ALL technical work to subagents. File edits restricted strictly to `.agents/teamwork/` metadata (`.md`).
- **Audit Enforcement**: The Forensic Auditor's verdict is a non-negotiable binary veto. If `auditor_m2` reports `INTEGRITY VIOLATION`, the milestone fails unconditionally.
- **Mandatory Integrity Warning**: Must be included verbatim in all Worker dispatch prompts:
  > DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.
- **Prohibited Substrings**: Zero occurrences of `std::system`, `cmd.exe`, or `powershell.exe` in the codebase.
- **MinGW GCC vs MSVC C2712**: Ensure `crash_isolation.h` compiles cleanly under both MSVC and MinGW GCC.
- **Third-party Warning Isolation**: `third_party/miniz/` and Steinberg headers must be isolated to prevent compiler failure under `/W4 /WX` or `-Wall -Wextra -Werror`.
- **Succession Threshold**: Successor must self-succeed after its cumulative spawn count reaches 16.

---

## 4. Key Artifacts Index

- `f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md` — Authoritative User Specification
- `f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md` — Master Project Blueprint & Feature Inventory
- `f:/Projects/Praccy/.agents/teamwork/orchestrator/TEST_INFRA.md` — Test Architecture & Methodology
- `f:/Projects/Praccy/.agents/teamwork/orchestrator/GATE_STATUS.md` — Gate Verdict Registry
- `f:/Projects/Praccy/.agents/teamwork/orchestrator/BRIEFING.md` — Persistent Orchestrator Working Memory
- `f:/Projects/Praccy/.agents/teamwork/orchestrator/progress.md` — Execution Progress Tracker
- `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_1/handoff.md` — Features 7 & 8 Blueprint
- `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_2/handoff.md` — Feature 9 Blueprint
- `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_3/handoff.md` — Features 10, 11, 12, 13 Blueprint
