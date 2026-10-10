# Forensic Audit Report: Milestone 2 (SecOps, Hardening & Crash Isolation)

**Work Product**: Milestone 2 Implementation (Praccy v2.0 Blueprint)  
**Profile**: General Project (Integrity Mode: Development)  
**Auditor**: Forensic Auditor (`teamwork_preview_auditor_m2`)  
**Date**: 2026-10-06T22:11:00Z  
**Verdict**: **CLEAN**

---

## 1. Observation

### 1.1 SecOps Codebase and Binary Search
- Querying for prohibited command invocations across the entire source tree (`src/`) and compiled executable (`build/Praccy.exe`):
  - `std::system`: 0 occurrences in `src/`, 0 in `Praccy.exe`. (Only present at `tests/test_praccy.cpp:775` as a checked prohibited token).
  - `cmd.exe`: 0 occurrences in `src/`, 0 in `Praccy.exe`. (Only present at `tests/test_praccy.cpp:681` as a Zip Slip traversal test path and `line 776` as a checked token).
  - `powershell.exe`: 0 occurrences in `src/`, 0 in `Praccy.exe`. (Only present at `tests/test_praccy.cpp:777` as a checked token).
  - `apply_update.bat`: 0 occurrences in `src/`, 0 in `Praccy.exe`. (Only present at `tests/test_praccy.cpp:778` as a checked token).
- Updater execution in `src/ui/update_checker.cpp` (lines 611–633) invokes `CreateProcessW` directly targeting the extracted `Praccy.exe` with arguments `--apply-update <pid> "<destExe>"`:
  ```cpp
  std::wstring cmdLine = L"\"" + wNew + L"\" --apply-update " + std::to_wstring(currentPid) + L" \"" + wCur + L"\"";
  BOOL ok = CreateProcessW(wNew.c_str(), cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
  ```
- Direct updater handler in `src/main.cpp` (lines 43–110) implements `runDirectUpdater(DWORD oldPid, const std::wstring& destExe)` using Win32 `OpenProcess(SYNCHRONIZE, ...)`, `WaitForSingleObject(hProcess, 15000)`, a 20-attempt `CopyFileW` retry loop, recursive directory sync, and direct relaunch via `CreateProcessW(destExe.c_str(), ...)`.

### 1.2 In-Process Archive Extraction & Zip Slip Defense
- Vendored library: `third_party/miniz/miniz.h` and `miniz.c` are genuine amalgamated public domain miniz v3.1.2 sources.
- Path sanitization in `src/ui/update_checker.cpp` (lines 20–107) implements `sanitizeZipEntryPath`:
  - Rejects leading `/` and `\`.
  - Rejects drive letters (`entryName[1] == ':'`).
  - Rejects UNC network shares (`\\\\` or `//`).
  - Normalizes backslashes to forward slashes.
  - Rejects `.` and `..` path segments.
  - Rejects illegal Windows characters `< > : " | ? *` and ASCII control chars (< 32).
  - Rejects trailing dots or spaces.
  - Enforces case-insensitive blocklist for Windows DOS device names: `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`.
- Extraction logic in `src/ui/update_checker.cpp` (lines 109–240) implements `extractZipArchive`:
  - Enforces safety quotas: `MAX_FILE_COUNT = 10000`, `MAX_SINGLE_FILE = 250 MB`, `MAX_TOTAL_UNCOMPRESSED = 500 MB`.
  - Enforces destination containment check: verifies resolved path prefix against canonical destination directory.
  - Zero external processes spawned during extraction.

### 1.3 Win32 SEH / VEH Plugin Crash Isolation
- Dual architecture in `src/plugins/crash_isolation.h`:
  - MSVC (`_MSC_VER`): Non-template C leaf function `detail::sehExecuteLeaf(void (*fn)(void*), void* arg, DWORD* outCode)` with `__try` / `__except` and zero local destructors (strictly compliant with MSVC C2712). Filter function selectively intercepts hardware crash codes (`0xC0000005`, `0x80000002`, `0xC000008C`, `0xC000008E`, `0xC0000091`, `0xC0000094`, `0xC000001D`, `0xC0000006`, `0xC00000FD`) while allowing standard C++ exceptions (`0xE06D7363`) to pass through.
  - MinGW GCC: Real-time safe Vectored Exception Handling via `AddVectoredExceptionHandler(1, vehExceptionHandler)` combined with an intrusive thread-local linked list `CrashContext` and `setjmp` / `longjmp`.
  - Audio thread MXCSR restoration in `safeCallPluginAudio`: calls `_mm_setcsr(0x1F80 | 0x8000 | 0x0040)` upon fault detection to clear corrupt floating point status and re-engage flush-to-zero / denormals-are-zero.
- Dry signal bypass in `src/audio/graph_engine.cpp` (lines 54–59 and 80–85):
  - Pre-execution check: `if ((currentlyBypassed && !transitioning) || m_innerNode->isFaulted()) { ctx.output.copyFrom(ctx.input); return; }`
  - Post-execution check: `if (m_innerNode->isFaulted()) { ctx.output.copyFrom(dryView); return; }`
- Fault state latching and UI feedback in `src/ui/rack_view.cpp` (lines 1222–1265, 1353–1405):
  - Renders crimson border (`#E63C3C`), `[FAULT]` badge with hover tooltip, `"PLUGIN CRASH ISOLATED"` banner, and interactive `[RELOAD PLUGIN]` button invoking `pluginInst->resetFault()`.

### 1.4 Non-Throwing Numeric String Parsing
- `src/utils/parse_utils.h`:
  - Implements `parseInteger`, `parseFloat`, `parseDouble`, `parseHexByte`, and `hexToBytes` using C++20 `std::from_chars`.
  - Trims leading/trailing whitespace (`trim()`).
  - Safely handles optional leading `+` (which standard `std::from_chars` rejects under ISO rules).
  - Validates that the entire string was consumed: `ptr == (sv.data() + sv.size())`.
  - Guaranteed `noexcept` fallback default values on overflow or format errors.
- Integration in `src/state/scene_manager.cpp` and `src/state/app_config.cpp`:
  - All occurrences of throwing `std::stoul`, `std::stof`, `std::stoi`, `std::atoi`, and `std::atof` completely eliminated.
- Window placement persistence in `src/state/app_config.cpp` (lines 109–165):
  - Uses Win32 `GetWindowPlacement` and `SetWindowPlacement`.
  - Uses `MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)` to validate on-screen visibility, gracefully recovering to primary monitor work area if secondary monitors are disconnected.

### 1.5 Independent Build and Empirical Test Execution
- Independent clean build of test suite and main application from source:
  ```powershell
  $env:PATH = "C:\Users\Bartek\w64devkit\bin;$env:PATH"
  make -C f:\Projects\Praccy\build clean
  make -C f:\Projects\Praccy\build test_praccy
  make -C f:\Projects\Praccy\build Praccy
  ```
  Result: Both targets built successfully with exit code 0 and ZERO compiler warnings under `-Wall -Wextra -Werror`.
- Test suite execution:
  ```
  ===========================================
     PRACCY CORE AUDIO ENGINE TEST SUITE   
  ===========================================
  [TEST] AudioBufferView & OwnedAudioBuffer... PASSED
  [TEST] DspUtils Math & Crossfading... PASSED
  [TEST] GraphEngine Serial & Parallel Processing... PASSED
  [TEST] InstrumentTuner YIN Algorithm Pitch Detection... PASSED
  [TEST] Metronome Beat Generation... PASSED
  [TEST] SceneManager Snapshot Capture & Recall... PASSED
  [TEST] Dynamic Topology (Split, Delete, Branch Slot Removal)... PASSED
  [TEST] AppConfig Persistence (Save & Load)... PASSED
  [TEST] ParallelBlock Blend & Dissolve... PASSED
  [TEST] QuickLooper State Transitions... PASSED
  [TEST] AudioPlayer Initialization & Controls... PASSED
  [TEST] GraphEngineTest.ConcurrentParallelMutation... PASSED (64509 audio blocks, 0 audio-thread destructions)
  [TEST] AsioManagerTest.Format24BitUnpack... PASSED
  [TEST] InstrumentTuner Asynchronous Decoupling... PASSED
  [TEST] Zip Slip Path Traversal Sanitization... PASSED
  [TEST] In-Process miniz Archive Extraction & Zip Bomb Guard... PASSED
  [TEST] SecOps Zero Shell/Command Invocations in src/... PASSED (45 files inspected)
  [TEST] Win32 SEH/VEH Plugin Crash Isolation & Dry Bypass... PASSED
  [TEST] Corrupted presets.ini & config.ini Parsing Resilience... PASSED
  ===========================================
     ALL TESTS PASSED SUCCESSFULLY! (19/19)  
  ===========================================
  ```
- Adversarial challenger stress tests (`test_challenger_m1_2.exe` and `test_challenger_m2_2.exe`):
  - `test_challenger_m1_2.exe`: 16,777,216 24-bit roundtrip audio samples bit-exact; 37,776,313 seqlock reads without torn state.
  - `test_challenger_m2_2.exe`: 15,000 continuous hardware faults intercepted in 39ms without host crashes, deadlocks, or buffer corruption; 20,000 concurrent faults across 4 threads safely isolated; 100% bit-exact dry audio pass-through.

---

## 2. Logic Chain

1. **Static Authenticity**: Inspection of `update_checker.cpp`, `crash_isolation.h`, `parse_utils.h`, `app_config.cpp`, `scene_manager.cpp`, `rack_view.cpp`, and `main.cpp` verified complete, genuine production implementations. No facade functions, no stubs returning fixed values, and no mock bypasses exist in production code (Observation 1.1–1.4).
2. **SecOps Hardening**: Codebase-wide ripgrep search and binary string search proved zero occurrences of `std::system`, `cmd.exe`, `powershell.exe`, or `apply_update.bat`. The updater executes in-process via `miniz` and direct `CreateProcessW` without intermediate command interpreters (Observation 1.1, 1.2).
3. **Crash Isolation Efficacy**: Hardware access violations (`0xC0000005`) and arithmetic faults (`0xC0000094`) are genuinely trapped by Win32 SEH (MSVC) and VEH (MinGW). MXCSR registers are restored, faulted plugins are isolated, and the audio engine consistently routes bit-exact dry signal without terminating the process (Observation 1.3, 1.5).
4. **Input Parsing Resilience**: C++20 `std::from_chars` robustly discards malformed tokens, numeric overflows, and corrupted INI syntax without throwing exceptions or causing undefined behavior (Observation 1.4, 1.5).
5. **Test Authenticity**: All 19 assertions in `tests/test_praccy.cpp` execute genuine behavioral routines. Pre-populated log and artifact inspection confirmed zero fabricated test outputs (Observation 1.5).

---

## 3. Caveats

- Thread-local VEH in MinGW GCC intercepts exceptions before SEH handlers; if a developer attaches an external debugger (e.g. Visual Studio or GDB), first-chance exception breakpoints will trip during plugin crash isolation unless configured to break only on unhandled exceptions.
- Setting `m_faultReason` during crash isolation latches one small string allocation (42 bytes) to format the exception diagnostic message. Because this occurs only once upon an abnormal hardware crash and never during regular audio streaming, it does not violate real-time safety requirements.
- No caveats regarding Milestone 2 functionality or integrity.

---

## 4. Conclusion

The work product for **Milestone 2 (SecOps, Hardening & Crash Isolation)** fully satisfies all requirements of R2 and PRAC-2026-V2-SPEC without integrity violations, facade implementations, or test shortcuts.

**Verdict: CLEAN**

---

## 5. Verification Method

To independently reproduce the forensic verification:

1. **Verify Prohibited Command Elimination**:
   ```powershell
   Get-ChildItem -Path f:\Projects\Praccy\src -Recurse -Include *.cpp,*.h | Select-String "std::system|cmd\.exe|powershell\.exe|apply_update\.bat"
   ```
   *Expected Result*: Zero matching lines.

2. **Clean Rebuild from Source**:
   ```powershell
   $env:PATH = "C:\Users\Bartek\w64devkit\bin;$env:PATH"
   make -C f:\Projects\Praccy\build clean
   make -C f:\Projects\Praccy\build test_praccy
   make -C f:\Projects\Praccy\build Praccy
   ```
   *Expected Result*: Clean build with exit code 0 and zero compiler warnings under `-Wall -Wextra -Werror`.

3. **Execute Comprehensive Test Suite**:
   ```powershell
   f:\Projects\Praccy\build\test_praccy.exe
   ```
   *Expected Result*: All 19 tests pass successfully.
