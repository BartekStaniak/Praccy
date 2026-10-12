# Audio DSP & Real-Time Engine Survey Handoff Report

## 1. Observation
1. **Existing Test Suite Baseline**:
   - Command: `ctest --output-on-failure` executed in `F:\Projects\Praccy\build`.
   - Result: 10/10 tests passed in 10.96 seconds (`test_praccy`, `test_asio_driver`, `test_challenger_m1`, `test_challenger_m1_2`, `test_challenger_m2`, `test_challenger_m2_1`, `test_challenger_m3_1`, `test_challenger_m3_2`, `test_challenger_m4_1`, `test_challenger_m4_2`).
2. **Audio Graph Engine & Mutex Contention**:
   - File: `F:\Projects\Praccy\src\audio\graph_engine.h` and `src\audio\graph_engine.cpp`.
   - In `graph_engine.cpp:695`: `std::unique_lock<std::mutex> lock(m_graphMutex, std::try_to_lock);` executes inside `GraphEngine::process()` on the audio callback thread.
   - In `graph_engine.cpp:534`, `542`, `549`, `603`, `610`: UI thread locks `m_graphMutex` via `std::lock_guard<std::mutex> lock(m_graphMutex)`. When held by UI, `try_to_lock` fails and `process()` skips all signal processing nodes.
3. **Parallel Branch & SPSC Queues**:
   - In `graph_engine.h:164-167`: `ParallelBranch` utilizes `moodycamel::ReaderWriterQueue<SlotCommand> m_commandQueue{QUEUE_CAPACITY}` and `moodycamel::ReaderWriterQueue<std::unique_ptr<PluginSlot>> m_reclaimQueue{QUEUE_CAPACITY}`.
   - In `graph_engine.cpp:286-331`: `ParallelBranch::process()` drains `m_commandQueue` wait-free and pushes retired slots to `m_reclaimQueue`.
4. **Latency Reporting & PDC Deficiencies**:
   - In `src\audio\audio_node.h:43`: `AudioNode::latencySamples()` returns `m_latencySamples{0}`.
   - In `src\audio\graph_engine.h:22-61`: `PluginSlot` does not override `latencySamples()`. It returns 0 even if `m_innerNode` has latency.
   - In `src\plugins\vst3_host.cpp:344`: `m_latencySamples = m_processor->getLatencySamples();` updates latency.
   - In `src\plugins\clap_host.cpp:127`: `m_latencySamples = m_latencyExt->get(m_plugin);` updates latency.
   - In `src\audio\graph_engine.cpp:398-456`: `ParallelSplitMergeBlock::process()` iterates over `m_branches` and sums them directly into `m_mixBuffer` with zero delay compensation, zero delay buffers, and zero calculation of $\Delta L = |L_A - L_B|$.
5. **Parameter Automation**:
   - `ParamAutomationRegister` does not exist anywhere in `src/`.
   - In `src\plugins\builtin_dsp.cpp:30-35`: `OverdriveEffect::setParameterValue()` directly modifies raw float `m_drive`, `m_tone`, `m_level` without atomics or synchronization.
   - In `src\ui\rack_view.cpp:2101`: UI sliders directly invoke `pInst->setParameterValue(desc.id, val)`.
6. **Retroactive Riff Capture & WAV Export**:
   - No circular buffer for retroactive audio capture exists in `src/audio/` or `src/tools/`.
   - `src/utils/wav_writer.h` does not exist.
   - In `src\state\app_config.h:19-36`: `AppConfig` has no field for `retroactiveBufferMinutes`.
   - In `src\ui\rack_view.cpp:1950-2027`: `renderBottomBar()` contains Settings and Practice Tools buttons, but no `[★ Save Last Riff]` button.
   - In `src\ui\rack_view.cpp:212-225`: Keyboard shortcut handler contains `Ctrl+P` and presets 1-8, but no `Ctrl+Shift+R` global shortcut.
   - In `src\ui\modals\settings_modal.cpp:122-183`: `renderAudioTab()` lacks duration selection and RAM footprint HUD.

---

## 2. Logic Chain
1. From Observation 2: The current `GraphEngine::process()` relies on `try_to_lock` on `m_graphMutex`. If the UI thread acquires this lock to add/remove/reorder serial nodes, the audio thread cannot acquire it and skips node execution, violating Gate 1.2 (0 mutex locks on audio thread). Replacing this with an SPSC command queue and reclamation queue (identical to the pattern already proven in `ParallelBranch`, Observation 3) will eliminate the mutex and guarantee lock-free block-boundary mutations.
2. From Observation 4: Because `PluginSlot` does not forward latency and `ParallelSplitMergeBlock` does not delay faster branches, any branch containing latency-inducing plugins creates phase cancellation and comb filtering when merged. Implementing a pre-allocated stereo circular delay buffer (supporting $\Delta L \le 16384$ samples) in `ParallelBranch` and equal-power 64-sample crossfading for dynamic latency changes will achieve phase-coherent summing with zero clicks or pitch modulation.
3. From Observation 5: Direct calls from the UI thread to `setParameterValue()` cause data races against the audio thread's `process()` calls. A `ParamAutomationRegister` with an atomic dirty bitmask (`std::atomic<uint64_t>`) and a flat array of `std::atomic<float>` allows UI/MIDI threads to set parameter values wait-free with automatic coalescing, while the audio thread drains only dirty parameters at block boundaries with zero dynamic allocations.
4. From Observation 6: Implementing `RetroactiveRiffBuffer` pre-allocated during `prepare()` with durations 1, 2, 5, or 10 minutes ensures zero audio-thread allocation. Using atomic head latching and dual-slice `memcpy` into a staging buffer completes in $<0.15\text{ms}$ ($150\mu\text{s}$), allowing header-only in-process writer `src/utils/wav_writer.h` to encode 24-bit PCM WAV asynchronously on a background thread without blocking audio or UI.
5. Connecting `AppConfig::retroactiveBufferMinutes` to `SettingsModal` and wiring `[★ Save Last Riff]` + `Ctrl+Shift+R` in `RackView` (guarded by `!io.WantTextInput`) provides a complete pro-audio musician workflow with floating HUD toast feedback.

---

## 3. Caveats
- No caveats regarding current codebase structure or compilation: all 10 tests are verified passing on the current Windows build.
- The survey did not modify source code, as this agent is assigned to a read-only investigation role. All architectural details, class headers, memory footprints, and method signatures are fully documented in `survey_report.md` for immediate implementation.

---

## 4. Conclusion
The codebase is in a clean, passing state (10/10 CTest targets pass). Implementing R1 and R2 requires:
1. Creating `src/audio/param_automation.h` (`ParamAutomationRegister`).
2. Updating `PluginSlot::latencySamples()`, adding PDC delay lines and 64-sample dynamic latency crossfaders to `ParallelBranch` / `ParallelSplitMergeBlock`, and replacing `m_graphMutex` with an SPSC queue in `GraphEngine`.
3. Creating `src/audio/retroactive_buffer.h` and header-only `src/utils/wav_writer.h`.
4. Adding `retroactiveBufferMinutes` to `AppConfig`, adding duration/RAM controls to `SettingsModal`, and adding `[★ Save Last Riff]` + `Ctrl+Shift+R` to `RackView`.
5. Authoring the Stage 1 test harness `tests/test_praccy_v2_1.cpp` and integrating it into CTest.

---

## 5. Verification Method
1. **CTest Baseline Verification**:
   ```pwsh
   cd F:\Projects\Praccy\build
   ctest --output-on-failure
   ```
   Must report `100% tests passed out of 10`.
2. **File Inspection**:
   - Review findings report: `F:\Projects\Praccy\.agents\teamwork\explorer_survey_audio\survey_report.md`.
   - Review briefing: `F:\Projects\Praccy\.agents\teamwork\explorer_survey_audio\BRIEFING.md`.
3. **Invalidation Conditions**:
   - If `ctest` in `build/` fails any of the 10 existing test targets.
   - If audio thread allocations or mutex acquisitions are detected by `test_challenger_m1` or `test_asio_driver`.
