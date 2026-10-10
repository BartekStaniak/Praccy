# Forensic Survey Report: SecOps, Crash Isolation & Core Infrastructure (Praccy v2.0)

**Date**: 2026-10-06T18:36:00Z  
**Author**: Explorer Subagent (`teamwork_preview_explorer_survey_2`)  
**Mission**: Requirement R2 & Associated Acceptance Criteria Investigation  
**Working Directory**: `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_2/`  

---

## 1. Observation

### 1.1 Update Checker & Command Execution Audit
A codebase-wide search across `src/` and repository roots yielded the following exact occurrences:

1. **`std::system()` and External Command Invocations**:
   - `src/ui/update_checker.cpp:330-331`:
     ```cpp
     std::string tarCmd = "tar -xf \"" + zipPath.string() + "\" -C \"" + extractDir.string() + "\"";
     int tarRet = std::system(tarCmd.c_str());
     ```
   - `src/ui/update_checker.cpp:335-337`:
     ```cpp
     std::string psCmd = "powershell -NoProfile -NonInteractive -Command \"Expand-Archive -Path '" +
         zipPath.string() + "' -DestinationPath '" + extractDir.string() + "' -Force\"";
     std::system(psCmd.c_str());
     ```
   - No other occurrences of `std::system()` exist in the entire codebase.
2. **`cmd.exe` Invocation in Binary/Updater Code**:
   - `src/ui/update_checker.cpp:419`:
     ```cpp
     std::wstring cmdLine = L"cmd.exe /c \"" + wBat + L"\" " + std::to_wstring(currentPid) + L" \"" + wNew + L"\" \"" + wCur + L"\"";
     ```
   - Invoked via `CreateProcessW` (line 427) to run a dynamically generated batch script `apply_update.bat` with process polling (`tasklist`, lines 398-403).
3. **Strings Inspection & Attack Surface**:
   - Spawning `tar` and `powershell.exe` via `std::system` invokes `%COMSPEC%` (`cmd.exe`) under the hood, creates console window flash/flicker, introduces path injection risks if directories contain quotes or special characters (`&`, `|`, `;`), and fails environments where PowerShell execution is restricted by enterprise policy.
   - The Acceptance Criteria explicitly demands:
     > *"Static analysis string inspection confirms zero occurrences of `std::system`, `cmd.exe`, or `powershell.exe` in binary/updater code."*
     The current binary violates this criterion at lines 331, 335, 337, and 419.

---

### 1.2 VST3 and CLAP Hosting Crash Isolation (SEH) Audit
Inspection of plugin hosting code in `src/plugins/` revealed zero exception handling around untrusted plugin DLL boundaries:

1. **VST3 Audio Processing**:
   - `src/plugins/vst3_host.cpp:346-372`:
     ```cpp
     void Vst3PluginInstance::process(audio::AudioProcessContext& ctx) {
         ...
         m_processor->process(data);
     }
     ```
     `m_processor->process(data)` is executed directly on the real-time audio thread without SEH or C++ `try-catch`.
2. **VST3 GUI and Control Invocations**:
   - `src/plugins/vst3_host.cpp:441`: `m_plugView->attached(reinterpret_cast<void*>(parentHwnd), Steinberg::kPlatformTypeHWND)`
   - `src/plugins/vst3_host.cpp:459`: `view->getSize(&rect)`
   - `src/plugins/vst3_host.cpp:478`: `m_plugView->removed()`
   - `src/plugins/vst3_host.cpp:409-411`: `m_controller->setParamNormalized(...)`
   - `src/plugins/vst3_host.cpp:498-502`: `m_component->setState(&stream)`, `m_controller->setComponentState(&stream)`
3. **CLAP Audio Processing and GUI Invocations**:
   - `src/plugins/clap_host.cpp:164`: `m_plugin->process(m_plugin, &processData)`
   - `src/plugins/clap_host.cpp:225-240`: `m_guiExt->create()`, `m_guiExt->set_parent()`, `m_guiExt->show()`
   - `src/plugins/clap_host.cpp:245-246`: `m_guiExt->hide()`, `m_guiExt->destroy()`
   - `src/plugins/clap_host.cpp:254`: `m_guiExt->get_size()`
4. **Win32 Window Callback**:
   - `src/plugins/plugin_window.cpp:158-177`: `PluginWindowManager::windowProc` dispatches messages without SEH protection.
5. **Compiler Nuance & Empirical Verification**:
   - Testing `__try` / `__except` syntax on the installed MinGW-w64 GCC 16.2.0 compiler output:
     ```
     <stdin>:12:7: error: expected 'catch' before '__except'
     ```
     GCC's C++ frontend does NOT support Microsoft `__try` / `__except` extensions.
   - Testing Win32 Vectored Exception Handling (`AddVectoredExceptionHandler` with `setjmp` recovery) under GCC 16.2.0 succeeded:
     ```
     VEH caught access violation successfully!
     ```
   - In MSVC (`_MSC_VER`), `__try` / `__except` is supported natively, but **MSVC Compiler Error C2712** forbids `__try` in any function requiring C++ object unwinding (destructors).

---

### 1.3 Numeric String Parsing Audit (`scene_manager.cpp` & `app_config.cpp`)
Grep searches for string-to-number conversions identified multiple throwing and unchecked parsing locations:

1. **`src/state/scene_manager.cpp`**:
   - Line 28 (in `hexToBytes`):
     ```cpp
     uint8_t b = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
     ```
     Throws `std::invalid_argument` if any character in the hex string is non-hex; throws `std::out_of_range` on integer overflow. Neither exception is caught.
   - Line 540:
     ```cpp
     size_t count = (itNodes != kv.end()) ? std::stoul(itNodes->second) : 0;
     ```
     Throws unhandled `std::invalid_argument` if `numNodes=` contains malformed text.
   - Lines 552-554:
     ```cpp
     np.slot.dryWet = kv.count(pfx + "dryWet") ? std::stof(kv.at(pfx + "dryWet")) : 1.0f;
     np.slot.inputGainDb = kv.count(pfx + "inGain") ? std::stof(kv.at(pfx + "inGain")) : 0.0f;
     np.slot.outputGainDb = kv.count(pfx + "outGain") ? std::stof(kv.at(pfx + "outGain")) : 0.0f;
     ```
     Throws unhandled `std::invalid_argument` or `std::out_of_range` on malformed float strings.
   - Line 560: `std::stoul(kv.at(pfx + "numBranches"))`
   - Lines 565-566: `std::stof(kv.at(bpfx + "gain"))`, `std::stof(kv.at(bpfx + "pan"))`
   - Line 571: `std::stoul(kv.at(bpfx + "numSlots"))`
   - Lines 579-581: `std::stof(kv.at(spfx + "dryWet"))`, `std::stof(kv.at(spfx + "inGain"))`, `std::stof(kv.at(spfx + "outGain"))`
   - Any corruption or tampering in `presets.ini` immediately terminates the application with an unhandled C++ exception.
2. **`src/state/app_config.cpp`**:
   - Line 49: `inputMode = static_cast<audio::InputRoutingMode>(std::atoi(val.c_str()));`
   - Lines 51, 53, 55: `std::atof(val.c_str())` for `input_gain_db`, `master_volume_db`, `metronome_bpm`.
   - `std::atoi` and `std::atof` exhibit undefined behavior on overflow, return `0` indistinguishably on errors, and `atof` is locale-dependent (period vs comma decimal separators).
3. **`std::from_chars` Verification**:
   - Verified empirically on the environment's C++20 compiler (`g++.exe -std=c++20`):
     ```cpp
     std::from_chars(sv.data(), sv.data() + sv.size(), val, 16); // Hex
     std::from_chars(sv.data(), sv.data() + sv.size(), floatVal); // Float
     ```
     Both integer (base 10, base 16) and floating-point conversions succeeded with zero allocations, non-throwing return codes (`std::errc`), and graceful fallback on invalid strings.

---

### 1.4 Window Placement & Maximized State Audit
1. **Creation**:
   - `src/main.cpp:60-66`:
     ```cpp
     HWND hwnd = CreateWindowW(
         wc.lpszClassName,
         L"Praccy - ASIO VST3/CLAP Practice Host",
         WS_OVERLAPPEDWINDOW,
         100, 100, 1280, 720,
         nullptr, nullptr, wc.hInstance, nullptr
     );
     ```
     Position `(100, 100)` and size `(1280, 720)` are hardcoded constants.
2. **Display**:
   - `src/main.cpp:291`: Always invokes `ShowWindow(hwnd, SW_SHOWDEFAULT);`.
3. **Configuration Storage**:
   - `src/state/app_config.h:9-23`: Does not declare any members for window coordinates, dimensions, or maximized flags.
   - `src/state/app_config.cpp:67-89`: Does not serialize window geometry.

---

### 1.5 Build Environment, Compiler Flags & Test Harness Survey
1. **Toolchain**:
   - Host OS: Windows 10/11 x64.
   - Active Compiler: `C:/Users/Bartek/w64devkit/bin/g++.exe` (GCC 16.2.0, x86_64-w64-mingw32).
   - Build Tool: `GNU Make 4.4.1`.
   - CMake: `4.4.3`.
2. **Current Flags in `CMakeLists.txt:85-89`**:
   ```cmake
   if(MSVC)
       target_compile_options(Praccy PRIVATE /O2 /W4 /permissive-)
   else()
       target_compile_options(Praccy PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter)
   endif()
   ```
   - MSVC is missing `/WX` (warnings as errors).
   - Test targets (`test_praccy`, `test_asio_driver`) do NOT have `target_compile_options` set at all.
   - `target_include_directories` does not use `SYSTEM` for `third_party/`, causing third-party header warnings to propagate into consumer code.
3. **Latent Warnings Discovered During Strict Compilation**:
   - `src/audio/asio_manager.cpp:46`: `warning: 'char* strncpy(char*, const char*, size_t)' output may be truncated [-Wstringop-truncation]`
   - `src/plugins/vst3_host.cpp:176, 181, 317`: `warning: cast between incompatible function types from 'FARPROC' to 'InitDllProc' / 'GetFactoryProc' [-Wcast-function-type]`
   - `src/plugins/plugin_scanner.cpp:265`: `warning: cast between incompatible function types from 'FARPROC' to 'GetFactoryProc' [-Wcast-function-type]`
   - `third_party/vst3_pluginterfaces/base/funknown.cpp:301, 321, 354, 390`: `warning: format '%X' expects argument of type 'unsigned int', but argument 3 has type 'long unsigned int' [-Wformat=]`
4. **Current Test Suites**:
   - `build/test_praccy.exe`: Executes 11 unit tests covering audio buffers, DSP utils, graph engine, tuner, metronome, scene manager memory capture, topology, config save/load, quick looper, and audio player. All 11 currently pass.
   - `build/test_asio_driver.exe`: Only attempts to enumerate physical ASIO drivers in the registry and sleep 500ms; lacks mock ASIO headless stress testing.
   - Missing tests for R2: corrupted `presets.ini` test, simulated plugin crash isolation test.

---

## 2. Logic Chain

### 2.1 From Updater Commands to In-Process & Safe Architecture
1. *Observation 1.1* demonstrates that `tar` and `powershell.exe` are invoked via `std::system()`, and `cmd.exe` is invoked via `CreateProcessW` to run `apply_update.bat`.
2. Spawning shell commands introduces command injection risks, launches visible console windows, depends on external system binaries that may be locked down, and violates Acceptance Criteria ("zero occurrences of `std::system`, `cmd.exe`, or `powershell.exe`").
3. Therefore:
   - For archive extraction: We must replace external process extraction with an in-process ZIP engine.
   - Between `miniz` and Win32 Shell COM API (`IShellDispatch`):
     - `miniz` is a self-contained single-header/source C library with zero OS dependencies, pure in-memory/in-process stream decoding, and allows rigorous sanitization against Zip Slip path traversal (`../`).
     - `IShellDispatch::CopyHere` operates asynchronously on Explorer threads, requires COM STA marshalling, can hang or spawn shell confirmation dialogs, and requires linking `shell32`.
     - Hence, `miniz` is the superior, deterministic choice.
   - For update restart:
     - Instead of writing a `.bat` script executed by `cmd.exe /c`, the downloaded/extracted executable `Praccy.exe` can be launched directly with a command-line flag: `Praccy.exe --apply-update <oldPid> "<destPath>"`.
     - The updater instance waits on `OpenProcess(SYNCHRONIZE, FALSE, oldPid)` using `WaitForSingleObject()`, copies itself over the target executable via Win32 `CopyFileW()`, spawns the newly installed `Praccy.exe`, and exits.
     - This eliminates all `.bat` files, `cmd.exe`, `powershell.exe`, and `std::system()` entirely.

### 2.2 From Plugin Access Violations to Structured Crash Isolation
1. *Observation 1.2* confirms that calls to VST3 and CLAP `process()`, `openGui()`, `closeGui()`, and parameter setters are completely unprotected.
2. Third-party plugins execute native x86_64 machine code inside the Praccy host process. Any unhandled null pointer dereference or memory corruption generates a Win32 hardware exception (Access Violation `0xC0000005`). Standard C++ `catch(...)` does NOT catch SEH exceptions without compiler-specific asynchronous unwind flags.
3. *Observation 1.2* also revealed that MSVC strictly prohibits `__try` / `__except` within functions that instantiate local C++ objects with destructors (C2712).
4. Furthermore, GCC on Windows does not support `__try` / `__except` keywords in C++ mode.
5. Therefore:
   - We must encapsulate plugin invocations in a dedicated, isolated leaf wrapper function containing NO non-trivial C++ destructors.
   - On MSVC (`_MSC_VER`): Use native `__try` / `__except(filter(GetExceptionCode()))`.
   - On MinGW/GCC: Use Win32 Vectored Exception Handling (`AddVectoredExceptionHandler`) paired with thread-local protection flags and `setjmp`/`longjmp`.
   - When an exception is trapped:
     - The plugin is marked as faulted: `m_faulted.store(true, std::memory_order_release)`.
     - Audio buffers are passed through dry (`ctx.output.copyFrom(ctx.input)`).
     - The host engine continues running without terminating or audio glitching.
     - The UI displays a warning/bypass badge for the faulted plugin.

### 2.3 From Crashing Conversions to Resilient `std::from_chars`
1. *Observation 1.3* shows that `hexToBytes()` in `scene_manager.cpp` calls `std::stoul()` without exception handling, and preset loading calls `std::stoul()` / `std::stof()`.
2. Any malformed, truncated, or edited `presets.ini` file triggers `std::invalid_argument` or `std::out_of_range`, which unwinds out of `SceneManager::loadFromFile()` and terminates Praccy.
3. *Observation 1.3* verified that `std::from_chars` (`<charconv>`) operates non-throwingly for both integer and floating-point types in C++20.
4. Therefore, creating a utility parser `parseInteger<T>` and `parseFloat` backed by `std::from_chars` with fallback to default values guarantees that malformed config and preset files load safely without crashing.

### 2.4 From Hardcoded Dimensions to `GetWindowPlacement` Persistence
1. *Observation 1.4* shows that window coordinates are hardcoded to `(100, 100, 1280, 720)` in `main.cpp` and `AppConfig` does not persist window state.
2. If the user closes Praccy while maximized, using `GetWindowRect` would erroneously save maximized coordinates as the normal restored window size.
3. Win32 `GetWindowPlacement` / `SetWindowPlacement` decouples the restored window rectangle (`rcNormalPosition`) from the maximized state (`showCmd == SW_SHOWMAXIMIZED`).
4. Therefore, storing `window_x`, `window_y`, `window_w`, `window_h`, and `window_maximized` in `AppConfig` and restoring them via `SetWindowPlacement()` with multi-monitor bounds checking (`MonitorFromRect`) satisfies all requirements.

### 2.5 From Compiler Warnings to `/W4 /WX` Clean Build
1. *Observation 1.5* shows that `/WX` is currently missing in MSVC, and strict compiler flags trigger warnings on third-party code (`funknown.cpp`) and first-party code (`asio_manager.cpp`, `vst3_host.cpp`, `plugin_scanner.cpp`).
2. If `/WX` is added to MSVC without isolating third-party code, compilation fails immediately due to warnings in Steinberg VST3 base and ImGui.
3. Therefore:
   - Third-party code must be isolated with `SYSTEM` include directories and separate source property warning levels.
   - First-party code must fix the `FARPROC` cast warnings (by casting through `reinterpret_cast<void*>(...)` or pragmas) and the `strncpy` truncation warning (by using `memcpy` with explicit null terminator or `snprintf`).
   - `/W4 /WX` can then be cleanly applied to all first-party Praccy targets.

---

## 3. Caveats

1. **MinGW GCC vs MSVC SEH Implementation**:
   - The user specification mentions `__try` / `__except` blocks (the MSVC syntax). Because the local development machine uses MinGW GCC 16.2.0, an MSVC-only `__try` / `__except` block would fail compilation if built locally under GCC unless guarded by `#if defined(_MSC_VER)`.
   - The blueprint design must provide a portable abstraction: native `__try` / `__except` on MSVC, and Win32 Vectored Exception Handling (`AddVectoredExceptionHandler`) on MinGW GCC.
2. **Crash State Recovery Limitations**:
   - SEH isolates memory access violations and prevents the application process from terminating. However, if a plugin corrupts its own internal state or heap memory before the access violation occurs, calling subsequent methods on that same plugin instance (such as trying to read its parameters or re-render its GUI) may trigger repeated faults.
   - Hence, once a plugin encounters an exception, it must be permanently flagged as `m_faulted = true` and completely bypassed for both audio and GUI until the user explicitly reloads or removes it.
3. **Zip Slip Protection**:
   - When extracting archives using `miniz`, archive entry paths must be strictly sanitized to prevent directory traversal attacks (e.g. entries containing `../` or absolute drive letters).
4. **Multi-Monitor Window Placement**:
   - If a user disconnects an external display and restarts Praccy, restoring the previous coordinates could place the window entirely off-screen. The restoration logic must validate visibility using `MonitorFromRect(..., MONITOR_DEFAULTTONULL)`.

---

## 4. Conclusion

The codebase currently contains critical SecOps and stability vulnerabilities:
1. External process spawning via `std::system()` (`tar`, `powershell.exe`) and `CreateProcessW` (`cmd.exe`).
2. Zero crash isolation around untrusted third-party VST3 and CLAP plugin boundaries.
3. Throwing numeric conversions (`std::stoul`, `std::stof`) that cause instant application crashes upon encountering malformed `presets.ini` files.
4. Hardcoded window geometry with zero persistence.
5. Incomplete MSVC warning policy (`/WX` missing, third-party includes unshielded).

### Concrete Blueprint Recommendations:
- **SecOps**:
  - Add `third_party/miniz/miniz.h` and `miniz.c`.
  - Implement in-process archive extraction in `update_checker.cpp` with Zip Slip path sanitization.
  - Implement native Win32 update helper mode (`Praccy.exe --apply-update <pid> <dest>`) using `CreateProcessW` directly without `cmd.exe` or batch scripts.
- **Crash Isolation**:
  - Author `src/plugins/crash_isolation.h` providing `SafeCallPluginAudio()` and `SafeCallPluginGui()` using `#if defined(_MSC_VER) __try/__except` (with C2712 leaf function isolation) and `#else AddVectoredExceptionHandler`.
  - Add `std::atomic<bool> m_faulted{false};` to `IPluginInstance`. On fault: bypass audio, mute/copy dry signal, alert UI.
- **Hardened Numeric Parsing**:
  - Author `src/utils/parse_utils.h` with `parseInteger<T>()`, `parseFloat()`, and `parseHexByte()` using non-throwing `std::from_chars`.
  - Update `scene_manager.cpp` (`hexToBytes`, `loadFromFile`) and `app_config.cpp` (`load`).
- **Window Geometry**:
  - Extend `AppConfig` with `windowX`, `windowY`, `windowW`, `windowH`, and `windowMaximized`.
  - Add `saveWindowPlacement(HWND)` and `restoreWindowPlacement(HWND)` in `app_config.cpp` using Win32 `GetWindowPlacement` / `SetWindowPlacement` with `MonitorFromRect` fallback.
- **Build & Quality Engineering**:
  - Update `CMakeLists.txt`: mark `third_party` as `SYSTEM PRIVATE`, separate third-party compilation from first-party warning flags, enable `/W4 /WX` on MSVC and `-Wall -Wextra -Werror` on GCC.
  - Fix latent `FARPROC` and `strncpy` warnings in first-party code.
  - Add comprehensive unit tests for corrupted presets and simulated crash isolation in `tests/test_praccy.cpp`.

---

## 5. Verification Method

### 5.1 Static Analysis Inspection for Prohibited Strings
Run the following PowerShell commands across the repository to verify zero occurrences of prohibited process calls:
```powershell
Get-ChildItem -Path f:\Projects\Praccy\src -Recurse -Include *.cpp,*.h | Select-String -Pattern "std::system\(", "cmd\.exe", "powershell\.exe", "tar -xf"
```
*Expected*: Zero matches returned.

### 5.2 Build Verification Commands
Verify clean compilation under GCC with warnings treated as errors:
```powershell
& "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build clean
& "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build test_praccy
& "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build Praccy
```
*Expected*: Exit code 0, zero compiler warnings or errors.

### 5.3 Automated Test Suite Execution
Execute the core test suite:
```powershell
f:\Projects\Praccy\build\test_praccy.exe
```
*Expected*: All tests pass with exit code 0.

### 5.4 Regression Suite Invalidation Conditions
The architectural blueprint is invalidated if:
1. `miniz` or archive extraction throws an unhandled exception or allows paths containing `../`.
2. A simulated access violation in a plugin terminates the Praccy host process.
3. Feeding a corrupted `presets.ini` (e.g. `numNodes=notanumber`, `state=XXYYZZ`) throws an exception or crashes `test_praccy.exe`.
4. Storing window placement when maximized fails to restore the unmaximized normal rectangle upon unmaximizing.
