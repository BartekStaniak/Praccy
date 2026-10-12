# Handoff Report: Praccy V2.4 Stage 1 Shared Infrastructure & Test Harness Spec Survey

**Agent:** `spec_miner_survey_infra`  
**Working Directory:** `F:\Projects\Praccy\.agents\teamwork\spec_miner_survey_infra`  
**Date:** 2026-10-12  
**Handoff Type:** Hard (Task Complete)  

---

## 1. Observation

1. **Existing CTest Suite Status**:
   - Running `ctest` in `F:\Projects\Praccy\build` reported:
     ```
     Test project F:/Projects/Praccy/build
           Start  1: test_praccy
      1/10 Test  #1: test_praccy ......................   Passed    0.23 sec
           Start  2: test_asio_driver
      2/10 Test  #2: test_asio_driver .................   Passed    1.74 sec
           Start  3: test_challenger_m1
      3/10 Test  #3: test_challenger_m1 ...............   Passed    0.43 sec
           Start  4: test_challenger_m1_2
      4/10 Test  #4: test_challenger_m1_2 .............   Passed    1.57 sec
           Start  5: test_challenger_m2
      5/10 Test  #5: test_challenger_m2 ...............   Passed    0.02 sec
           Start  6: test_challenger_m2_1
      6/10 Test  #6: test_challenger_m2_1 .............   Passed    4.19 sec
           Start  7: test_challenger_m3_1
      7/10 Test  #7: test_challenger_m3_1 .............   Passed    0.03 sec
           Start  8: test_challenger_m3_2
      8/10 Test  #8: test_challenger_m3_2 .............   Passed    0.02 sec
           Start  9: test_challenger_m4_1
      9/10 Test  #9: test_challenger_m4_1 .............   Passed    1.84 sec
           Start 10: test_challenger_m4_2
     10/10 Test #10: test_challenger_m4_2 .............   Passed    0.80 sec

     100% tests passed out of 10
     Total Test time (real) =  10.90 sec
     ```
2. **`CMakeLists.txt` Target Definitions**:
   - In `CMakeLists.txt` lines 158-174, `add_executable(test_asio_driver ...)` is commented out:
     ```cmake
     # test_asio_driver disabled due to mismatched allocation warnings; remove or re-enable after fixing
     # add_executable(test_asio_driver
     #     tests/test_asio_driver.cpp
     ...
     ```
   - However, in line 209: `add_test(NAME test_asio_driver COMMAND test_asio_driver)` is active. Because `test_asio_driver.exe` exists in `build/`, CTest ran it and passed. On a clean configure, it will fail unless lines 158-174 are uncommented.
3. **Requirement R6 Infrastructure Audit**:
   - `src/utils/`: Directory contains only `parse_utils.h` (4,409 bytes). `src/utils/archive_utils.*` does not exist.
   - `src/ui/update_checker.cpp`: Lines 20-107 define `sanitizeZipEntryPath`, and lines 109-240 define `extractZipArchive` using vendored `third_party/miniz`. Limits are hardcoded to `MAX_SINGLE_FILE = 250MB`, `MAX_TOTAL_UNCOMPRESSED = 500MB`, `MAX_FILE_COUNT = 10000`, with no XML limit and no compression ratio guard.
   - `src/ui/`: Contains no `drag_drop_router.*`.
   - `src/main.cpp`: Lines 637-664 handle `WM_DROPFILES` inline:
     ```cpp
     case WM_DROPFILES: {
         HDROP hDrop = reinterpret_cast<HDROP>(wParam);
         UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
         ...
         if (isValidWavFile(droppedPath)) {
             if (s_dropContext.player) s_dropContext.player->loadWavFile(filePathUtf8);
             if (s_dropContext.looper) s_dropContext.looper->loadWavFile(filePathUtf8);
         }
     ```
     No classification for presets (`.ini`, `.pracpreset`) or chains (`.chain`, `.pracchaim`) exists.
   - `src/midi/midi_manager.cpp`: Lines 51-65 store bindings in `std::vector<MidiBinding> m_bindings;`. Lines 128-144 dispatch CC events by linear loop:
     ```cpp
     std::lock_guard<std::mutex> lock(m_bindingsMutex);
     for (const auto& binding : m_bindings) {
         if (binding.midiChannel != 0 && binding.midiChannel != channel) continue;
         ...
     ```
     This is an $O(N)$ linear traversal under mutex lock. No flat $O(1)$ table exists.
4. **Style and Token Verification**:
   - Running `py scripts/check_hardcoded_colors.py` returned exit code 0:
     ```
     SUCCESS: Clean! 0 hardcoded IM_COL32 / ImVec4 literals found in F:\Projects\Praccy\src\ui.
     Design tokens fully enforced across all UI translation units.
     ```
   - Running `py scripts/wcag_audit.py` returned exit code 0:
     ```
     OVERALL ORACLE RESULT: ALL REQUIRED TOKEN PAIRS SATISFY WCAG 2.1 REQUIREMENTS!
     ```

---

## 2. Logic Chain

1. **R6 Gaps & Requirements**:
   - *Observation 3* proves that `archive_utils` is missing, `DragDropRouter` is missing, and `MidiManager` uses an $O(N)$ vector.
   - Per `ORIGINAL_REQUEST.md` (Follow-up 2026-10-12T17:57:38Z, Section R6):
     - `src/utils/archive_utils.*` must be created wrapping vendored `miniz` with decompression bomb guards (30MB XML, 128MB archive, 100:1 ratio, 100 files) and Win32 path sanitization.
     - `src/ui/drag_drop_router.*` must be created to classify audio tracks (`.wav`, `.flac`, `.mp3`), presets (`.ini`, `.pracpreset`), and chains (`.chain`, `.pracchaim`), replacing the inline `WM_DROPFILES` block in `src/main.cpp`.
     - `MidiManager` must replace `std::vector<MidiBinding>` with a flat $O(1)$ lookup table of size $17 \times 128$ (`FlatCcSlot` for 16 channels + Omni, 128 CC numbers) with 0 runtime heap allocations.
2. **CTest & Build Health**:
   - *Observation 1* confirms all 10 existing tests currently pass with 100% success rate.
   - *Observation 2* indicates a latent build issue: `test_asio_driver` is currently commented out in `CMakeLists.txt`, so any clean rebuild (`rmdir /s /q build && cmake -B build ...`) would fail CTest unless `add_executable(test_asio_driver ...)` is uncommented.
3. **Stage 1 Test Suite Scope**:
   - The user request requires: "New Stage 1 test harness `tests/test_praccy_v2_1.cpp` authored and passing in CTest regression suite."
   - The harness must thoroughly verify all Stage 1 requirements (R1 through R6) and Gate 1.1 - 1.5 criteria:
     - R1: ParamAutomationRegister atomic dirty bitmask, zero-alloc audio thread drain, 16,384 sample PDC phase coherence, 64-sample equal-power latency crossfade energy, SPSC block-boundary slot swaps.
     - R2: Retroactive riff buffer pre-allocation (1, 2, 5, 10 min), $<0.15$ms atomic write-head latching, header-only `src/utils/wav_writer.h` 24-bit RIFF/WAV encoding fidelity.
     - R3: `UndoManager` 64-level capacity bound, state snapshot fidelity, 400ms idle inactivity coalescing threshold, SceneBar A/B tone toggle.
     - R4: Front-panel macro fuzzy regex discovery heuristics, hotkey `!WantTextInput` guards, 800-point overview waveform envelope.
     - R5: Spotlight favorites boost scoring, `ImGuiListClipper` virtualization, DIN IEC 60268-10 PPM ballistics ($\tau=350$ms, 1.2s peak hold).
     - R6: Archive bomb guards (30MB XML, 128MB total, 100:1 ratio, 100 files), Zip Slip sanitization, DragDropRouter classification, MidiManager flat $O(1)$ CC dispatch with 0 heap allocations.

---

## 3. Caveats

- In `update_checker.cpp`, the current download process extracts application updates (which might include a binary larger than 128MB in future builds). When refactoring to use `archive_utils.h`, either allow callers to pass custom `ArchiveBombGuards` overrides or preserve the specialized updater limits while having `archive_utils` defaults strictly enforce the 30MB XML / 128MB archive / 100:1 ratio / 100 files limits for content archives.
- On this Windows machine, the `python` command points to an uninstalled Windows Store alias; the launcher `py` must be used to execute Python scripts (`py scripts/check_hardcoded_colors.py`).

---

## 4. Conclusion

The specification survey for Shared Infrastructure (R6) and the Stage 1 Test Harness is complete and documented in detail in `F:\Projects\Praccy\.agents\teamwork\spec_miner_survey_infra\survey_report.md`.
Key findings:
1. All 10 existing CTest targets pass 100% in 10.9s.
2. `archive_utils.*` and `drag_drop_router.*` are completely absent and need to be implemented per the exact signatures in `survey_report.md`.
3. `MidiManager` currently uses an $O(N)$ vector traversal under mutex; it must be refactored to a flat $17 \times 128$ lookup table with zero allocations.
4. `CMakeLists.txt` lines 158-174 for `test_asio_driver` should be uncommented.
5. `tests/test_praccy_v2_1.cpp` has a complete specification covering R1 through R6 ready for the implementation agent.

---

## 5. Verification Method

To independently verify the observations:
1. **CTest Execution**:
   ```pwsh
   cd F:\Projects\Praccy\build
   ctest --output-on-failure
   ```
   Assert: 10/10 tests pass.
2. **Design Token Linter**:
   ```pwsh
   cd F:\Projects\Praccy
   py scripts/check_hardcoded_colors.py
   py scripts/wcag_audit.py
   ```
   Assert: Exit code 0 for both scripts.
3. **Inspect Specification Report**:
   Inspect `F:\Projects\Praccy\.agents\teamwork\spec_miner_survey_infra\survey_report.md` to verify all required sections, signatures, edge cases, and test specifications.
