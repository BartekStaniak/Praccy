# Empirical Challenge Findings Report: Milestone 2

**Milestone**: Milestone 2 (SecOps, Hardening & Crash Isolation)  
**Agent**: Challenger Subagent 2 (`teamwork_preview_challenger_m2_2`)  
**Roles**: critic, specialist  
**Date**: 2026-10-06T22:11:00Z  
**Verdict**: **REQUEST_CHANGES**  

---

## 1. Observation

### 1.1 Empirical Test Suite & Execution Results
An empirical test harness was authored at `tests/test_challenger_m2_2.cpp` and compiled with GCC 16.2.0 via:
```powershell
$env:PATH = "C:\Users\Bartek\w64devkit\bin;$env:PATH"
g++ -std=c++20 -O2 -Isrc -Ithird_party -Ithird_party/readerwriterqueue tests/test_challenger_m2_2.cpp src/audio/graph_engine.cpp -lole32 -luuid -luser32 -lgdi32 -lwinmm -lavrt -o build/test_challenger_m2_2.exe
```
Execution of `f:\Projects\Praccy\build\test_challenger_m2_2.exe` produced the following verbatim output:
```
================================================================
   EMPIRICAL CHALLENGER TEST SUITE: MILESTONE 2 (SECOPS/CRASH)  
================================================================

[CHALLENGER-TEST 1] Continuous Hardware Faults across 15,000 Audio Blocks...
  - Processed 15000 continuous hardware faults in 39 ms
  - Total hardware crashes intercepted: 15000
  - Bit-exact dry pass-through blocks: 15000 / 15000
  -> PASSED: Zero host crashes, zero deadlocks, 100% bit-exact dry audio pass-through

[CHALLENGER-TEST 2] Memory Canary & Surrounding Buffer Integrity...
  -> PASSED: Canary buffers intact, zero memory corruption detected

[CHALLENGER-TEST 3] Multi-Threaded Concurrency (4 Threads, 20,000 Total Blocks)...
  - Completed 4 threads, total crashes intercepted: 20000
  - Total bit-exact audio blocks: 20000 / 20000
  -> PASSED: Thread-local VEH context perfectly isolated under heavy concurrent fault load

[CHALLENGER-TEST 4] Audio Thread Heap Allocation Audit...
  - safeCallPluginAudio 5,000 hardware crashes heap allocations: 0
  - PluginSlot::process 5,000 crashes heap allocations: 0
  - std::string("VST3 crash: ") + getExceptionDescription() on audio thread:
    * Length of string: 41 chars
    * Allocations detected: 1
    * Bytes allocated: 42
    [VULNERABILITY IDENTIFIED] Vst3PluginInstance::process and ClapPluginInstance::process
    allocate dynamic heap memory on the audio callback thread during crash latching!
  - m_faultReason = getExceptionDescription() from test_praccy.cpp:
    * Length of string: 29 chars
    * Allocations detected: 1
    * Bytes allocated: 31
  -> Audio thread allocation audit completed

[CHALLENGER-TEST 6] Nested safeCallPluginAudio Context Unwinding...
  -> PASSED: Nested crash contexts properly unwound without stack or state corruption

[CHALLENGER-TEST 5] Prohibited Command Strings Scan (src/ and Praccy.exe)...
  - Inspected 45 source files in src/, matches: 0
  - Inspected binary f:/Projects/Praccy/build/Praccy.exe (5516949 bytes), matches: 0
  -> PASSED: Zero prohibited strings in src/ or compiled binary

================================================================
   ALL EMPIRICAL CHALLENGER TESTS EXECUTED SUCCESSFULLY!        
================================================================
```

### 1.2 Crash Isolation & Dry Signal Resilience (Passed)
- In `src/plugins/crash_isolation.h` and `src/audio/graph_engine.cpp`:
  - 15,000 continuous hardware faults were processed in 39 ms across 7 distinct hardware exception types:
    * `EXCEPTION_ACCESS_VIOLATION` (null pointer read)
    * `EXCEPTION_ACCESS_VIOLATION` (unmapped address write `0xDEADBEEF`)
    * `EXCEPTION_INT_DIVIDE_BY_ZERO` (`0xC0000094`)
    * `EXCEPTION_ILLEGAL_INSTRUCTION` (`ud2` / `0xC000001D`)
    * `EXCEPTION_DATATYPE_MISALIGNMENT` (`0x80000002`)
    * `EXCEPTION_ARRAY_BOUNDS_EXCEEDED` (`0xC000008C`)
    * `EXCEPTION_FLT_STACK_CHECK` (`0xC00000FD`)
  - 15,000 / 15,000 blocks in single-threaded test and 20,000 / 20,000 blocks across 4 concurrent threads achieved 100% bit-exact dry pass-through (`std::memcmp == 0`).
  - Leading and trailing 128-byte memory canaries (`0xAA` and `0x55`) were verified intact after 5,000 latched faulted blocks; zero memory corruption occurred.
  - SSE MXCSR control word was verified restored to `0x1F80 | 0x8000 | 0x0040` (FTZ + DAZ) following every exception.
  - Nested crash contexts (`CrashContext*` intrusive stack) unwound cleanly without deadlock or frame corruption.

### 1.3 Prohibited Strings Inspection (Passed)
- Inspected 45 source files in `f:/Projects/Praccy/src/` (`.cpp`, `.h`, `.hpp`, `.c`): 0 occurrences of `std::system`, `cmd.exe`, `powershell.exe`, or `apply_update.bat`.
- Inspected compiled production application binary `f:/Projects/Praccy/build/Praccy.exe` (5,516,949 bytes): 0 occurrences of prohibited command strings.

### 1.4 Real-Time Audio Thread Dynamic Heap Allocation & Data Race (Vulnerability Found)
- In `src/plugins/vst3_host.cpp` (lines 377–381):
  ```cpp
  377:     if (!ok) {
  378:         m_faulted.store(true, std::memory_order_release);
  379:         m_faultReason = std::string("VST3 crash: ") + getExceptionDescription(exCode);
  380:         ctx.output.copyFrom(ctx.input);
  381:     }
  ```
- In `src/plugins/clap_host.cpp` (lines 170–174):
  ```cpp
  170:     if (!ok) {
  171:         m_faulted.store(true, std::memory_order_release);
  172:         m_faultReason = std::string("CLAP crash: ") + getExceptionDescription(exCode);
  173:         ctx.output.copyFrom(ctx.input);
  174:     }
  ```
- In `src/plugins/plugin_base.h` (lines 55–69):
  ```cpp
  55:     [[nodiscard]] virtual const std::string& faultReason() const noexcept {
  56:         return m_faultReason;
  57:     }
  ...
  67:     std::atomic<bool> m_faulted{false};
  68:     std::string m_faultReason;
  ```
- In `src/ui/rack_view.cpp` (lines 1262–1264):
  ```cpp
  1262:             if (ImGui::IsItemHovered()) {
  1263:                 ImGui::SetTooltip("Fault: %s\nDry audio pass-through is active.", pluginInst->faultReason().c_str());
  1264:             }
  ```
- Direct empirical measurement via global `operator new` hooking demonstrated:
  - String concatenation `std::string("VST3 crash: ") + getExceptionDescription(exCode)` yields a 41–42 character string.
  - The Small String Optimization (SSO) limit in both GCC libstdc++ and MSVC STL is 15 characters.
  - Exactly 1 dynamic heap allocation (42 bytes via `malloc`/`operator new`) is triggered on the audio callback thread during crash latching.
  - Plain `std::string m_faultReason` is written on the real-time audio callback thread (`vst3_host.cpp:379`, `clap_host.cpp:172`) and concurrently read on the UI thread (`rack_view.cpp:1263`) without mutex or atomic synchronization, constituting a data race (undefined behavior under ISO C++20).

---

## 2. Logic Chain

1. **Audio Callback Real-Time Constraints (PRAC-2026-V2-SPEC R1, R2, Acceptance Criteria)**:
   - PRAC-2026-V2-SPEC Acceptance Criteria explicitly states: *"Zero dynamic memory allocations occur within the audio callback loop."*
   - Milestone 2 Challenge Scope explicitly specifies: *"2. Verify zero heap allocations occur on the audio callback thread during crash isolation."*
2. **Analysis of Crash Latching Execution Path**:
   - `safeCallPluginAudio` and `PluginSlot::process` themselves do not allocate heap memory (Observation 1.1, 0 allocations across 5,000 crashed blocks).
   - However, when a plugin crashes, execution enters the `if (!ok)` error handler inside `Vst3PluginInstance::process` (`vst3_host.cpp:379`) or `ClapPluginInstance::process` (`clap_host.cpp:172`).
   - `process()` is invoked synchronously on the high-priority real-time audio thread (MMCSS Pro Audio).
   - In both hosts, `m_faultReason = std::string("... crash: ") + getExceptionDescription(exCode)` concatenates two strings to form a 41+ byte string.
   - Because standard library SSO buffers only accommodate 15 characters, `std::string` calls `operator new(42)`, triggering a dynamic heap allocation on the audio callback thread.
3. **Data Race on `m_faultReason`**:
   - `m_faultReason` is declared as non-atomic `std::string` in `IPluginInstance` (Observation 1.4).
   - The audio callback thread writes to `m_faultReason` on fault, while the UI thread reads `pluginInst->faultReason().c_str()` during ImGui frame rendering at 60 Hz in `rack_view.cpp:1263`.
   - Concurrent unsynchronized read and write on `std::string` is a C++ data race.

---

## 3. Caveats

- Win32 Vectored Exception Handling (MinGW GCC) and SEH (MSVC) selectively intercept hardware exceptions (`isCrashException`). Standard C++ exceptions (`throw std::runtime_error`) inside third-party plugins are rethrown by `safeCallPlugin`; if a hosted plugin throws a C++ exception, it is currently uncaught and will terminate the process.
- Under steady-state bypassed operation (after the initial faulted block is latched), zero heap allocations occur on subsequent blocks because `isFaulted()` short-circuits execution.

---

## 4. Conclusion

**Verdict: REQUEST_CHANGES**

While the core SEH/VEH crash isolation mechanism (`src/plugins/crash_isolation.h`) and `PluginSlot` audio pass-through demonstrated resilience across 35,000 continuous and concurrent hardware crashes, Milestone 2 cannot be approved as-is due to a real-time safety violation on the audio callback thread:

### Required Changes for Worker:
1. **Eliminate Audio-Thread Heap Allocation in `vst3_host.cpp` & `clap_host.cpp`**:
   - In `src/plugins/plugin_base.h`, replace `std::string m_faultReason;` with either:
     - `std::atomic<const char*> m_faultReason{nullptr};`
     - OR a fixed-size char array `std::array<char, 128> m_faultReason{};` (or `std::atomic<DWORD> m_lastExceptionCode{0};`).
   - In `src/plugins/vst3_host.cpp` (line 379) and `src/plugins/clap_host.cpp` (line 172):
     - Replace dynamic `std::string` concatenation with storing the static string literal directly:
       ```cpp
       m_faultReason.store(getExceptionDescription(exCode), std::memory_order_release);
       ```
       (Note: `getExceptionDescription()` already returns `const char*` pointing to string literals with static storage duration).
   - In `src/plugins/plugin_base.h`:
     ```cpp
     [[nodiscard]] virtual const char* faultReason() const noexcept {
         const char* r = m_faultReason.load(std::memory_order_acquire);
         return r ? r : "Unknown Fault";
     }
     ```
2. **Resolve Data Race on `faultReason()`**:
   - Using `std::atomic<const char*>` completely resolves the data race between the audio callback thread and UI tooltip rendering in `src/ui/rack_view.cpp`.

---

## 5. Verification Method

To independently reproduce and verify these findings:

1. **Run the Empirical Stress Test Binary**:
   ```powershell
   f:\Projects\Praccy\build\test_challenger_m2_2.exe
   ```
   *Expected Result*: Test 1, 2, 3, 5, 6 pass. Test 4 explicitly detects and logs the 42-byte heap allocation during `std::string` crash latching.

2. **Re-compile and Re-run After Worker Remediation**:
   ```powershell
   $env:PATH = "C:\Users\Bartek\w64devkit\bin;$env:PATH"
   g++ -std=c++20 -O2 -Isrc -Ithird_party -Ithird_party/readerwriterqueue tests/test_challenger_m2_2.cpp src/audio/graph_engine.cpp -lole32 -luuid -luser32 -lgdi32 -lwinmm -lavrt -o build/test_challenger_m2_2.exe
   f:\Projects\Praccy\build\test_challenger_m2_2.exe
   ```
   *Pass Criteria*: All tests pass with 0 allocations detected on the audio callback thread.
