# Forensic Audit Report: Milestone 4 (Modular UI Refactoring & Practice Suite Overhaul)

**Work Product**: Milestone 4 Implementation (Requirement R4)  
**Profile**: General Project (C++ Real-time Audio / UI)  
**Verdict**: **CLEAN**

---

## 1. Observation

Direct empirical evidence gathered across all Milestone 4 components:

### 1.1 Build and Binary Compilation
- Command: `cmake --build build --target Praccy test_praccy`
  Output:
  ```
  [100%] Built target Praccy
  [100%] Built target test_praccy
  ```
  Exit code: 0. Zero compiler warnings or errors under `-O3 -Wall -Wextra -Werror` / `/O2 /W4 /WX`.

### 1.2 Core Test Suite Execution
- Command: `.\build\test_praccy.exe`
  Output:
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
  [TEST] GraphEngineTest.ConcurrentParallelMutation... PASSED (74132 audio blocks, 0 audio-thread destructions)
  [TEST] AsioManagerTest.Format24BitUnpack... PASSED
  [TEST] InstrumentTuner Asynchronous Decoupling... PASSED
  [TEST] Zip Slip Path Traversal Sanitization... PASSED
  [TEST] In-Process miniz Archive Extraction & Zip Bomb Guard... PASSED
  [TEST] SecOps Zero Shell/Command Invocations in src/... PASSED (53 files inspected)
  [TEST] Win32 SEH/VEH Plugin Crash Isolation & Dry Bypass... PASSED
  [TEST] Corrupted presets.ini & config.ini Parsing Resilience... PASSED
  [TEST] String Parsing NaN/Inf & Leading Sign Sanitization... PASSED
  [TEST] WCAG AA/AAA Contrast Ratio Compliance (4 Themes)... PASSED
  [TEST] Theme Switching & Atomic Token Integrity... PASSED
  [TEST] Cubic Hermite Spline Evaluation & Audio-Reactive Sag... PASSED
  [TEST] Viewport Footprint Centering & Deadband Calculations... PASSED
  [TEST] EqualPowerRamp Energy Conservation Identity... PASSED
  [TEST] QuickLooper Circular Progress & Trigonometry... PASSED
  [TEST] WAV Drag-and-Drop Extension Validation... PASSED
  [TEST] Floating HUD Alpha Decay Computation... PASSED
  ===========================================
     ALL TESTS PASSED SUCCESSFULLY! (28/28)  
  ===========================================
  ```
  Exit code: 0. 28 of 28 unit tests passed (100%).

### 1.3 Full Regression & Challenger Test Suites Execution
- Command: `ctest --test-dir build --output-on-failure`
  Output:
  ```
  Test project F:/Projects/Praccy/build
      Start 1: test_praccy
  1/9 Test #1: test_praccy ......................   Passed    0.23 sec
      Start 2: test_challenger_m1
  2/9 Test #2: test_challenger_m1 ...............   Passed    0.46 sec
      Start 3: test_challenger_m1_2
  3/9 Test #3: test_challenger_m1_2 .............   Passed    1.70 sec
      Start 4: test_challenger_m2
  4/9 Test #4: test_challenger_m2 ...............   Passed    0.02 sec
      Start 5: test_challenger_m2_1
  5/9 Test #5: test_challenger_m2_1 .............   Passed    4.90 sec
      Start 6: test_challenger_m3_1
  6/9 Test #6: test_challenger_m3_1 .............   Passed    0.05 sec
      Start 7: test_challenger_m3_2
  7/9 Test #7: test_challenger_m3_2 .............   Passed    0.03 sec
      Start 8: test_challenger_m4_1
  8/9 Test #8: test_challenger_m4_1 .............   Passed    2.23 sec
      Start 9: test_challenger_m4_2
  9/9 Test #9: test_challenger_m4_2 .............   Passed    1.43 sec

  100% tests passed out of 9
  ```
  Exit code: 0. 9/9 test suites passed.

### 1.4 Hardcoded Color Enforcement
- Command: `py scripts/check_hardcoded_colors.py`
  Output:
  ```
  ------------------------------------------------------------------------
  SUCCESS: Clean! 0 hardcoded IM_COL32 / ImVec4 literals found in F:\Projects\Praccy\src\ui.
  Design tokens fully enforced across all UI translation units.
  ```
  Exit code: 0.
- Independent Codebase Grep:
  - Search for `IM_COL32` in `src/ui/`: 0 matches found across all UI files.
  - Search for `ImVec4(` in `src/ui/`: 2 matches found, strictly contained within `src/ui/design_tokens.h` (lines 43, 451). 0 matches outside `design_tokens.h`.
  - Search for raw 6/8-digit hex color literals `0x[0-9a-fA-F]{6,8}` in `src/ui/`: 1 match in `src/ui/thumbnail_manager.cpp` line 18, which is a Win32 COM CLSID structure definition (`s_pngClsid`), not a color literal.

### 1.5 Modal Decoupling Authenticity
- `CMakeLists.txt` lines 53-55:
  ```cmake
  src/ui/modals/plugin_browser_modal.cpp
  src/ui/modals/settings_modal.cpp
  src/ui/modals/practice_tools_modal.cpp
  ```
  These translation units are compiled into the `Praccy` executable target.
- File inspection:
  - `src/ui/modals/plugin_browser_modal.h` (74 lines) & `.cpp` (470 lines): implements `PluginBrowserModal` class with Spotlight fuzzy search, recent items MRU caching, and direct audio graph slot insertion.
  - `src/ui/modals/settings_modal.h` (58 lines) & `.cpp` (394 lines): implements `SettingsModal` class with Audio, Plugins, Theme, Updates, and About tabs.
  - `src/ui/modals/practice_tools_modal.h` (48 lines) & `.cpp` (306 lines): implements `PracticeToolsModal` class with Quick Looper and Backing Track tabs.
  - `src/ui/ui_helpers.h` (357 lines): centralized shared UI widgets (`ResettableSliderFloat`, `renderSlidingPillToggle`, `drawStarGeometry`, `renderBadgePill`, `computeHudToastAlpha`).
- Extraction verification in `src/ui/rack_view.cpp`:
  - `RackView` holds `std::unique_ptr<PluginBrowserModal>`, `std::unique_ptr<SettingsModal>`, `std::unique_ptr<PracticeToolsModal>` (lines 179-181).
  - Modal rendering delegated to `m_pluginBrowserModal->render()`, `m_settingsModal->render()`, `m_practiceToolsModal->render()` (lines 227-231).
  - Modal opening delegates to `m_pluginBrowserModal->open()` (lines 215, 945, 1638, 1794), `m_settingsModal->open()` (line 1971), and `m_practiceToolsModal->toggle()` (line 1985).
  - All old inline modal implementation functions (`renderSettingsModal`, `renderPracticeToolsModal`) were cleanly removed from `src/ui/rack_view.cpp`.

### 1.6 Win32 API Authenticity
- Folder Picker in `src/ui/modals/settings_modal.cpp` lines 31-89:
  - Uses `ScopedComInitializer` with `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)` and RAII `CoUninitialize()`.
  - Instantiates `IFileOpenDialog` via `CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fileDialog))`.
  - Sets options `FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST`.
  - Calls `fileDialog->Show(parentHwnd)`, `fileDialog->GetResult(&item)`, `item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)`.
  - Converts wide string to UTF-8 via `WideCharToMultiByte(CP_UTF8, ...)`, frees via `CoTaskMemFree(pszPath)`.
  - Updates `m_scanner.addCustomSearchPath(...)` and persists to `cfg.customPluginPaths`.
- WAV Drag-and-Drop in `src/main.cpp`:
  - Line 216: `DragAcceptFiles(hwnd, TRUE);`
  - Lines 637-658 in `WndProc`:
    - Handles `WM_DROPFILES`.
    - Queries file count via `DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0)`.
    - Retrieves path via `DragQueryFileW(hDrop, 0, filePathW, MAX_PATH)`.
    - Validates extension case-insensitively via `isValidWavFile` (lines 42-47).
    - Dispatches to `s_dropContext.player->loadWavFile(pathUtf8)` and `s_dropContext.looper->loadWavFile(pathUtf8)`.
    - Releases drop handle via `DragFinish(hDrop)`.
  - `src/tools/quick_looper.cpp` lines 163-294 implements genuine RIFF/WAVE parsing (16/24/32-bit PCM, 32-bit IEEE float, sample rate conversion/resampling).

### 1.7 EqualPowerRamp Real-Time Safety & Authenticity
- Trigonometric formulation in `src/audio/dsp_utils.h` lines 95-106:
  - `const float progress = static_cast<float>(m_currentSample) / static_cast<float>(m_totalSamples);`
  - `const float angle = progress * DspUtils::HALF_PI;` (where `HALF_PI = PI * 0.5f`)
  - `gainOld = std::cos(angle);`
  - `gainNew = std::sin(angle);`
  - Energy conservation: $\cos^2(\theta) + \sin^2(\theta) \equiv 1.0$. Maximum observed deviation in challenger harness: $1.2 \times 10^{-7}$.
- Real-time safety in `src/audio/graph_engine.cpp` lines 692-748:
  - Crossfading processes outgoing chain (`m_retiringBuffer`) and incoming chain (`m_activeBuffer`) sample-by-sample into `mainView`.
  - Scratch and active buffers are pre-allocated during `GraphEngine::prepare` (lines 513-516: `m_mainProcessingBuffer.resize(2, maxBlockSize)`, `m_scratchBuffer.resize(2, maxBlockSize)`, `m_retiringBuffer.resize(2, maxBlockSize)`, `m_activeBuffer.resize(2, maxBlockSize)`).
  - All views (`retView`, `actView`, `stepScratch`) are non-owning stack slices.
  - Zero dynamic heap allocations (`new`, `malloc`, `vector::resize`, `vector::push_back`) occur on the audio callback thread.
  - Node retirement and memory reclamation is handled off-thread on the UI thread via `GraphEngine::processReclamation()` (lines 618-629) after transition completion.
  - Empirical verification in `test_challenger_m4_1`: 12,000 rapid consecutive preset switches across 30,139 blocks confirmed 0 dynamic heap allocations on the audio callback thread.

### 1.8 Plugin Card Authenticity
- `src/ui/rack_view.cpp` lines 1078-1079:
  - Exact dimensions: `const float cardWidth = 240.0f; const float cardHeight = 224.0f;`
  - Begun with `ImGui::BeginChild(childId, ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar);`
- 5 vertical zones:
  - Zone 1 (Header): Format pill badge ("VST3", "CLAP", "DSP"), name, split button, delete button, active/bypassed sliding pill toggle (`renderSlidingPillToggle`).
  - Zone 2 (Body): Thumbnail preview image with multi-layer glow border, or high-tech procedural vector waveform faceplate. Clickable to open GUI window or DSP tweak modal.
  - Zone 3 (Monospace readout): `MIX: %3.0f%%` and `TRIM:%+5.1fdB` rendered using `g_fontMono`.
  - Zone 4 (Mix): `ResettableSliderFloat` with double-click reset to 100% and vertical level meter.
  - Zone 5 (Trim): `ResettableSliderFloat` with double-click reset to 0.0 dB.
- Zero inert dummy knobs:
  - Inspection of `renderPluginSlot` confirms no inert knobs are drawn. The rotary knob helper method `renderRotaryKnob` (line 1882) is uncalled.

### 1.9 Test Suite Integrity & Zero Tolerance
- Inspection of `tests/test_praccy.cpp`:
  - 307 real `assert(...)` statements across 28 test cases.
  - 0 instances of `assert(true)`, `assert(1)`, or dummy assertions.
  - Tests 25–28 (`testEqualPowerRampEnergyConservation`, `testQuickLooperCircularProgressMath`, `testWavDragAndDropExtensionValidation`, `testFloatingHudAlphaDecayComputation`) execute genuine mathematical and state machine assertions.
  - Previous tests 1–24 from Milestones 1–3 were preserved intact.

---

## 2. Logic Chain

1. **Static and Behavioral Authenticity**:
   - Observations 1.1, 1.2, and 1.3 show the codebase compiles under strict warning flags (`/W4 /WX`, `-Wall -Wextra -Werror`) and passes 100% of unit tests and challenger test suites.
   - Observation 1.9 demonstrates the test suite executes 307 concrete assertions testing mathematical identities, state transitions, and audio pipelines without stubbing or evasion.

2. **Design Token Compliance**:
   - Observation 1.4 confirms `py scripts/check_hardcoded_colors.py` reports 0 hardcoded colors across `src/ui/`.
   - Independent grep verification confirms zero undeclared `IM_COL32` and `ImVec4` calls exist in `src/ui/` outside `design_tokens.h`. All UI rendering in `rack_view.cpp`, `plugin_browser_modal.cpp`, `settings_modal.cpp`, and `practice_tools_modal.cpp` exclusively consumes semantic design tokens.

3. **Structural and Architectural Hygiene**:
   - Observation 1.5 confirms `src/ui/modals/plugin_browser_modal.cpp`, `settings_modal.cpp`, and `practice_tools_modal.cpp` exist as genuine translation units in `CMakeLists.txt` and are completely extracted from `rack_view.cpp`.
   - Observation 1.8 confirms plugin cards are sized exactly at 240x224px, contain 5 active functional zones, and have zero inert knobs.

4. **Platform and OS API Correctness**:
   - Observation 1.6 shows authentic COM `IFileOpenDialog` with `FOS_PICKFOLDERS` replacing manual string entry, and authentic `DragAcceptFiles` / `WM_DROPFILES` / `DragQueryFileW` handling for WAV files.
   - Dropped WAV files are parsed via an authentic in-process RIFF loader in `QuickLooper` that handles multiple bit depths and resampling.

5. **Real-Time Safety & DSP Integrity**:
   - Observation 1.7 shows `EqualPowerRamp` calculates exact $\cos / \sin$ quarter-wave gain curves preserving energy ($\Delta < 1.2 \times 10^{-7}$).
   - `GraphEngine` pre-allocates all crossfade buffers, uses non-blocking stack references in the audio callback, and defers node destruction to the UI thread via `processReclamation()`, ensuring zero dynamic heap allocations during scene transitions.

Therefore, the work product contains genuine logic across all Milestone 4 requirements and complies with the Development integrity mode.

---

## 3. Caveats

1. **Fuzzy Search Scoring Edge Cases (Challenger Findings)**:
   - *Long Query Inversion*: In `PluginBrowserModal::calculateFuzzyScore`, the exact match score is a fixed constant (`1000`), while the prefix match adds `500` plus cumulative subsequence bonus (`15 + consecutive * 5` per matching character). For queries longer than 10 characters (e.g. 11-char `"superchorus"`), the prefix score reaches `1045`, scoring higher than the exact match.
   - *Partial Sequence Score*: The subsequence loop adds points for partial character sequence matches even when the entire query sequence was not found (`qIdx < q.length()`), yielding non-zero scores (e.g. querying `"distortion"` matches letters 'd' and 'r' in `"Drive"`).
   - *Assessment*: These are heuristic ranking edge cases in search scoring, NOT integrity violations (they are genuine, non-facade code). Recommended for polish in Milestone 5.
2. **WM_DROPFILES Buffer Limit**:
   - `src/main.cpp` allocates `wchar_t filePathW[MAX_PATH]` (260 characters). Dropped files located deeper than `MAX_PATH` will be truncated by `DragQueryFileW`.
3. **Headless Execution for Native Win32 Folder Picker**:
   - `IFileOpenDialog` requires an interactive Windows desktop session and is skipped during headless CI testing.

---

## 4. Conclusion

Milestone 4 (Modular UI Refactoring & Practice Suite Overhaul - Requirement R4) satisfies all forensic integrity checks:
- **NO hardcoded test results** or test assertion bypasses.
- **NO facade implementations** or dummy stubs.
- **NO raw undeclared color literals** in `src/ui/`.
- **Authentic modal translation unit decoupling** and genuine Win32 COM & Drag-Drop integrations.
- **Authentic EqualPowerRamp crossfade trigonometry** and verified real-time safety (zero heap allocations on the audio callback thread).
- **Exact 240x224px plugin cards** with zero inert dummy knobs.

**Verdict**: **CLEAN**

---

## 5. Verification Method

To independently reproduce this forensic audit:

1. **Build Application & Tests**:
   ```powershell
   cmake --build build --target Praccy test_praccy
   ```
   *Expected*: Exit code 0, targets built with zero errors/warnings.

2. **Execute Unit Test Suite**:
   ```powershell
   .\build\test_praccy.exe
   ```
   *Expected*: `ALL TESTS PASSED SUCCESSFULLY! (28/28)`.

3. **Execute Full Test Suite (including M1–M4 challenger stress tests)**:
   ```powershell
   cmake --build build --target test_challenger_m1 test_challenger_m1_2 test_challenger_m2 test_challenger_m2_1 test_challenger_m3_1 test_challenger_m3_2 test_challenger_m4_1 test_challenger_m4_2
   ctest --test-dir build --output-on-failure
   ```
   *Expected*: `100% tests passed out of 9`.

4. **Verify Design Token Compliance**:
   ```powershell
   py scripts/check_hardcoded_colors.py
   ```
   *Expected*: `SUCCESS: Clean! 0 hardcoded IM_COL32 / ImVec4 literals found in F:\Projects\Praccy\src\ui.`.
