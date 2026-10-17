### 1. EXECUTIVE SUMMARY

This execution plan remediates all **26 verified findings** (`F-01` through `F-26`: 6 Critical, 11 High, 8 Medium, 1 Low) identified in the forensic audit of the `Praccy` C++20 / Dear ImGui ASIO VST3 & CLAP audio plugin host. Rather than applying call-site band-aids, all 26 findings are resolved at their structural origin across **8 Root-Cause Clusters (`RCC-01`–`RCC-08`)**, organized into **6 Workstreams (`WS-1`–`WS-6`)** and **22 atomic tasks (`TASK-00`–`TASK-21`)** executed across **6 dependency waves (`Wave 0`–`Wave 5`)**.

* **Critical Path:** `TASK-00` (Build Consolidation & Sanitizer Harness) $\to$ `TASK-01` (Characterization & Failure-Reproduction Suite) $\to$ `TASK-03` (Wait-Free Parameter & Transport Bus) $\to$ `TASK-04` (GraphEngine Lock-Free Topology & Reclamation) $\to$ `TASK-06` (VST3/CLAP Thread-Safe State & Transport Integration) $\to$ `TASK-08` (Immutable Scene Snapshot Transitions) $\to$ `TASK-20` (Production Verification Replacement) $\to$ `TASK-21` (Full Sanitizer & Real-Time Gate).
* **Parallelism:** Up to 5 concurrent agent worktrees in Waves 2, 3, and 4 with zero overlapping write ownership (`files_owned`).
* **Top Risks:**
1. **Cross-Thread State Serialization Deadlocks:** Transitioning `SceneManager` away from live UI-thread `loadState()` calls requires off-thread node instantiation or audio-thread state swaps without blocking the ASIO callback.
2. **Windows Driver / SEH ABI Differences (MSVC vs. MinGW-w64):** Refactoring `crash_isolation.h` and `AsioManager` MMCSS thread affinity must preserve ABI compatibility across both compilers.
3. **High-DPI Layout Regressions:** Scaling `RackView` and modals dynamically on `WM_DPICHANGED` requires synchronized D3D11 font texture invalidation outside `ImGui::NewFrame()`.


* **Out of Scope:** Out-of-process IPC plugin sandboxing (requires a separate executable/shared-memory architecture beyond single-binary host remediation) and non-WAV compressed audio codecs (`.mp3`/`.flac` decoding libraries are not vendored).

### 2. ASSUMPTIONS AND OPEN QUESTIONS

#### Labeled Assumptions (Verified by Dedicated Tasks)

* **`ASM-01` (Toolchain Environment):** The target build environment supports CMake 3.20+ with either MSVC 2022 or MinGW-w64 GCC 12+/Clang 16+ and Windows SDK (`windows.h`, `d3d11.h`, `avrt.h`, `winmm.h`, `gdiplus.h`), while Linux CI agents can compile portable DSP/state/interop targets directly. *Resolved and verified by `TASK-00`.*
* **`ASM-02` (Maximum Parameter Address Space):** Hosted VST3 and CLAP plugins expose arbitrary 32-bit `paramId` hashes (not merely contiguous indices `0..1023`). Where `ParamAutomationRegister` (`MAX_PARAMS = 1024`) cannot index arbitrary 32-bit hashes directly, a lock-free MPSC ring buffer or hash-mapped slot table is required between non-audio producers (UI/MIDI) and the audio thread. *Resolved and verified by `TASK-03`.*
* **`ASM-03` (Single Active ASIO Driver Instance):** `Praccy` hosts at most one hardware `AsioManager` instance per process, though unit tests instantiate temporary `AsioManager` objects on the stack. *Resolved and verified by `TASK-11`.*
* **`ASM-04` (Single-Binary Crash Isolation Boundary):** Until an out-of-process plugin bridge is introduced in a future major release, in-process hardware fault isolation (`crash_isolation.h`) must never invoke `longjmp` across C++ stack frames with non-trivial destructors and must use fixed-capacity `char[128]` fault buffers instead of `std::string` on the audio thread. *Resolved and verified by `TASK-10`.*

#### Open Questions for Human Product Sign-Off (Non-Blocking Defaults Specified)

* **`OQ-01` (`ThumbnailManager` vs. `DeviceFaceplateBadge`):** Should `ThumbnailManager`'s live HWND `PrintWindow`/`BitBlt` capture be wired into `RackView::renderPluginCard` as an optional toggle over `DeviceFaceplateBadge`, or decommissioned so only vector hardware badges are rendered? *Default in `TASK-16`: Wire `ThumbnailManager::getThumbnail()` into `renderPluginCard` when `SettingsModal`'s thumbnail toggle is enabled, and bypass GDI+ polling when disabled.*
* **`OQ-02` (`Key Map Mode` `Ctrl+K` Scope):** Should `Ctrl+K` support binding computer keys (`A`–`Z`, `0`–`9`, `F1`–`F12`) to binary toggle controls (Bypass, Mute, Solo) as well as macro/knob nudges, and persist in `AppConfig` (`config.ini`)? *Default in `TASK-17`: Implement full click-to-bind computer key mapping for slot Bypass, Mute, Solo, and knob nudge/toggle persisted in `config.ini`.*
* **`OQ-03` (Audio Track Drag-and-Drop Target):** When a user drags a `.wav` file onto the main rack viewport while `PracticeToolsModal` is closed, should it load into `AudioPlayer` only (instead of both `AudioPlayer` and `QuickLooper`)? *Default in `TASK-17`: Route dropped `.wav` files to `QuickLooper` only when the Looper section is actively focused/hovered; otherwise route exclusively to `AudioPlayer`.*

### 3. FINDINGS REGISTER

| ID | Title | Severity | Category | Affected Files / Modules | Evidence & Current vs. Expected Behavior | Validity Status |
| --- | --- | --- | --- | --- | --- | --- |
| **`F-01`** | Multi-Producer Corruption of SPSC `ReaderWriterQueue` (`m_paramQueue` & `m_guiOutParamQueue`) | **CRITICAL** | Real-Time Concurrency | `src/plugins/vst3_host.h:162`, `src/plugins/vst3_host.cpp:166,200,532,657,668,761`, `src/plugins/clap_host.h:129-130`, `src/plugins/clap_host.cpp:271,285,477,628,629,639,672`, `src/audio/graph_engine.cpp:64-68`, `src/main.cpp:563-586` | **Current:** `moodycamel::ReaderWriterQueue` (strictly SPSC) is enqueued concurrently by UI thread (`setParameterValue`), WinMM MIDI thread (`midiInProc`), and ASIO Audio thread (`setParameterFromAudioThread`).<br>

<br>**Expected:** Each SPSC queue has exactly 1 producer thread and 1 consumer thread; audio-thread automation writes directly to audio-thread parameter structures. | **CONFIRMED** |
| **`F-02`** | Live UI-Thread `loadState()` Race Against Audio-Thread `process()` & DC Step in Crossfade Tail | **CRITICAL** | Real-Time Concurrency | `src/state/scene_manager.cpp:85-100,410-444`, `src/audio/graph_engine.cpp:1506-1507,1535-1549` | **Current:** `SceneManager::applyGraphSnapshotInPlace` calls `slot.automationRegister().reset()` and `inner->loadState()` on the UI thread while `process()` runs on the audio thread; `m_outgoingTailBuffer` repeats `m_lastBlockSampleL/R` as a DC shelf when `numSamples < rampSamples`.<br>

<br>**Expected:** Binary state chunk changes never execute concurrently with `process()`; crossfades blend two live signal paths or a full ring-buffered history. | **CONFIRMED** |
| **`F-03`** | `QuickLooper` 23–46 MB Synchronous `fill`/`memcpy` in Audio Callback & UI/Audio Buffer Data Races | **CRITICAL** | Real-Time Performance & Memory Safety | `src/tools/quick_looper.h:42-95`, `src/tools/quick_looper.cpp:45-138,147-186,239-252,277-281,310-365` | **Current:** `executeCommand` runs up to 46 MB of `std::fill`/`std::memcpy` inside the ASIO callback; UI methods run `executeCommand` directly whenever `m_isProcessingAudio` is `false` between blocks; `loadWavFile` calls `m_loopBufferL.resize()` on UI thread without synchronization.<br>

<br>**Expected:** $O(1)$ buffer swaps on audio thread; all mutations serialized via command/buffer swap queue; zero `resize()` on active buffers. | **CONFIRMED** |
| **`F-04`** | `AudioPlayer` Hazard Pointer Bypass on UI Thread (`seek`, `durationSeconds`, `positionSeconds`, `positionNormalized`) | **HIGH** | Memory Safety (UAF) | `src/tools/audio_player.h:106-114`, `src/tools/audio_player.cpp:19-36,160-163,203-225` | **Current:** UI thread methods load `m_activeBuffer` and dereference `buf->numFrames` / `buf->sampleRate` without acquiring `m_audioReadingBuffer`, racing against `retireBuffer(oldBuf)` on the loader `std::jthread`.<br>

<br>**Expected:** UI thread reads atomic metadata (`m_activeNumFrames`, `m_activeSampleRate`) and never dereferences raw `AudioTrackBuffer*`. | **CONFIRMED** |
| **`F-05`** | WinMM MIDI Thread Dereferences Unowned `GraphEngine` & `ParallelSplitMergeBlock` Raw Pointers | **CRITICAL** | Concurrency / Memory Safety (UAF) | `src/main.cpp:551-588`, `src/audio/graph_engine.h:315,436`, `src/audio/graph_engine.cpp:959,1084,1143` | **Current:** `midiInProc` calls `graph.getNode()` (which releases `m_uiMutex` upon return) and `block->getBranch()` (unsynchronized `std::vector`), racing against UI node/branch deletion.<br>

<br>**Expected:** MIDI parameter dispatch never holds raw node/branch pointers outside lock scope or routes through a lock-free audio-thread event queue. | **CONFIRMED** |
| **`F-06`** | `MidiManager` Seqlock Write Omission, Unverified Read, and Real-Time `std::string` Allocation | **HIGH** | Real-Time Concurrency | `src/midi/midi_types.h:58-70`, `src/midi/midi_manager.h:79-95`, `src/midi/midi_manager.cpp:63-99,150-154,189-203` | **Current:** `addBinding` and `addCcBinding` mutate `m_ccTable` without incrementing `m_ccTableSeq`; `handleControlChange` does not verify `seq1 == seq2` after reading and calls `toMidiBinding()`, allocating a `std::string` ($>21$ chars) on the WinMM thread.<br>

<br>**Expected:** Full seqlock write/read protocol and zero-allocation `FlatCcBinding` callback dispatch. | **CONFIRMED** |
| **`F-07`** | Audio-Thread `std::vector::push_back` Reallocation & Deleted Node Re-Insertion in `stashForReclamation` & `CrossfadeTo` | **CRITICAL** | Real-Time Safety & Graph Integrity | `src/audio/graph_engine.h:198,468`, `src/audio/graph_engine.cpp:380-383,783-789,820-831,1334` | **Current:** When `m_reclaimQueue` is full, `stashForReclamation` pushes retired nodes back into `m_activeSlots` (without capacity check, causing `operator new` reallocation and re-activating deleted plugins!); `CrossfadeTo` lacks `MAX_GRAPH_NODES` bounds check.<br>

<br>**Expected:** Pre-allocated fixed-capacity retirement ring buffer; retired nodes never re-enter active processing vectors. | **CONFIRMED** |
| **`F-08`** | Audio-Thread `std::string` Heap Allocation During Plugin Crash Latching | **HIGH** | Real-Time Safety | `src/plugins/vst3_host.cpp:638`, `src/plugins/clap_host.cpp:614`, `src/plugins/plugin_base.h:71`, `tests/test_challenger_m2_2.cpp:635-659` | **Current:** On hardware exception inside `process()`, `Vst3PluginInstance` and `ClapPluginInstance` execute `m_faultReason = std::string("VST3 crash: ") + getExceptionDescription(exCode);` on the audio thread.<br>

<br>**Expected:** Store `std::atomic<uint32_t> m_faultCode` or fixed `char m_faultReason[128]` with zero heap allocation on the audio thread. | **CONFIRMED** |
| **`F-09`** | Win32 VEH `longjmp` Over C++ Frames & Premature `_resetstkoflw()` in `crash_isolation.h` | **HIGH** | Stability / Undefined Behavior | `src/plugins/crash_isolation.h:77-79,97-115,134-156` | **Current:** `vehExceptionHandler` calls `longjmp` across C++ frames (`std::function`, RAII locks) and calls `_resetstkoflw()` before the stack pointer is unwound on the VEH path; `First=0` misses exceptions if a plugin registers a handler.<br>

<br>**Expected:** Pure `noexcept` C function-pointer trampoline with `_resetstkoflw()` invoked *after* stack restoration (`setjmp != 0`). | **CONFIRMED** |
| **`F-10`** | `AsioManager::packSamples` Signed `int32_t` Overflow UB at `+1.0f` & Unclamped `ASIOSTFloat32LSB` | **CRITICAL** | DSP / Undefined Behavior | `src/audio/asio_manager.cpp:458-470` | **Current:** `1.0f * 2147483647.0f` rounds to `2147483648.0f` in `float32`, overflowing `int32_t` to `-2147483648` (`INT32_MIN`) under UBSan; `ASIOSTFloat32LSB` ignores `±Inf` and values outside `[-1.0f, 1.0f]`.<br>

<br>**Expected:** Exact `double` scaling clamped to `[INT32_MIN, INT32_MAX]` and `std::isfinite` + `[-1.0f, 1.0f]` clamping for `Float32LSB`. | **CONFIRMED** |
| **`F-11`** | `AsioManager` Cross-Thread MMCSS Handle Reversion & Static `s_instance` Pointer Stomp | **HIGH** | OS Thread Affinity & Lifecycle | `src/audio/asio_manager.h:98-105`, `src/audio/asio_manager.cpp:13-16,189-193,204-208,224-228,498-501` | **Current:** `AvSetMmThreadCharacteristicsW` is called on the ASIO hardware thread (`line 500`), while `AvRevertMmThreadCharacteristics` is called on the UI thread (`line 225`), failing with `ERROR_INVALID_HANDLE`; secondary `AsioManager` instances overwrite `s_instance`.<br>

<br>**Expected:** Revert MMCSS handle on the ASIO callback thread via `thread_local` RAII guard; guard `s_instance` ownership exclusively for active driver instance. | **CONFIRMED** |
| **`F-12`** | Host Transport Context (`tempoBpm`, Time Signature) Hardcoded in VST3 and `nullptr` in CLAP | **HIGH** | Architectural Contradiction | `src/audio/audio_node.h:14-19`, `src/state/scene_manager.h:85-88`, `src/plugins/vst3_host.cpp:430-435,542-546`, `src/plugins/clap_host.cpp:553-554`, `src/main.cpp:494-502` | **Current:** VST3 `m_processContext.tempo` is hardcoded to `120.0` (`4/4`) and CLAP `processData.transport` is `nullptr`, ignoring `Metronome` and `ScenePreset` transport settings.<br>

<br>**Expected:** `AudioProcessContext` propagates live BPM, time signature, and sample position to both VST3 and CLAP plugins every block. | **CONFIRMED** |
| **`F-13`** | Master Safety Limiter Comment vs. Math Contradiction | **LOW** | Documentation vs. DSP | `README.md:70`, `src/audio/graph_engine.cpp:1568-1580` | **Current:** Comment claims `"transparent cubic saturator at +/-0.98 FS"`, while code computes `0.95f + 0.05f * std::tanh((x - 0.95f) / 0.05f)`.<br>

<br>**Expected:** Align soft-clipper implementation and documentation to a fast rational Padé `tanh` knee with verified threshold accuracy. | **CONFIRMED** |
| **`F-14`** | Redundant Buffer Copies in `PluginSlot::process`, `ParallelBranch::process`, and `GraphEngine::process` | **HIGH** | Real-Time DSP Efficiency | `src/audio/graph_engine.cpp:76-95,476-489,1521-1532` | **Current:** `PluginSlot::process` copies `ctx.input` to `dryView` twice when `inGain != 1.0f` and `wet < 1.0f`; serial and branch loops copy scratch buffers back on every slot instead of ping-pong swapping views.<br>

<br>**Expected:** Single dry reference copy and zero-copy ping-pong `AudioBufferView` swapping across slot chains. | **CONFIRMED** |
| **`F-15`** | Per-Sample Transcendentals in Hot Loops & Unvectorized $O(N^2)$ Modulo YIN Pitch Detector | **MEDIUM** | Real-Time DSP Efficiency | `src/audio/dsp_utils.h:124-129`, `src/audio/graph_engine.cpp:505-506,1575-1579`, `src/plugins/builtin_dsp.cpp:70,173-174`, `src/tools/metronome.cpp:58-61`, `src/tools/tuner.cpp:311-320` | **Current:** Per-sample `std::sin`/`std::cos`/`std::exp`/`std::tanh` in crossfades, metronome, and saturators; `detectPitchYin` runs `% m_bufferSize` inside a $4096 \times 4096$ inner loop.<br>

<br>**Expected:** Linearized contiguous window buffer for SIMD-vectorized YIN autocorrelation, phasor recurrence for `Metronome`/`EqualPowerRamp`, and SIMD-friendly Padé `tanh`. | **CONFIRMED** |
| **`F-16`** | `ClapPluginInstance::getPreferredSize` `const_cast` Mutation & Unparented GUI Resource Leak | **HIGH** | Plugin Host Lifecycle | `src/plugins/clap_host.cpp:742-763`, `src/plugins/plugin_window.cpp:95-115` | **Current:** `getPreferredSize() const` calls `m_guiExt->create()` and sets `const_cast<ClapPluginInstance*>(this)->m_guiCreated = true`, leaking an unparented GUI if `openGui()` is not called or fails.<br>

<br>**Expected:** `getPreferredSize()` is non-mutating or cleans up temporary size queries; GUI lifecycle is strictly owned by `openGui()`/`closeGui()`. | **CONFIRMED** |
| **`F-17`** | Dummy VST3 COM Refcounts (`return 1`/`1000`), Manual `new`, and 19 Silent `catch (...)` Blocks | **MEDIUM** | Modern C++ & Host Hygiene | `src/plugins/vst3_param_queue.h:45-46,103-104`, `src/plugins/vst3_host.cpp:54-55,110-111,147-148,321`, `src/plugins/clap_host.cpp:396`, `src/plugins/plugin_scanner.cpp:135,235` | **Current:** Host COM classes return fake refcounts (`1` or `1000`), risking UAF if a plugin retains `IComponentHandler` after `unload()`; `new Vst3PluginInstance()` used instead of `std::make_unique`; silent `catch (...)` discards error diagnostics.<br>

<br>**Expected:** Atomic COM refcounting with explicit host detachment on `unload()`, `std::make_unique`, and structured diagnostic logging. | **CONFIRMED** |
| **`F-18`** | Detached `std::thread` Use-After-Free on Exit in `UpdateChecker` & Unbounded WAV Export Threads | **HIGH** | Concurrency & Lifecycle | `src/ui/update_checker.h:59-72`, `src/ui/update_checker.cpp:142-145,267-270`, `src/utils/wav_writer.h:132-139` | **Current:** `UpdateChecker` spawns `.detach()` threads capturing `this`; if process exits during WinINet timeout, detached threads access destroyed `m_mutex`; `asyncWriteWav24Bit` spawns unbounded detached threads.<br>

<br>**Expected:** RAII `std::jthread` members joined on destruction with cancellation tokens (`std::stop_token`). | **CONFIRMED** |
| **`F-19`** | Unescaped INI Delimiters (` | `, `=`, `\n`), CLAP/BuiltIn Erasure in DAW Export, and Truncated XML Hex Entities | **MEDIUM** | Serialization & Interop | `src/state/app_config.cpp:80-111,203-221`, `src/state/scene_manager.cpp:782-1003`, `src/state/daw_interop/reaper_adapter.cpp:280`, `src/state/daw_interop/ableton_adapter.cpp:281`, `src/utils/xml_parser.cpp:62-65` | **Current:** Raw ` |
| **`F-20`** | Dead `ThumbnailManager` GDI+/D3D11 Subsystem Polling Every Frame with Zero Callers | **MEDIUM** | Architectural Contradiction | `src/ui/thumbnail_manager.h`, `src/ui/thumbnail_manager.cpp:1-416`, `src/ui/rack_view.cpp:358,2358,2363`, `src/ui/widgets/device_badges.h:112`, `src/ui/modals/settings_modal.cpp:533` | **Current:** `m_thumbnailMgr->update()` and `captureWindowAsync()` run every frame and allocate D3D11 textures, but `getThumbnail()` has 0 callers in `src/`.<br>

<br>**Expected:** Wire `getThumbnail()` into `RackView::renderPluginCard` gated by an explicit `AppConfig` setting, skipping GDI+ work when disabled. | **CONFIRMED** |
| **`F-21`** | Unimplemented `Key Map Mode` (`Ctrl+K`) Façade, Broken Focused `Q` Shortcut, and Dual-Load Drag-and-Drop | **MEDIUM** | UX & Feature Contract | `src/ui/rack_view.h:158`, `src/ui/rack_view.cpp:428-432,481-483,500-520,3048-3062`, `src/ui/drag_drop_router.cpp:25-26,120-127`, `src/main.cpp:418-420` | **Current:** `m_keyMapMode` renders a banner but zero widgets check it; focused `Q` ignores `m_focusedSlot`; `DragDropRouter` claims `.mp3`/`.flac` support (unsupported by `WavReader`) and loads `.wav` into both `player` and `looper`.<br>

<br>**Expected:** Functional `KeyMapMode` binding dispatch, context-aware `Q` Spotlight opening, and `.wav`-only single-target drop routing. | **CONFIRMED** |
| **`F-22`** | High-DPI Layout Collisions from Unscaled Pixel Literals & Missing `WM_DPICHANGED` Font Atlas Rebuild | **HIGH** | Dear ImGui High-DPI UX | `src/main.cpp:796-808`, `src/ui/rack_view.cpp:1224-1231,1695-1715,1742,1874,2005`, `src/ui/modals/tuner_settings_modal.cpp:107,129,192,259,272,275` | **Current:** `WM_DPICHANGED` never rebuilds `io.Fonts`; `RackView` and `TunerSettingsModal` use unscaled pixel offsets (`240.0f`, `224.0f`, `SameLine(130.0f)`), causing text/widget collisions at 150%–200% DPI.<br>

<br>**Expected:** Deferred pre-frame font atlas rebuild on DPI change and `ui::dpiScale()` applied across all card/modal layout dimensions. | **CONFIRMED** |
| **`F-23`** | Per-Frame UI Heap Churn, Inline `ColorToken` Literal (`rack_view.cpp:63`), and Global `s_suppressResetId` | **MEDIUM** | Dear ImGui Frame Performance | `src/ui/rack_view.cpp:63,948,1051,1066,1500-1503,2469-2472,3559-3691`, `src/ui/modals/tuner_settings_modal.cpp:307,400,614`, `src/tools/tuner.h:20`, `src/tools/tuner.cpp:445`, `src/ui/ui_helpers.h:72`, `src/ui/widgets/rotary_knob.h:72` | **Current:** Render loop allocates `std::string`, `std::vector`, `std::ostringstream`, and `unordered_map` keys every frame; `TunerResult::noteName` is a heap `std::string`; `rack_view.cpp:63` hardcodes `ColorToken(0,0,0,48)`.<br>

<br>**Expected:** Zero steady-state heap allocations per UI frame; fixed `char[8]` in `TunerResult`; semantic shadow token in `ThemeTokens`. | **CONFIRMED** |
| **`F-24`** | `LevelMeter` Peak-Hold & Sticky Clip Indicators Computed on Audio Thread Are Ignored by `RackView::renderMeter` | **MEDIUM** | Dear ImGui Metering UX | `src/audio/dsp_utils.h:148-214`, `src/ui/rack_view.h:122`, `src/ui/rack_view.cpp:1661-1662,1690-1691,2065-2066,3150-3251` | **Current:** `LevelMeter` computes `m_holdLeft`, `m_holdRight`, and `m_clipped` atomics, but `RackView::renderMeter` only takes `float level` and never renders peak-hold ticks or clip LEDs.<br>

<br>**Expected:** `renderMeter` renders peak-hold ticks and click-to-clear clip indicators from `LevelMeter`. | **CONFIRMED** |
| **`F-25`** | `CMakeLists.txt` 7x Duplicate Compilation (`PRACCY_CORE_SOURCES` Dead), 16 `-Wconversion` Warnings, and 3-Way Version Drift | **HIGH** | Build Hygiene & Type Safety | `CMakeLists.txt:39-43,60-499`, `src/version.h:3-7`, `README.md:11,29-31`, `src/utils/wav_reader.h:145,151,152`, `src/tools/quick_looper.cpp:229,326,331,334`, `src/tools/audio_player.cpp:93,103,106,206,271,273` | **Current:** `PRACCY_CORE_SOURCES` is defined at line 481 after all targets and never used; `-Wconversion` emits 16 warnings (including float24 frame-index precision loss after 5.8 mins); version strings disagree across `version.h` (`2.6.0`) and `README.md` (`2.4` / `beta2`).<br>

<br>**Expected:** Shared `praccy_core` library target, zero `-Wconversion` warnings, CMake sanitizer options, and unified version metadata. | **CONFIRMED** |
| **`F-26`** | Mock-Only "Verification Theatre" in Challenger & v2.1 Regression Test Suites | **HIGH** | Verification Integrity | `tests/test_challenger_m1_2.cpp:22-230`, `tests/test_challenger_stage1_audio.cpp:643-780`, `tests/test_challenger_stage1_2.cpp:25-31,101-170`, `tests/test_praccy_v2_1.cpp:1014-1463`, `tests/test_challenger_m4_2.cpp:34-260` | **Current:** Tests define local toy structs (`DinPpmBallistics`, `MockPluginItem`, local lambdas) instead of linking and calling production classes, allowing `F-01`..`F-24` to pass CI undetected.<br>

<br>**Expected:** Every test links `praccy_core` and exercises production methods under ASan, UBSan, TSan, and audio-thread allocation traps. | **CONFIRMED** |

### 4. ROOT CAUSE CLUSTERS

#### Cluster `RCC-01`: Unenforced Inter-Thread Messaging & Parameter Bus Contracts

* **Member Findings:** `F-01`, `F-05`, `F-06`, `F-12`
* **Structural Root Cause:** The codebase lacks a unified, type-safe inter-thread event/parameter bus between the **UI thread**, **WinMM MIDI callback thread**, and **ASIO Audio thread**. Instead:
1. `Vst3PluginInstance` and `ClapPluginInstance` expose raw SPSC `moodycamel::ReaderWriterQueue` instances (`m_paramQueue`, `m_guiOutParamQueue`) that are written to directly by UI methods (`setParameterValue`), MIDI callbacks (`main.cpp:585`), and audio-thread methods (`setParameterFromAudioThread`), violating the Single-Producer precondition.
2. `ParamAutomationRegister` (`MAX_PARAMS = 1024`) only supports contiguous indices `< 1024`, forcing arbitrary 32-bit VST3/CLAP parameter IDs to fall back to `innerNode->setParameterValue()` from the UI and MIDI threads (`src/audio/graph_engine.cpp:52-55`, `src/main.cpp:585`).
3. The WinMM MIDI callback (`main.cpp:551-588`) reaches directly into `GraphEngine::getNode()` and `ParallelSplitMergeBlock::getBranch()` to obtain raw pointers without holding a lock across the call, while `MidiManager` uses a half-implemented seqlock and heap-allocates `std::string` inside `FlatCcBinding::toMidiBinding()`.
4. Host transport state (`tempoBpm`, time signature, sample position) has no channel in `AudioProcessContext`, leaving VST3 hardcoded to 120 BPM and CLAP with `transport = nullptr`.


* **Latent Unreported Risks Discovered:**
* In `Vst3PluginInstance::onGuiPerformEdit` (`src/plugins/vst3_host.cpp:166`), when a user drags a knob inside a third-party VST3 native editor window, the plugin calls `performEdit` on the UI thread, which calls `m_paramQueue.try_enqueue` concurrently with MIDI CC automation and `PluginSlot::process` (`setParameterFromAudioThread`)!
* In `ParallelSplitMergeBlock::getBranch` (`src/audio/graph_engine.cpp:609`), `m_branches` is a `std::vector<std::unique_ptr<ParallelBranch>>` mutated by `addBranch`/`removeBranch` on the UI thread with **no mutex at all**, while read concurrently by both the WinMM MIDI thread (`main.cpp:566`) and the ASIO audio thread (`ParallelSplitMergeBlock::process`, `line 650`)!


* **Candidate Architectural Approaches:**
* *Approach A (Patchwork Mutexes):* Wrap `m_paramQueue` in a `std::mutex` and lock `m_uiMutex` inside `process()`. **Rejected:** Violates the zero-lock real-time audio thread invariant and causes priority inversion between the ASIO thread and UI/MIDI threads.
* *Approach B (Unified Lock-Free Parameter & Transport Bus — Chosen):*
1. Upgrade `ParamAutomationRegister` to support both fast $O(1)$ indexed parameters (`0..1023`) **and** a lock-free MPSC bounded slot array (64 atomic `{paramId, value}` slots using `std::atomic<uint64_t>` compare-and-swap packing `uint32_t paramId` + `float value`) so **any** 32-bit VST3/CLAP parameter ID can be written wait-free from both UI and MIDI threads into `PluginSlot` without ever touching `IPluginInstance` from non-audio threads.
2. Separate `IPluginInstance` queues strictly by direction and single producer:
* `m_uiToAudioParamQueue` (SPSC: UI thread is sole producer for native GUI `performEdit`; Audio thread is sole consumer).
* `m_audioToUiParamQueue` (SPSC: Audio thread is sole producer for CLAP output events and automation readback; UI thread is sole consumer).
* `setParameterFromAudioThread(paramId, value)` writes directly into audio-thread-owned structures (`m_inputParameterChanges` in VST3, `m_audioLocalParams` fixed array in CLAP)—never enqueuing into an SPSC queue on the audio thread.


3. Add `GraphEngine::dispatchParameterThreadSafe(slotIdx, branchIdx, branchSlotIdx, paramId, normalizedValue)` which acquires `m_uiMutex` (safe on UI/MIDI threads, never called on the audio thread) and writes to the target `PluginSlot::automationRegister()` while holding the lock, eliminating raw pointer escape to the WinMM thread.
4. Fix `MidiManager`'s seqlock (`m_ccTableSeq`) with full acquire/release sequence numbers on all writers (`addBinding`, `addCcBinding`, `stopLearnAndBind`, `removeBinding`) and pass `const FlatCcBinding&` directly to the callback without constructing `std::string`.
5. Extend `AudioProcessContext` with `tempoBpm`, `timeSigNumerator`, `timeSigDenominator`, and `projectTimeSamples`, populated lock-free from `Metronome` in `main.cpp` and consumed by both `Vst3PluginInstance::process` and `ClapPluginInstance::process`.





#### Cluster `RCC-02`: Shared Mutable Audio-Engine State & Non-Deterministic Reclamation

* **Member Findings:** `F-02`, `F-03`, `F-04`, `F-07`
* **Structural Root Cause:** Multiple subsystems (`SceneManager`, `QuickLooper`, `AudioPlayer`, `GraphEngine`, `ParallelSplitMergeBlock`) violate the **Immutable Audio-Object Handoff Principle** (where large buffers or stateful nodes are prepared off-thread, handed to the audio thread via an SPSC command, and retired back to a reclamation thread when replaced). Specifically:
1. `SceneManager::applyGraphSnapshotInPlace` mutates live `IPluginInstance` binary state (`loadState`) on the UI thread while `process()` is executing on the audio thread.
2. `QuickLooper` shares single `m_loopBufferL/R` vectors between UI and audio threads, executing 46 MB `memcpy`/`fill` on the audio thread and `resize()` on the UI thread.
3. `AudioPlayer` exposes raw `m_activeBuffer` pointers to UI getters without hazard-pointer protection.
4. `GraphEngine::stashForReclamation` and `ParallelBranch::stashForReclamation` push overflow retired nodes back into `m_activeNodes`/`m_activeSlots` on the audio thread, causing heap reallocation and resurrected zombie plugins.


* **Latent Unreported Risks Discovered:**
* `ParallelSplitMergeBlock::addBranch` (`src/audio/graph_engine.cpp:585`) and `removeBranch` (`line 596`) directly mutate `m_branches` (`std::vector::push_back` / `erase`) on the UI thread while `ParallelSplitMergeBlock::process` (`line 650`) iterates `m_branches` on the ASIO audio thread—an immediate Use-After-Free crash whenever a user adds or removes a parallel split branch while audio is running!
* `QuickLooper::prepare` (`src/tools/quick_looper.cpp:12`) can also be called when sample rate changes in `main.cpp:478` while the UI or audio thread accesses looper state.


* **Candidate Architectural Approaches:**
* *Approach A (Try-Locks in Audio Callback):* Add `std::mutex::try_lock` around `QuickLooper::process`, `PluginSlot::process`, and `ParallelSplitMergeBlock::process`, outputting silence on contention. **Rejected:** Causes audible dropouts during parameter undo, looper triggers, and branch edits, and still leaves 46 MB `memcpy` inside the audio callback.
* *Approach B (Strict Command-Driven RCU / Pointer-Swap Lifecycle — Chosen):*
1. **Lock-Free Fixed-Capacity Retirement Ring (`src/audio/graph_engine.h/.cpp`):** Replace the 256-item `m_reclaimQueue` fallback (`m_activeSlots.push_back` / `m_activeNodes.push_back`) with a pre-allocated fixed-capacity retirement buffer (`std::array<std::unique_ptr<AudioNode>, 1024>`) owned by the audio thread that drains into `m_reclaimQueue` each block and **never** pushes retired nodes back into active processing vectors. Route `ParallelSplitMergeBlock` branch additions/removals through an SPSC command queue (`BranchCommand`) identical to `GraphEngine` and `ParallelBranch`.
2. **Safe Scene Snapshot Application (`src/state/scene_manager.cpp`):** In `SceneManager::canApplyInPlace`, compare a fast 64-bit FNV-1a hash of each slot's last-applied `sp.state` against the incoming preset's `sp.state`. Only use `applyGraphSnapshotInPlace` when the binary state chunk hash is unchanged (updating scalar parameters via `slot.setParameter(id, val)` without calling `inner->loadState()` or `automationRegister().reset()`). Whenever a VST3/CLAP binary chunk differs, construct the new node graph off-thread and transition via `graph.crossfadeToNodes(std::move(newNodes), 10.0f)`. Also replace `m_outgoingTailBuffer`'s 1-block DC clamp with true dual-graph crossfade evaluation during `CrossfadeTo`.
3. **Wait-Free `QuickLooper` Pointer Swap & Audio-Owned State (`src/tools/quick_looper.h/.cpp`):** Never execute `executeCommand` on the UI thread when `m_isRunning` is active; replace `Undo`/`Redo` 46 MB `memcpy` with $O(1)$ `std::vector::swap` on pre-allocated buffers; record pre-overdub samples into `m_undoBufferL/R[head]` sample-by-sample during `Overdubbing`; and implement `loadWavFile` by allocating a new `LooperTrackBuffer` on the caller thread and swapping it onto the audio thread via an SPSC pointer handoff (retired via reclamation queue).
4. **Atomic Metadata Cache in `AudioPlayer` (`src/tools/audio_player.h/.cpp`):** Store `m_activeNumFrames` (`std::atomic<size_t>`) and `m_activeSampleRate` (`std::atomic<double>`) updated upon buffer swap in `executeLoad`, removing all `m_activeBuffer` pointer dereferences from UI-thread `seek()`, `durationSeconds()`, `positionSeconds()`, and `positionNormalized()`.





#### Cluster `RCC-03`: Unchecked Hardware Driver, Crash-Isolation & Thread-Lifecycle Boundaries

* **Member Findings:** `F-08`, `F-09`, `F-10`, `F-11`, `F-16`, `F-17`, `F-18`
* **Structural Root Cause:** Boundary adapters interfacing with Win32 OS primitives (VEH/SEH exception dispatch, MMCSS thread characteristics, WinINet background threads), ASIO DMA sample buffers, and VST3/CLAP C/COM ABIs violate low-level OS and C++ ABI invariants:
1. `crash_isolation.h` mixes `setjmp`/`longjmp` with C++ stack frames (`std::function`, RAII objects) inside a Win32 Vectored Exception Handler (`First=0`) and allocates `std::string` on the audio thread when formatting crash descriptions.
2. `AsioManager::packSamples` multiplies `float` by `2147483647.0f` (which rounds to $2^{31}$ in IEEE-754 float32), causing signed `int32_t` overflow UB at `+1.0f`, and calls `AvRevertMmThreadCharacteristics` on the UI thread instead of the ASIO hardware thread.
3. `ClapPluginInstance::getPreferredSize` mutates state inside a `const` method via `const_cast`, and VST3 COM host objects use dummy refcounts (`return 1`).
4. `UpdateChecker` and `asyncWriteWav24Bit` spawn `.detach()` threads capturing `this` or unbounded buffers without joinable lifecycle management.


* **Latent Unreported Risks Discovered:**
* In `AsioManager::unpackSamples` (`src/audio/asio_manager.cpp:427`), `ASIOSTFloat32LSB` copies raw driver input floats directly into the host input buffer without checking `std::isfinite(s)`! A rogue virtual ASIO driver or hardware glitch feeding `NaN` into the input poisons every downstream filter (`BiquadFilter` state variables `m_z1`, `m_z2` in `builtin_dsp.h:42` and `tuner.cpp:74` permanently latch to `NaN`!).
* In `Vst3PluginInstance::unload` (`src/plugins/vst3_host.cpp:365-402`), `m_componentHandler` (`PraccyComponentHandler`) is a direct member of `Vst3PluginInstance`; if a plugin's `IEditController` invokes `m_componentHandler` during teardown or from a lingering timer, it accesses a partially destroyed `Vst3PluginInstance`.


* **Candidate Architectural Approaches:**
* *Approach A (Keep `longjmp` + Suppress UBSan):* Add `__attribute__((no_sanitize("undefined")))` on `packSamples` and keep `longjmp` in `crash_isolation.h`. **Rejected:** Hides real hardware phase-inversion pops (`-2147483648` output at `0 dBFS`) and leaves stack corruption UB intact.
* *Approach B (ABI-Safe C Trampolines, Exact Quantizers & RAII Thread/COM Guards — Chosen):*
1. **Zero-Allocation C-Trampoline Crash Isolation (`src/plugins/crash_isolation.h`, `plugin_base.h`, `vst3_host.cpp`, `clap_host.cpp`):** Refactor `safeCallPluginAudio` to invoke a plain `void(*fn)(void*) noexcept` C trampoline over a `void* ctx` containing zero C++ objects with destructors, register the VEH handler with `First=1` ( inspecting only thread-local guarded plugin scopes and passing non-hardware C++ exceptions `0xE06D7363` through to the standard C++ unwinder via `EXCEPTION_CONTINUE_SEARCH`), call `_resetstkoflw()` *after* `setjmp` returns non-zero on the restored stack frame, and store fault state in a fixed `char m_faultReason[128]` / `std::atomic<uint32_t> m_faultCode{0}` with zero heap allocation.
2. **Bit-Exact Sanitized ASIO Quantizer & Thread-Local MMCSS Guard (`src/audio/asio_manager.h/.cpp`):** Sanitize both `unpackSamples` and `packSamples` against `NaN`/`±Inf`, compute `ASIOSTInt32LSB` scaling in `double` (`std::lrint(std::clamp<double>(s, -1.0, 1.0) * 2147483647.0)`), manage the MMCSS `"Pro Audio"` handle via a `thread_local` RAII guard on the ASIO callback thread, and protect `AsioManager::s_instance` with compare-exchange ownership tied to `loadDriver`/`unloadDriver`.
3. **Strict Plugin GUI & COM Lifecycle (`src/plugins/clap_host.cpp`, `src/plugins/vst3_host.cpp`, `src/plugins/vst3_param_queue.h`):** Make `ClapPluginInstance::getPreferredSize` side-effect-free (only querying `m_guiExt->get_size` if `m_guiCreated` is already true, or destroying the temporary query view immediately before returning), and give `PraccyComponentHandler` / `PraccyPlugFrame` atomic reference counts (`std::atomic<uint32_t>`) with an explicit `detachPlugin()` nulling guard on `unload()`.
4. **Joinable Worker Lifecycle (`src/ui/update_checker.h/.cpp`, `src/utils/wav_writer.h`):** Replace all `.detach()` threads in `UpdateChecker` with `std::jthread` members (`m_checkThread`, `m_downloadThread`) that honor `std::stop_token` and join cleanly in `~UpdateChecker()`, and bound `asyncWriteWav24Bit` with a shared single-worker task queue or `std::jthread` pool.





#### Cluster `RCC-04`: Suboptimal Real-Time DSP & Buffer Pipeline Design

* **Member Findings:** `F-13`, `F-14`, `F-15`
* **Structural Root Cause:** The DSP pipeline treats `AudioBufferView` as an in-place mutable target requiring defensive copies before and after every slot (`PluginSlot::process`, `ParallelBranch::process`, `GraphEngine::process`), evaluates expensive transcendental functions (`std::sin`, `std::cos`, `std::tanh`, `std::exp`) sample-by-sample in scalar loops, and runs autocorrelation (`InstrumentTuner::detectPitchYin`) directly over a modulo-indexed ring buffer.
* **Latent Unreported Risks Discovered:**
* In `ParallelSplitMergeBlock::process` (`src/audio/graph_engine.cpp:681-720`), when summing multiple parallel branches, if no branch is soloed, every branch runs its own `EqualPowerRamp` pan calculation per sample.


* **Candidate Architectural Approaches:**
* *Approach A (Vendor External SIMD DSP Library):* Pull in a heavy third-party FFT/SIMD library. **Rejected:** Sandbox/offline build has no outbound internet access and must rely solely on standard C++20 and compiler auto-vectorization / SSE2 intrinsics.
* *Approach B (Ping-Pong Buffer Views, Phasor Recurrence & Contiguous SIMD Windowing — Chosen):*
1. Eliminate the duplicate `dryView.copyFrom(ctx.input)` in `PluginSlot::process` (`src/audio/graph_engine.cpp:76-95`) and use pointer-swapping (`std::swap`) between `mainView` and `scratchView` across serial/branch slot chains.
2. Replace per-sample `std::sin`/`std::cos`/`std::exp` in `Metronome::process` and `EqualPowerRamp` with recursive complex phasor rotation / exponential decay multipliers ($y[n] = c \cdot y[n-1]$), and replace `std::tanh` in `GraphEngine::process` (`lines 1575-1579`), `OverdriveEffect`, and `TubeAmpEffect` with a branchless $C^2$-continuous Padé `[3/2]` rational saturator (`dsp_utils.h::fastTanh`) matching its documented ceiling.
3. In `InstrumentTuner::detectPitchYin` (`src/tools/tuner.cpp:311-320`), unwrap the circular ring buffer once into a pre-allocated contiguous linear member `std::vector<float> m_linearWindow` (`2 * m_bufferSize`) so the inner difference loop (`for (size_t j = 0; j < halfBuffer; ++j)`) is a contiguous pointer stride that GCC/Clang/MSVC auto-vectorizes with SSE2/AVX2 (`8x` speedup, zero `%` modulo).





#### Cluster `RCC-05`: Ad-Hoc Unescaped String Serialization & Format-Blind DAW Adapters

* **Member Findings:** `F-19`
* **Structural Root Cause:** `AppConfig` and `SceneManager` construct and parse `.ini` files using raw `std::getline` and `|`/`=` token splitting without a shared escaping codec, `XmlParser::decodeEntities` casts hex codepoints to a single `char` only when `<= 0x7F`, and `ReaperAdapter`/`AbletonAdapter` hardcode VST3 XML/RPP schemas without branching on `PluginSlotPreset::type`.
* **Latent Unreported Risks Discovered:**
* `AppConfig::save` (`src/state/app_config.cpp:104`) writes `recents` and `favorites` plugin names delimited by `|` or `,`; a plugin name containing `,` splits into two fake favorite entries on reload.


* **Candidate Architectural Approaches:**
* *Approach A (Replace `.ini` with JSON/SQLite):* Break backward compatibility with existing `config.ini` and `presets.ini` files. **Rejected:** Breaks existing user presets and test fixtures (`test_challenger_m2_1.cpp`).
* *Approach B (Bi-Directional Escaping Codec in `parse_utils.h` + Multi-Format DAW Adapter Dispatch — Chosen):*
1. Add `escapeDelimitedField(std::string_view)` / `unescapeDelimitedField(std::string_view)` in `src/utils/parse_utils.h` (encoding `\`, `|`, `=`, `,`, `\r`, `\n` as `\xHH` escapes while remaining 100% backward-compatible with unescaped legacy ASCII strings) and apply across `AppConfig` and `SceneManager`.
2. Upgrade `XmlParser::decodeEntities` (`src/utils/xml_parser.cpp:60-68`) to encode any valid Unicode scalar value (`0x01..0x10FFFF`) into proper 1–4 byte UTF-8 sequences.
3. Update `ReaperAdapter::exportFromScene` and `AbletonAdapter::exportFromScene` to branch on `slot.type` (`"VST3"`, `"CLAP"`, `"BuiltIn"`) and preserve parallel branch gain/pan/mute/solo metadata.





#### Cluster `RCC-06`: Disconnected UI Feature Wiring & Dead Subsystems

* **Member Findings:** `F-20`, `F-21`, `F-24`
* **Structural Root Cause:** UI features were added as visual chrome (`Key Map Mode` banner, `ThumbnailManager` background capture, `LevelMeter` peak-hold/clip atomic computation, `DragDropRouter` file extension table) without completing the data-flow loop between the UI widgets and the underlying state/audio engines.
* **Candidate Architectural Approaches:**
* *Approach A (Delete All Incomplete Features):* Strip `Key Map Mode`, `ThumbnailManager`, and peak-hold meters from the codebase. **Rejected:** Degrades documented product capabilities in `README.md` and wastes already-built DSP/GDI infrastructure.
* *Approach B (Complete End-to-End UI Data-Flow Wiring — Chosen):*
1. **Peak-Hold & Sticky Clip Metering (`F-24`):** Update `RackView::renderMeter` to accept `const audio::LevelMeter& meter, int channel` (or `float peak, float hold, bool clipped, std::function<void()> onResetClip`), rendering the 2px peak-hold bar and clickable sticky clip LED cap.
2. **Gated Live Thumbnails (`F-20`):** Add `bool enableLiveThumbnails{false}` to `AppConfig`; when enabled, render `m_thumbnailMgr->getThumbnail(pluginPath)` inside `RackView::renderPluginCard`, and when disabled, skip `m_thumbnailMgr->update()` and `captureWindowAsync()` completely.
3. **Functional `Key Map Mode`, Context-Aware `Q` Shortcut & Clean `.wav` Drop Routing (`F-21`):** Implement `KeyBinding` registration and hotkeys in `RackView` when `m_keyMapMode == true`, pass `(m_focusedBlockIndex, m_focusedBranchIndex)` to `m_pluginBrowserModal->open()` on `Q`, restrict `DragDropRouter::classifyFile` to `.wav`, and route dropped `.wav` files to a single target (`QuickLooper` if focused, else `AudioPlayer`).





#### Cluster `RCC-07`: Immediate-Mode GUI Frame-Budget & High-DPI Scaling Architecture

* **Member Findings:** `F-22`, `F-23`
* **Structural Root Cause:** Layout dimensions in `RackView` and `TunerSettingsModal` were authored as hardcoded 96-DPI pixel literals instead of functions of `ui::dpiScale()`, `WM_DPICHANGED` updates `s_dpiScale` inside `WndProc` mid-message-loop without rebuilding the Dear ImGui D3D11 font atlas before `ImGui::NewFrame()`, and render methods construct temporary `std::string`/`std::vector` objects on every 144 Hz frame.
* **Candidate Architectural Approaches:**
* *Approach A (Use `io.FontGlobalScale`):* Scale existing font textures via bilinear filtering in `WM_DPICHANGED`. **Rejected:** Produces blurry, pixelated typography on 150%–200% High-DPI displays.
* *Approach B (Deferred Pre-Frame Font Atlas Rebuild + `dpiScale()` Layout Helper & Zero-Alloc Frame Loop — Chosen):*
1. In `src/main.cpp`, have `WM_DPICHANGED` set an atomic/bool flag `g_pendingDpiScale = newScale;`. At the very top of the Win32 render loop (before `ImGui_ImplDX11_NewFrame()`), rebuild `io.Fonts` at `15.0f * g_pendingDpiScale` and `13.0f * g_pendingDpiScale` and call `ImGui_ImplDX11_InvalidateDeviceObjects()` + `ImGui_ImplDX11_CreateDeviceObjects()`.
2. Scale all layout constants in `RackView` and `TunerSettingsModal` via `ui::px(float basePixels)` (`return std::round(basePixels * ui::dpiScale());`).
3. Replace per-frame `std::string`/`std::vector`/`std::ostringstream` allocations in `RackView`, `TunerSettingsModal`, and `InstrumentTuner::currentResult()` (`TunerResult::noteName` $\to$ `std::array<char, 8>`) with stack-allocated `char` buffers and cached vectors invalidated only on state mutation; move `ColorToken(0, 0, 0, 48)` at `src/ui/rack_view.cpp:63` into `ThemeTokens::surfaces.cardShadow`; and move `s_suppressResetId` into ImGui storage (`ImGui::GetStateStorage()`).





#### Cluster `RCC-08`: Build Graph Fragmentation, Narrowing Conversions & Mock-Only Test Harnesses

* **Member Findings:** `F-25`, `F-26`
* **Structural Root Cause:** `CMakeLists.txt` lacks a shared `praccy_core` library target (defining `PRACCY_CORE_SOURCES` at the bottom of the file after all targets), disables strict narrowing diagnostics (`-Wconversion`), and allows test suites (`test_praccy_v2_1.cpp`, `test_challenger_stage1_2.cpp`, `test_challenger_m1_2.cpp`) to compile standalone mock classes rather than linking against production headers/sources.
* **Candidate Architectural Approaches:**
* *Approach A (Keep Duplicate Source Lists in `CMakeLists.txt`):* Only fix compiler warnings in `src/`. **Rejected:** Leaves build times 7x slower and permits tests to stub out production symbols (`SceneManager::applyGraphSnapshot` stubbed at `tests/test_challenger_stage1_2.cpp:25-31`).
* *Approach B (Unified `praccy_core` Library Target, Strict Compiler/Sanitizer Gates & Production-Linked Tests — Chosen):*
1. Define `praccy_core` (`STATIC` library) at the top of `CMakeLists.txt` before all executables, link all 16 test targets against `praccy_core`, add CMake options `PRACCY_ENABLE_ASAN_UBSAN` and `PRACCY_ENABLE_TSAN`, enable `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor`, fix all 16 narrowing conversions in `wav_reader.h`, `quick_looper.cpp`, and `audio_player.cpp`, and synchronize version strings across `src/version.h`, `CMakeLists.txt`, and `README.md` (`2.6.0`).
2. Replace all mock/stubbed tests in `test_praccy_v2_1.cpp`, `test_challenger_stage1_2.cpp`, `test_challenger_m1_2.cpp`, `test_challenger_stage1_audio.cpp`, and `test_challenger_m4_2.cpp` with production assertions against the real classes.





### 5. TARGET ARCHITECTURE

#### Structural Contracts & Invariants Enforced by Target Architecture

1. **Invariant `INV-RT-01` (Single-Producer Queue Ownership & Wait-Free Parameter Bus):**
* Every `moodycamel::ReaderWriterQueue` instance in the codebase has a statically documented **single producer thread** and **single consumer thread**, enforced via debug thread-ID assertions (`PRACCY_ASSERT_PRODUCER_THREAD`).
* All parameter updates originating from the UI thread or WinMM MIDI thread target `PluginSlot::setParameter(uint32_t paramId, float value)` which writes wait-free to `ParamAutomationRegister` (upgraded with a 64-slot atomic CAS overflow table for arbitrary 32-bit VST3/CLAP parameter IDs $\ge 1024$).
* `IPluginInstance::setParameterFromAudioThread(uint32_t paramId, double value)` is called **exclusively** on the audio thread and appends directly to `m_inputParameterChanges` (VST3) or `m_audioLocalParams` (CLAP) without touching `m_uiToAudioParamQueue`.


2. **Invariant `INV-RT-02` (Zero Audio-Thread Heap Allocation & Bounded Reclamation):**
* No method reachable from `AsioManager::processAudio` or `MidiManager::midiInProc` may invoke `operator new`, `operator delete`, `std::vector::push_back` without a prior `size() < capacity()` guard, `std::string` construction, or `std::fill`/`std::memcpy` exceeding $O(\text{blockSize})$.
* Retired `AudioNode`, `PluginSlot`, `ParallelBranch`, `AudioTrackBuffer`, and `LooperTrackBuffer` objects are transferred via fixed-capacity SPSC reclamation queues to the UI/housekeeping thread (`processReclamation()`) and never re-inserted into active audio vectors.


3. **Invariant `INV-STATE-03` (Immutable Plugin State Chunks During Active Audio):**
* `IPluginInstance::loadState()` (`IComponent::setState` / `clap_plugin_state::load`) is **never** called on a plugin instance that is currently attached to `GraphEngine::m_activeNodes` while audio processing is active. In-place snapshot application (`applyGraphSnapshotInPlace`) is restricted to scalar parameter updates when the 64-bit FNV-1a hash of `PluginSlotPreset::state` matches the active slot's state hash.


4. **Invariant `INV-ABI-04` (C-Trampoline Crash Isolation & Thread-Affine OS Handles):**
* `safeCallPluginAudio` executes across a pure C function pointer (`void(*)(void*) noexcept`) with zero C++ stack frames between `setjmp` and the plugin boundary, and writes fault telemetry into a fixed `char[128]` buffer.
* Thread-affine Win32 handles (`AvSetMmThreadCharacteristicsW`) are owned and reverted by a `thread_local` RAII guard on the exact thread that created them.


5. **Invariant `INV-UI-05` (DPI-Proportional Layout & Zero Steady-State UI Frame Allocations):**
* All pixel dimensions in `src/ui/` are scaled by `ui::px(base)` (`base * ui::dpiScale()`), font atlases rebuild at the top of the main loop before `ImGui::NewFrame()` when `WM_DPICHANGED` triggers, and steady-state `RackView::render` performs zero heap allocations per frame.



### 6. DEPENDENCY GRAPH AND EXECUTION ORDER

```
[WAVE 0: Foundation, Build Consolidation & Characterization Harness (Serialized Setup)]
  └── TASK-00 (CMake praccy_core + Sanitizer Targets + -Wconversion Fixes + Version Sync)
        └── TASK-01 (Baseline Characterization & Failure-Reproduction Test Suite)

[WAVE 1: Core Lock-Free Primitives, Crash ABI & Escape Codecs (5x Parallel)]
  ├── TASK-02 (Parse/INI Escaping Codec & UTF-8 XML Entities)        [owns: parse_utils.h, xml_parser.*]
  ├── TASK-03 (ParamAutomationRegister 32-Bit Hash Bus & Transport)  [owns: param_automation.h, audio_node.h, dsp_utils.h]
  ├── TASK-10 (ABI-Safe Crash Isolation C-Trampoline & Fixed Faults) [owns: crash_isolation.h, plugin_base.h]
  ├── TASK-11 (AsioManager Bit-Exact Quantizer & Thread-Local MMCSS) [owns: asio_manager.h, asio_manager.cpp]
  └── TASK-13 (Joinable UpdateChecker & Bounded Async WAV Writer)    [owns: update_checker.*, wav_writer.h]

[WAVE 2: Engine Concurrency, Host Lifecycles & Practice Tools (5x Parallel)]
  ├── TASK-04 (GraphEngine Bounded Reclamation, Ping-Pong & Thread-Safe Param Dispatch) [owns: graph_engine.h, graph_engine.cpp]
  ├── TASK-05 (MidiManager Seqlock & Zero-Alloc FlatCcBinding Dispatch)                 [owns: midi_types.h, midi_manager.*]
  ├── TASK-06 (VST3 & CLAP SPSC Separation, Transport, GUI Const & COM Refcounting)     [owns: vst3_host.*, vst3_param_queue.h, clap_host.*, plugin_window.*, plugin_scanner.cpp]
  ├── TASK-09 (QuickLooper O(1) Swap & AudioPlayer Atomic Metadata Cache)               [owns: quick_looper.*, audio_player.*]
  └── TASK-12 (SIMD-Vectorized Linear YIN Tuner, Phasor Metronome & Padé BuiltIn DSP)   [owns: tuner.*, metronome.*, builtin_dsp.*]

[WAVE 3: State Serialization, Scene Transitions & UI Design Tokens (3x Parallel)]
  ├── TASK-07 (AppConfig Escaped INI Persistence & Live Thumbnail Config Flag)          [owns: app_config.h, app_config.cpp]
  ├── TASK-08 (SceneManager State-Hash Guarded Transitions & Escaped Preset INI)        [owns: scene_manager.h, scene_manager.cpp]
  ├── TASK-14 (DAW Interop Multi-Format CLAP/VST3/BuiltIn Export & Branch Preservation) [owns: src/state/daw_interop/*]
  └── TASK-15 (UI Design Tokens Shadow Token, DPI Helper & Widget Storage ID)           [owns: design_tokens.h, ui_helpers.h, rotary_knob.h, tuner_settings_modal.*]

[WAVE 4: RackView UX Completion & Main Loop Integration (2x Sequential on UI/Main)]
  └── TASK-16 (RackView Metering, Gated Thumbnails, High-DPI Scaling & Zero-Alloc Loop) [owns: rack_view.h, rack_view.cpp, thumbnail_manager.*, settings_modal.cpp]
        └── TASK-17 (KeyMapMode Binding Engine, Context-Aware 'Q' & WAV Drop Routing)   [owns: rack_view.*, drag_drop_router.*]
              └── TASK-18 (main.cpp WM_DPICHANGED Pre-Frame Font Rebuild & MIDI Wiring) [owns: main.cpp]

[WAVE 5: Verification Theatre Replacement & Final Sanitizer Gate (Serialized Closure)]
  └── TASK-19 (Multi-Threaded Concurrency, ASIO UBSan & Seqlock Regression Suite)       [owns: tests/test_challenger_m1_2.cpp, tests/test_challenger_m2_2.cpp, tests/test_challenger_m4_2.cpp]
        └── TASK-20 (Replace Mocked Tests in test_praccy_v2_1 & test_challenger_stage1) [owns: tests/test_praccy_v2_1.cpp, tests/test_challenger_stage1_2.cpp, tests/test_challenger_stage1_audio.cpp]
              └── TASK-21 (Final E2E Build, Static Analysis & ASan/UBSan/TSan Gate)     [read-only verification gate]

```

* **Hard Serialization Points:**
1. `TASK-00` modifies `CMakeLists.txt` and must merge before any wave compiles against `praccy_core`.
2. `TASK-03` modifies `src/audio/audio_node.h` and `src/audio/param_automation.h`; it must complete before `TASK-04` (`graph_engine.*`) and `TASK-06` (`vst3_host.*`, `clap_host.*`) begin.
3. `TASK-16` and `TASK-17` both modify `src/ui/rack_view.h` and `src/ui/rack_view.cpp` and are therefore serialized in Wave 4 (`TASK-16` $\to$ `TASK-17` $\to$ `TASK-18`).



### 7. SWARM TOPOLOGY

#### Agent Roles

1. **`build-infra-engineer`:** Owns `CMakeLists.txt`, compiler warning flags, sanitizer presets, and build graph deduplication (`TASK-00`).
2. **`rt-audio-architect`:** Owns real-time audio graph, lock-free queues, parameter automation register, DSP math, and practice tools (`TASK-03`, `TASK-04`, `TASK-09`, `TASK-12`).
3. **`systems-security-engineer`:** Owns Win32 VEH/SEH crash isolation, ASIO DMA sample packing/MMCSS thread affinity, MIDI seqlock, RAII worker threads, and escaping codecs (`TASK-02`, `TASK-05`, `TASK-10`, `TASK-11`, `TASK-13`).
4. **`plugin-state-engineer`:** Owns VST3/CLAP host wrappers, COM lifetimes, `AppConfig`, `SceneManager`, and DAW interop adapters (`TASK-06`, `TASK-07`, `TASK-08`, `TASK-14`).
5. **`imgui-ux-engineer`:** Owns Dear ImGui design tokens, High-DPI scaling, `RackView`, modals, `DragDropRouter`, and `main.cpp` UI/DPI integration (`TASK-15`, `TASK-16`, `TASK-17`, `TASK-18`).
6. **`verification-architect`:** Authors characterization reproductions (`TASK-01`), replaces mocked tests with production-linked tests (`TASK-19`, `TASK-20`), and runs final gate verification (`TASK-21`).
7. **`independent-reviewer`:** Performs mandatory code, concurrency, and contract review on every task before merge. **Never shares identity or context with the task's implementing agent.**

#### Isolation, Ownership & Merge Protocol

* **Git Worktree Isolation:** The orchestrator creates an isolated git repository (`git init && git add -A && git commit -m "baseline"` if `.git` is absent in the unpacked archive) and provisions one worktree per active task (`git worktree add ../wt-TASK-XX -b task/TASK-XX`).
* **Strict File Ownership Enforcement:** Before merging any task branch, the orchestrator executes `git diff --name-only main...task/TASK-XX` and verifies that **every modified file** is listed in that task's `files_owned` manifest. Any modification outside `files_owned` triggers an immediate automated rejection.
* **Wave Barriers:** All tasks within Wave $N$ are reviewed by `independent-reviewer`, fast-forward merged into `main`, and verified against the cumulative test suite before Wave $N+1$ worktrees are branched from `main`.

### 8. TASK MANIFEST

```yaml
tasks:
  - id: TASK-00
    title: "Consolidate CMake praccy_core Target, Enable Strict Warnings/Sanitizers, Fix 16 Narrowing Warnings & Unify Version"
    workstream: WS-1-Build-And-Foundation
    cluster: RCC-08
    findings_addressed: ["F-25"]
    agent_role: build-infra-engineer
    depends_on: []
    blocks: ["TASK-01", "TASK-02", "TASK-03", "TASK-10", "TASK-11", "TASK-13"]
    files_owned:
      - "CMakeLists.txt"
      - "src/version.h"
      - "README.md"
      - "src/utils/wav_reader.h"
    files_read_only:
      - "src/tools/quick_looper.cpp"
      - "src/tools/audio_player.cpp"
    objective: >
      Eliminate 7x duplicate compilation in CMakeLists.txt by promoting PRACCY_CORE_SOURCES to a reusable
      STATIC library target (praccy_core) before all executable targets, add CMake options for ASan/UBSan/TSan
      and strict compiler warnings (-Wconversion -Wshadow -Wpedantic -Wnon-virtual-dtor), fix the -Wconversion
      warnings in wav_reader.h, and unify version strings across version.h, CMakeLists.txt, and README.md.
    instructions:
      - "If the workspace root is not yet a git repository, initialize it (`git init && git add -A && git commit -m 'Initial baseline snapshot'`)."
      - "In `CMakeLists.txt`, move the `PRACCY_CORE_SOURCES` definition (currently at lines 481-499, which is dead code) to line 60 before `add_executable(Praccy ...)`, create `add_library(praccy_core STATIC ${PRACCY_CORE_SOURCES})`, configure its include directories and link libraries once, and link `Praccy` and all 16 test executables against `praccy_core`."
      - "Update the outdated comment at `CMakeLists.txt:303` from `13 test targets` to `16 test targets`."
      - "Add CMake options `PRACCY_ENABLE_ASAN_UBSAN` and `PRACCY_ENABLE_TSAN` to `CMakeLists.txt` and enable `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` for GCC/Clang and `/W4` for MSVC."
      - "In `src/utils/wav_reader.h:145,151,152`, fix `-Wconversion` warnings by casting integer sample values explicitly to `double` before dividing (`static_cast<float>(static_cast<double>(val >> 8) / 8388608.0)` and `static_cast<float>(static_cast<double>(p32[0]) / 2147483648.0)`)."
      - "In `src/version.h:3-7` and `README.md:11,29-31`, synchronize the version string to `2.6.0` (`v2.6.0`)."
    inputs:
      - "Baseline repository archive"
    outputs:
      - "Updated CMakeLists.txt with praccy_core static library and sanitizer flags"
      - "Warning-free src/utils/wav_reader.h and synchronized version headers"
    acceptance_criteria:
      - "`PRACCY_CORE_SOURCES` is defined before all targets and linked via `praccy_core`."
      - "`g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -fsyntax-only src/utils/wav_reader.h` exits with 0 warnings."
      - "`src/version.h`, `CMakeLists.txt`, and `README.md` all report version `2.6.0`."
    verification_commands:
      - "g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -fsyntax-only src/utils/wav_reader.h"
      - "grep -n '2.6.0' src/version.h CMakeLists.txt README.md"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- CMakeLists.txt src/version.h README.md src/utils/wav_reader.h"
    risk_level: LOW
    stop_conditions:
      - "Any existing test target fails to resolve symbols when linked against praccy_core."

  - id: TASK-01
    title: "Author Pre-Refactor Characterization & Bug-Reproduction Harness"
    workstream: WS-6-Verification-And-Gates
    cluster: RCC-08
    findings_addressed: ["F-26"]
    agent_role: verification-architect
    depends_on: ["TASK-00"]
    blocks: ["TASK-02", "TASK-03", "TASK-10", "TASK-11", "TASK-13"]
    files_owned:
      - "tests/test_remediation_characterization.cpp"
    files_read_only:
      - "src/audio/asio_manager.cpp"
      - "src/tools/quick_looper.cpp"
      - "src/tools/audio_player.cpp"
      - "src/midi/midi_manager.cpp"
    objective: >
      Create a self-contained characterization and bug-reproduction test suite (`tests/test_remediation_characterization.cpp`)
      before refactoring begins, capturing baseline behavior and deterministic reproductions for UBSan int32 overflow (F-10),
      QuickLooper concurrent race/allocation (F-03), AudioPlayer UI getter UAF (F-04), MidiManager seqlock/allocation (F-06),
      and INI delimiter injection (F-19).
    instructions:
      - "Create `tests/test_remediation_characterization.cpp` with deterministic test functions for each cluster invariant."
      - "Include a mode flag (`--verify-fixed`) so that before remediation it documents the exact failure modes and after remediation (`TASK-21`) it asserts 100% pass under ASan, UBSan, and TSan."
    inputs:
      - "TASK-00 consolidated build configuration"
    outputs:
      - "tests/test_remediation_characterization.cpp"
    acceptance_criteria:
      - "`tests/test_remediation_characterization.cpp` compiles cleanly under `g++ -std=c++20 -Isrc -fsanitize=address,undefined`."
    verification_commands:
      - "g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -fsyntax-only tests/test_remediation_characterization.cpp"
    reviewer_role: independent-reviewer
    rollback_procedure: "rm -f tests/test_remediation_characterization.cpp"
    risk_level: LOW
    stop_conditions: []

  - id: TASK-02
    title: "Implement Bi-Directional INI Escaping Codec & Full UTF-8 XML Numeric Entity Decoder"
    workstream: WS-4-State-And-DAW-Interop
    cluster: RCC-05
    findings_addressed: ["F-19"]
    agent_role: systems-security-engineer
    depends_on: ["TASK-01"]
    blocks: ["TASK-07", "TASK-08", "TASK-14"]
    files_owned:
      - "src/utils/parse_utils.h"
      - "src/utils/xml_parser.h"
      - "src/utils/xml_parser.cpp"
    files_read_only:
      - "tests/test_interop_fuzz.cpp"
    objective: >
      Prevent delimiter injection (`|`, `=`, `,`, `\r`, `\n`, `\`) in INI state files by adding bi-directional
      escaping functions in `src/utils/parse_utils.h`, and fix `XmlParser::decodeEntities` (`src/utils/xml_parser.cpp:60-68`)
      so numeric XML character references (`&#x...;` and `&#...;`) above `0x7F` are encoded as valid multi-byte UTF-8
      instead of being silently dropped.
    instructions:
      - "In `src/utils/parse_utils.h`, add `escapeIniField(std::string_view input)` and `unescapeIniField(std::string_view input)`. Encode `\\` as `\\\\`, `|` as `\\p`, `=` as `\\e`, `,` as `\\c`, `\\n` as `\\n`, and `\\r` as `\\r`. Ensure strings without escape sequences decode identically for 100% backward compatibility with existing `.ini` files."
      - "In `src/utils/xml_parser.cpp:60-68`, replace the `val > 0 && val <= 0x7F` ASCII-only check with a proper UTF-8 encoder supporting Unicode scalar values `0x01..0xD7FF` and `0xE000..0x10FFFF` (1 to 4 UTF-8 bytes), and validate that the entire entity substring consists of valid hex/decimal digits."
    inputs:
      - "src/utils/parse_utils.h, src/utils/xml_parser.cpp"
    outputs:
      - "Updated parse_utils.h with escapeIniField/unescapeIniField and UTF-8 compliant xml_parser.cpp"
    acceptance_criteria:
      - "Round-trip `unescapeIniField(escapeIniField(s)) == s` holds for all strings containing `|`, `=`, `,`, `\\`, and newlines."
      - "`XmlParser::parse` decodes `&#x00E9;` (`é`) and `&#x266F;` (`♯`) into valid UTF-8 sequences."
    verification_commands:
      - "g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -fsanitize=address,undefined src/utils/xml_parser.cpp -c -o /tmp/xml_parser.o"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/utils/parse_utils.h src/utils/xml_parser.h src/utils/xml_parser.cpp"
    risk_level: LOW
    stop_conditions:
      - "Any existing XML test in `tests/test_interop_fuzz.cpp` or `tests/test_interop_roundtrip.cpp` regresses."

  - id: TASK-03
    title: "Upgrade ParamAutomationRegister for Arbitrary 32-Bit IDs, Extend AudioProcessContext Transport & Optimize DspUtils"
    workstream: WS-2-RealTime-Audio-Core
    cluster: RCC-01
    findings_addressed: ["F-01", "F-12", "F-13", "F-15"]
    agent_role: rt-audio-architect
    depends_on: ["TASK-01"]
    blocks: ["TASK-04", "TASK-06", "TASK-12"]
    files_owned:
      - "src/audio/param_automation.h"
      - "src/audio/audio_node.h"
      - "src/audio/dsp_utils.h"
    files_read_only: []
    objective: >
      Eliminate the root cause of multi-producer SPSC queue corruption (F-01) by allowing `ParamAutomationRegister`
      to accept arbitrary 32-bit VST3/CLAP parameter IDs (`>= 1024`) wait-free from any producer thread (UI or MIDI),
      extend `AudioProcessContext` with live transport metadata (F-12), and add branchless Padé `fastTanh` and
      phasor-based `EqualPowerRamp` in `dsp_utils.h` (F-13, F-15).
    instructions:
      - "In `src/audio/param_automation.h`, keep the fast 1024-entry direct array for `paramId < MAX_PARAMS`, and add a 64-slot lock-free CAS overflow table (`struct OverflowEntry { std::atomic<uint64_t> packed{0}; }` encoding `uint32_t paramId` in the high 32 bits and IEEE-754 `float` bits in the low 32 bits, plus a `std::atomic<uint64_t> m_overflowDirtyMask{0}`). In `setParameter(uint32_t paramId, float value)`, if `paramId >= MAX_PARAMS`, hash/probe into the 64-slot table using atomic `store`/`fetch_or` so arbitrary 32-bit VST3/CLAP parameter IDs are handled wait-free without ever falling back to `IPluginInstance::setParameterValue`."
      - "Remove `reset()`'s non-thread-safe bulk zeroing from active audio paths or mark it `resetBeforePrepareOnly() noexcept`."
      - "In `src/audio/audio_node.h:14-19`, add `double tempoBpm{120.0};`, `int32_t timeSigNumerator{4};`, `int32_t timeSigDenominator{4};`, and `int64_t projectTimeSamples{0};` to `AudioProcessContext`."
      - "In `src/audio/dsp_utils.h`, add `inline float fastTanh(float x) noexcept` using a clamped Padé `[3/2]` rational polynomial (`x * (27.0f + x2) / (27.0f + 9.0f * x2)` for `|x| < 3.0f`, clamped to `±1.0f` outside), and optimize `EqualPowerRamp::getNextGains` (`lines 124-129`) to use a fast polynomial approximation instead of per-sample `std::cos`/`std::sin`."
    inputs:
      - "src/audio/param_automation.h, src/audio/audio_node.h, src/audio/dsp_utils.h"
    outputs:
      - "Updated param_automation.h, audio_node.h, and dsp_utils.h"
    acceptance_criteria:
      - "`ParamAutomationRegister::setParameter(0x80001234u, 0.75f)` succeeds wait-free and is drained by `drainDirty()` with 0 heap allocations."
      - "`AudioProcessContext` carries `tempoBpm`, `timeSigNumerator`, `timeSigDenominator`, and `projectTimeSamples`."
      - "`EqualPowerRamp` energy conservation (`dry*dry + wet*wet`) remains within `1e-3f` of `1.0f` across all ramp positions."
    verification_commands:
      - "g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -Wshadow -fsyntax-only src/audio/param_automation.h src/audio/audio_node.h src/audio/dsp_utils.h"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/audio/param_automation.h src/audio/audio_node.h src/audio/dsp_utils.h"
    risk_level: MEDIUM
    stop_conditions:
      - "Energy conservation error in `EqualPowerRamp` exceeds tolerance in `test_challenger_m4_1.cpp`."

  - id: TASK-10
    title: "Refactor Win32 Crash Isolation to Pure C-Trampoline ABI & Zero-Allocation Fault Telemetry"
    workstream: WS-3-Plugin-Host-And-OS-Boundaries
    cluster: RCC-03
    findings_addressed: ["F-08", "F-09"]
    agent_role: systems-security-engineer
    depends_on: ["TASK-01"]
    blocks: ["TASK-06", "TASK-19"]
    files_owned:
      - "src/plugins/crash_isolation.h"
      - "src/plugins/plugin_base.h"
    files_read_only: []
    objective: >
      Eliminate C++ stack-frame `longjmp` Undefined Behavior and premature `_resetstkoflw()` in `src/plugins/crash_isolation.h`
      (F-09), and replace heap-allocated `std::string m_faultReason` mutations on the audio thread with fixed-capacity
      zero-allocation fault buffers in `IPluginInstance` (F-08).
    instructions:
      - "In `src/plugins/crash_isolation.h`, refactor the internal execution path so that on MinGW/GCC (`!defined(_MSC_VER)`), `setjmp(ctx.jmpBuf)` is invoked inside a `noexcept` C-compatible trampoline function `detail::sehTrampoline(void(*fnPtr)(void*) noexcept, void* fnCtx, DWORD* outExCode)` that contains zero C++ objects with non-trivial destructors."
      - "In `vehExceptionHandler` (`src/plugins/crash_isolation.h:97-115`), only intercept hardware faults (`EXCEPTION_ACCESS_VIOLATION`, `EXCEPTION_ARRAY_BOUNDS_EXCEEDED`, `EXCEPTION_DATATYPE_MISALIGNMENT`, `EXCEPTION_FLT_*`, `EXCEPTION_INT_*`, `EXCEPTION_ILLEGAL_INSTRUCTION`, `EXCEPTION_PRIV_INSTRUCTION`, `EXCEPTION_IN_PAGE_ERROR`, `EXCEPTION_STACK_OVERFLOW`) when `t_currentCrashContext != nullptr && t_currentCrashContext->active`. Do NOT call `_resetstkoflw()` inside `vehExceptionHandler` before `longjmp`; instead, call `_resetstkoflw()` immediately after `setjmp(ctx.jmpBuf) != 0` when `ctx.exceptionCode == EXCEPTION_STACK_OVERFLOW` on the restored stack frame!"
      - "In `src/plugins/plugin_base.h`, add a helper `struct FaultState { std::atomic<bool> faulted{false}; std::atomic<uint32_t> exceptionCode{0}; char reason[128]{}; }` (or `formatFaultReason(char (&buf)[128], const char* prefix, uint32_t code) noexcept` in `crash_isolation.h`) that formats `"VST3 crash: Access Violation (0xC0000005)"` into a pre-allocated fixed `char[128]` buffer using `std::snprintf` with zero heap allocations."
    inputs:
      - "src/plugins/crash_isolation.h, src/plugins/plugin_base.h"
    outputs:
      - "ABI-safe crash_isolation.h and zero-allocation fault telemetry helpers in plugin_base.h"
    acceptance_criteria:
      - "Zero `std::string` heap allocations occur when latching a plugin crash on the audio thread."
      - "`_resetstkoflw()` is called only after stack frame restoration on stack overflow faults."
    verification_commands:
      - "grep -n 'snprintf' src/plugins/crash_isolation.h"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/plugins/crash_isolation.h src/plugins/plugin_base.h"
    risk_level: MEDIUM
    stop_conditions:
      - "Nested `safeCallPluginAudio` recovery in `test_challenger_m2_2.cpp` fails."

  - id: TASK-11
    title: "Fix AsioManager Signed Int32 Overflow UB, Float32 Clamping, MMCSS Thread Affinity & Singleton Guard"
    workstream: WS-3-Plugin-Host-And-OS-Boundaries
    cluster: RCC-03
    findings_addressed: ["F-10", "F-11"]
    agent_role: systems-security-engineer
    depends_on: ["TASK-01"]
    blocks: ["TASK-19"]
    files_owned:
      - "src/audio/asio_manager.h"
      - "src/audio/asio_manager.cpp"
    files_read_only:
      - "src/audio/asio_defs.h"
    objective: >
      Fix the signed 32-bit integer overflow UB at `+1.0f` in `AsioManager::packSamples` (`ASIOSTInt32LSB`) and missing
      `±Inf` / range clamping in `ASIOSTFloat32LSB` and `unpackSamples` (F-10), fix cross-thread `AvRevertMmThreadCharacteristics`
      calls by managing the MMCSS handle via a `thread_local` RAII guard on the ASIO callback thread (F-11), and prevent
      secondary `AsioManager` instances from stomping `s_instance` while a driver is loaded (F-11).
    instructions:
      - "In `src/audio/asio_manager.cpp:458-460` (`AsioManager::packSamples` case `ASIOSTInt32LSB`), replace `static_cast<int32_t>(sample * 2147483647.0f)` with exact `double` scaling: `const double scaled = std::clamp(static_cast<double>(sample), -1.0, 1.0) * 2147483647.0; static_cast<int32_t*>(dst)[i] = static_cast<int32_t>(std::lrint(scaled));`."
      - "In `AsioManager::packSamples` case `ASIOSTFloat32LSB` (`lines 464-470`), replace `std::isnan(sample)` with `float s = src[i]; if (!std::isfinite(s)) s = 0.0f; static_cast<float*>(dst)[i] = std::clamp(s, -1.0f, 1.0f);`. In `AsioManager::unpackSamples` case `ASIOSTFloat32LSB` (`line 427`), sanitize non-finite input floats (`if (!std::isfinite(s)) s = 0.0f;`)."
      - "Add public static helpers `AsioManager::packInt32LSB(const float* src, int32_t* dst, uint32_t numSamples) noexcept` and `AsioManager::packFloat32LSB(const float* src, float* dst, uint32_t numSamples) noexcept` so unit tests can directly verify all sample formats under UBSan."
      - "In `src/audio/asio_manager.cpp:189-193, 204-208, 224-228, 498-501`, remove cross-thread `AvRevertMmThreadCharacteristics(m_mmcssHandle)` from `start()` and `stop()` on the UI thread. Instead, inside `AsioManager::processAudio`, use a `thread_local` RAII struct (`ThreadLocalMmcssGuard`) that calls `AvSetMmThreadCharacteristicsW(L\"Pro Audio\", &taskIdx)` on the ASIO hardware thread and calls `AvRevertMmThreadCharacteristics(handle)` when the ASIO hardware thread exits."
      - "In `AsioManager::AsioManager()` and `~AsioManager()` (`lines 15-23`), only assign `s_instance = this` if `s_instance == nullptr` (or when `loadDriver` succeeds), and only clear `s_instance = nullptr` in `~AsioManager()` if `s_instance == this`."
    inputs:
      - "src/audio/asio_manager.h, src/audio/asio_manager.cpp"
    outputs:
      - "UBSan-clean, MMCSS thread-affine AsioManager implementation"
    acceptance_criteria:
      - "Packing `1.0f` into `ASIOSTInt32LSB` produces `+2147483647` (`INT32_MAX`) with 0 UBSan errors (never `-2147483648`)."
      - "Packing `+Inf`, `-Inf`, `NaN`, `+1.5f`, and `-1.5f` into `ASIOSTFloat32LSB` clamps cleanly to `0.0f`, `+1.0f`, and `-1.0f`."
      - "`AvRevertMmThreadCharacteristics` is never called from the UI thread."
    verification_commands:
      - "g++ -std=c++20 -Isrc -fsanitize=address,undefined -fno-omit-frame-pointer -x c++ - -o /tmp/test_int32_pack <<< '#include <cassert>\n#include <cstdint>\n#include <cmath>\n#include <algorithm>\nint main(){ volatile float s = 1.0f; double sc = std::clamp(static_cast<double>(s), -1.0, 1.0) * 2147483647.0; int32_t p = static_cast<int32_t>(std::lrint(sc)); assert(p == 2147483647); return 0; }' && /tmp/test_int32_pack"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/audio/asio_manager.h src/audio/asio_manager.cpp"
    risk_level: MEDIUM
    stop_conditions:
      - "Any 24-bit bit-exactness test in `test_challenger_m1_2.cpp` regresses."

  - id: TASK-13
    title: "Replace Detached Threads in UpdateChecker & wav_writer.h with Joinable std::jthread Workers"
    workstream: WS-3-Plugin-Host-And-OS-Boundaries
    cluster: RCC-03
    findings_addressed: ["F-18"]
    agent_role: systems-security-engineer
    depends_on: ["TASK-01"]
    blocks: ["TASK-21"]
    files_owned:
      - "src/ui/update_checker.h"
      - "src/ui/update_checker.cpp"
      - "src/utils/wav_writer.h"
    files_read_only: []
    objective: >
      Eliminate Use-After-Free on application exit in `UpdateChecker` (`src/ui/update_checker.cpp:142-145, 267-270`)
      and unbounded thread spawning in `utils::asyncWriteWav24Bit` (`src/utils/wav_writer.h:132-139`) by replacing
      `.detach()` threads with joinable `std::jthread` workers and bounded concurrency.
    instructions:
      - "In `src/ui/update_checker.h`, add `std::jthread m_checkThread;` and `std::jthread m_downloadThread;`, plus a destructor `~UpdateChecker()` that requests stop (`m_checkThread.request_stop(); m_downloadThread.request_stop();`) and joins both threads before `m_mutex` and `m_info` are destroyed."
      - "In `src/ui/update_checker.cpp:142-145` and `267-270`, assign to `m_checkThread = std::jthread([this](std::stop_token st) { ... });` and `m_downloadThread = std::jthread([this](std::stop_token st) { ... });` (never `.detach()`), checking `st.stop_requested()` inside the download loop (`line 228`)."
      - "In `src/utils/wav_writer.h:132-139`, replace the unbounded `std::thread(...).detach()` with a bounded static `AsyncWavExportQueue` (owning a single joinable `std::jthread` worker that drains a mutex-guarded queue of pending export jobs and joins cleanly on process exit, or drops concurrent duplicate requests if more than 4 exports are already queued)."
    inputs:
      - "src/ui/update_checker.h, src/ui/update_checker.cpp, src/utils/wav_writer.h"
    outputs:
      - "Joinable std::jthread lifecycle in UpdateChecker and bounded async WAV exporter in wav_writer.h"
    acceptance_criteria:
      - "Zero `.detach()` calls remain in `src/ui/update_checker.cpp` and `src/utils/wav_writer.h`."
      - "Destroying `UpdateChecker` immediately after calling `checkForUpdates()` and `startDownload()` joins cleanly with 0 ASan/TSan errors."
    verification_commands:
      - "! grep -n 'detach()' src/ui/update_checker.cpp src/utils/wav_writer.h"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/ui/update_checker.h src/ui/update_checker.cpp src/utils/wav_writer.h"
    risk_level: LOW
    stop_conditions: []

  - id: TASK-04
    title: "Refactor GraphEngine & ParallelBranch for Bounded Reclamation, Lock-Free Branch Commands, Ping-Pong Buffers & Thread-Safe MIDI Dispatch"
    workstream: WS-2-RealTime-Audio-Core
    cluster: RCC-02
    findings_addressed: ["F-01", "F-02", "F-05", "F-07", "F-13", "F-14", "F-15"]
    agent_role: rt-audio-architect
    depends_on: ["TASK-03"]
    blocks: ["TASK-08", "TASK-16", "TASK-18", "TASK-20"]
    files_owned:
      - "src/audio/graph_engine.h"
      - "src/audio/graph_engine.cpp"
    files_read_only:
      - "src/audio/param_automation.h"
      - "src/audio/audio_node.h"
      - "src/audio/dsp_utils.h"
    objective: >
      Eliminate audio-thread `std::vector::push_back` reallocations and zombie node re-insertion in `GraphEngine`
      and `ParallelBranch` (F-07), synchronize `ParallelSplitMergeBlock` branch mutations via SPSC commands (F-05),
      add `GraphEngine::dispatchParameterThreadSafe` so the WinMM MIDI thread never touches raw pointers outside
      `m_uiMutex` (F-05), eliminate redundant buffer copies in `PluginSlot::process` and slot chains (F-14),
      and update the master safety limiter to use `DspUtils::fastTanh` (F-13, F-15).
    instructions:
      - "In `src/audio/graph_engine.cpp:51-57` (`PluginSlot::setParameter`), remove the `else if (m_innerNode)` fallback because `ParamAutomationRegister` (from `TASK-03`) now handles all 32-bit `paramId` values wait-free!"
      - "In `PluginSlot::process` (`src/audio/graph_engine.cpp:74-96`), eliminate the second `dryView.copyFrom(ctx.input)` (`line 94`) by keeping `dryView` unscaled and applying `inGain` into a pre-allocated `m_inputScaledBuffer` when `std::abs(inGain - 1.0f) > 1e-6f`."
      - "In `ParallelBranch` (`src/audio/graph_engine.h:198`, `src/audio/graph_engine.cpp:380-383`) and `GraphEngine` (`src/audio/graph_engine.h:468`, `src/audio/graph_engine.cpp:783-789`), increase `m_reclaimQueue` capacity to `1024`, add a pre-reserved audio-thread overflow vector `m_deferredReclaim` (`reserve(MAX_GRAPH_NODES)` in constructor/prepare) that drains into `m_reclaimQueue` at the start of `process()`, and NEVER push retired nodes back into `m_activeSlots` or `m_activeNodes`!"
      - "In `GraphEngine::drainCommandQueueIntoActive` (`src/audio/graph_engine.cpp:820-831`) and `GraphEngine::process` (`line 1334`), guard `m_activeNodes.push_back` during `CrossfadeTo` with `if (m_activeNodes.size() < MAX_GRAPH_NODES)`."
      - "In `ParallelSplitMergeBlock` (`src/audio/graph_engine.h:260-325`, `src/audio/graph_engine.cpp:560-725`), separate `m_uiBranches` (guarded by a mutex) from `m_activeBranches` (audio-thread owned, updated via an SPSC `m_branchCmdQueue` and `m_branchReclaimQueue`), eliminating the data race when adding/removing branches while audio is running."
      - "Add `bool GraphEngine::dispatchParameterThreadSafe(int slotIdx, int branchIdx, int branchSlotIdx, uint32_t paramId, float normalizedValue) noexcept` which locks `m_uiMutex`, validates indices against `m_uiNodes`, resolves the `PluginSlot*`, and calls `slot->setParameter(paramId, normalizedValue)` while holding `m_uiMutex`."
      - "In `GraphEngine::process` (`src/audio/graph_engine.cpp:1506-1507, 1535-1549`), when `CrossfadeTo` transitions between an old node list and a new node list, keep the outgoing nodes in a pre-allocated `m_fadingOutNodes` vector for the duration of the 10ms crossfade ramp (`m_crossfadeRemainingSamples > 0`), processing both `m_fadingOutNodes` (into `m_outgoingTailBuffer`) and `m_activeNodes` (into `mainView`) and blending them with `EqualPowerRamp` before retiring `m_fadingOutNodes` when the ramp completes! This eliminates the 1-block DC shelf artifact (`m_lastBlockSampleL/R`) completely."
      - "In `GraphEngine::process` (`lines 1568-1580`), update the Master Safety Limiter comment to match the `0.95` soft-knee `tanh` math and use `DspUtils::fastTanh`."
    inputs:
      - "TASK-03 updated audio_node.h, param_automation.h, dsp_utils.h"
    outputs:
      - "Refactored src/audio/graph_engine.h and src/audio/graph_engine.cpp"
    acceptance_criteria:
      - "Zero `m_activeSlots.push_back` or `m_activeNodes.push_back` calls exist inside `stashForReclamation`."
      - "`GraphEngine::dispatchParameterThreadSafe` safely dispatches parameters under `m_uiMutex` without returning raw pointers."
      - "During a 10ms `crossfadeToNodes` transition across multiple audio blocks, both outgoing and incoming chains are evaluated without repeating a frozen DC sample."
    verification_commands:
      - "! grep -n 'm_activeSlots.push_back' src/audio/graph_engine.cpp | grep -v 'AddSlot'"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/audio/graph_engine.h src/audio/graph_engine.cpp"
    risk_level: HIGH
    stop_conditions:
      - "Any PDC phase-cancellation or SPSC stress test in `test_challenger_stage1_audio.cpp` fails."

  - id: TASK-05
    title: "Fix MidiManager Seqlock Protocol & Eliminate Real-Time std::string Allocations in CC Dispatch"
    workstream: WS-2-RealTime-Audio-Core
    cluster: RCC-01
    findings_addressed: ["F-06"]
    agent_role: systems-security-engineer
    depends_on: ["TASK-01"]
    blocks: ["TASK-18", "TASK-20"]
    files_owned:
      - "src/midi/midi_types.h"
      - "src/midi/midi_manager.h"
      - "src/midi/midi_manager.cpp"
    files_read_only: []
    objective: >
      Fix the broken seqlock protocol in `MidiManager` where `addBinding` and `addCcBinding` mutate `m_ccTable`
      without incrementing `m_ccTableSeq`, verify `seq1 == seq2` after copying `CcTargetList` in `handleControlChange`,
      and eliminate the real-time `std::string` heap allocation caused by calling `FlatCcBinding::toMidiBinding()`
      when learned MIDI bindings (`id` length > 15 chars) fire on the WinMM callback thread.
    instructions:
      - "In `src/midi/midi_manager.cpp:63-76` (`addBinding`) and `lines 84-99` (`addCcBinding`), wrap all mutations to `m_ccTable` in the seqlock write protocol: `m_ccTableSeq.fetch_add(1, std::memory_order_acquire);` before modifying `m_ccTable`, and `m_ccTableSeq.fetch_add(1, std::memory_order_release);` after modifying `m_ccTable`."
      - "In `src/midi/midi_manager.cpp:189-203` (`handleControlChange`), implement a complete lock-free seqlock read loop (bounded to 2 retries): read `seq1 = m_ccTableSeq.load(std::memory_order_acquire);`, if `(seq1 & 1u) != 0u` return, copy `const CcTargetList exactSlot = m_ccTable[chIdx][ccNumber];` and `const CcTargetList omniSlot = (chIdx != 0) ? m_ccTable[0][ccNumber] : CcTargetList{};` onto the stack, execute `std::atomic_thread_fence(std::memory_order_acquire);`, and verify `m_ccTableSeq.load(std::memory_order_relaxed) == seq1` before dispatching!"
      - "To eliminate the `std::string` heap allocation in `toMidiBinding()` when `b.id` exceeds 15 characters (e.g. `\"cc_1_11_s0_b-1_sl-1_p1000\"` formatted in `stopLearnAndBind` at `line 150`), add `using FlatBindingCallback = std::function<void(const FlatCcBinding& binding, float mappedValue)>;` and `setFlatBindingCallback(FlatBindingCallback cb)` (and in `toMidiBinding()` for legacy callers, truncate or omit copying `id` in `handleControlChange` by invoking `m_flatBindingCallback` directly when set, or keeping `FlatCcBinding::id` under 15 chars `"c%X_%X_%d_%u"` so `std::string` SSO is guaranteed even on legacy `setBindingCallback` callers!)."
    inputs:
      - "src/midi/midi_types.h, src/midi/midi_manager.h, src/midi/midi_manager.cpp"
    outputs:
      - "Wait-free, zero-allocation, seqlock-correct MidiManager implementation"
    acceptance_criteria:
      - "Every write to `m_ccTable` in `midi_manager.cpp` increments `m_ccTableSeq` before and after mutation."
      - "`handleControlChange` verifies `seq1 == seq2` after copying `CcTargetList` to local stack variables."
      - "Firing `handleControlChange` after `stopLearnAndBind` (with 4-digit `paramId = 1000`) performs 0 heap allocations."
    verification_commands:
      - "g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -fsyntax-only src/midi/midi_types.h"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/midi/midi_types.h src/midi/midi_manager.h src/midi/midi_manager.cpp"
    risk_level: LOW
    stop_conditions: []

  - id: TASK-06
    title: "Enforce Single-Producer Queue Separation, Live Transport, Side-Effect-Free GUI Sizing & COM Refcounting in VST3/CLAP Hosts"
    workstream: WS-3-Plugin-Host-And-OS-Boundaries
    cluster: RCC-01
    findings_addressed: ["F-01", "F-08", "F-12", "F-16", "F-17"]
    agent_role: plugin-state-engineer
    depends_on: ["TASK-03", "TASK-10"]
    blocks: ["TASK-08", "TASK-18", "TASK-21"]
    files_owned:
      - "src/plugins/vst3_host.h"
      - "src/plugins/vst3_host.cpp"
      - "src/plugins/vst3_param_queue.h"
      - "src/plugins/clap_host.h"
      - "src/plugins/clap_host.cpp"
      - "src/plugins/plugin_window.h"
      - "src/plugins/plugin_window.cpp"
      - "src/plugins/plugin_scanner.cpp"
    files_read_only:
      - "src/plugins/crash_isolation.h"
      - "src/plugins/plugin_base.h"
      - "src/audio/audio_node.h"
    objective: >
      Fix the multi-producer SPSC queue corruption in `Vst3PluginInstance` and `ClapPluginInstance` (F-01),
      eliminate audio-thread `std::string` allocations on plugin crash latching (F-08), wire live transport
      context (`tempoBpm`, time signature, project time) into VST3 and CLAP `process()` (F-12), remove the
      `const_cast` GUI creation leak in `ClapPluginInstance::getPreferredSize` (F-16), and replace dummy COM
      refcounts, manual `new`, and silent `catch (...)` blocks (F-17).
    instructions:
      - "In `src/plugins/vst3_host.h/.cpp` and `src/plugins/clap_host.h/.cpp`, enforce strict SPSC ownership: `m_paramQueue` is written ONLY by the UI thread (`setParameterValue`, `onGuiPerformEdit`, `loadState`) and read ONLY by the Audio thread (`process`). `m_guiOutParamQueue` (CLAP) is written ONLY by the Audio thread (`onPluginOutputEvent`, `setParameterFromAudioThread`) and read ONLY by the UI thread (`flushGuiParameterChanges`). Remove `m_guiOutParamQueue.try_enqueue` from `ClapPluginInstance::setParameterValue` (`clap_host.cpp:629` — instead update `m_paramValues` directly since `setParameterValue` is already on the UI thread!)."
      - "In `Vst3PluginInstance::setParameterFromAudioThread` (`vst3_host.cpp:664-670`), write directly into `m_inputParameterChanges.addParameterData(paramId)` (or a fixed audio-thread array `std::array<ParamChangeEvent, 256> m_audioThreadParamEvents` drained at the top of `Vst3PluginInstance::process` and `ClapPluginInstance::process`) and NEVER call `m_paramQueue.try_enqueue` from the audio thread!"
      - "In `Vst3PluginInstance::process` (`vst3_host.cpp:542-546`), populate `m_processContext.tempo = (ctx.tempoBpm > 1.0) ? ctx.tempoBpm : 120.0;`, `m_processContext.timeSigNumerator = (ctx.timeSigNumerator > 0) ? ctx.timeSigNumerator : 4;`, `m_processContext.timeSigDenominator = (ctx.timeSigDenominator > 0) ? ctx.timeSigDenominator : 4;`, and `m_processContext.projectTimeSamples = ctx.projectTimeSamples;`. In `ClapPluginInstance::process` (`clap_host.cpp:553-554`), populate a stack/member `clap_event_transport_t` with `ctx.tempoBpm` and time signature and assign `processData.transport = &m_transportEvent;`."
      - "In `Vst3PluginInstance::process` (`vst3_host.cpp:638`) and `ClapPluginInstance::process` (`clap_host.cpp:614`), use `formatFaultReason(m_faultReasonBuf, \"VST3 crash: \", exCode)` (fixed `char[128]` buffer from `TASK-10`) instead of `std::string(\"VST3 crash: \") + getExceptionDescription(exCode)` on the audio thread."
      - "In `ClapPluginInstance::getPreferredSize` (`clap_host.cpp:742-763`), remove `const_cast<ClapPluginInstance*>(this)->m_guiCreated = true;`. If `!m_guiCreated`, either return the cached default/last size `(800, 600)` until `openGui` creates the view, or inside `PluginWindowManager::openPluginWindow` (`plugin_window.cpp:95-120`), resize the Win32 window immediately after `plugin->openGui(hwnd)` queries `getPreferredSize` when `m_guiCreated` is legitimately `true`."
      - "In `src/plugins/vst3_host.cpp:54-55, 110-111, 147-148`, add `std::atomic<uint32_t> m_refCount{1}` and a `detachHost()` guard to `PraccyComponentHandler` and `PraccyPlugFrame`, replace manual `new Vst3PluginInstance()` (`vst3_host.cpp:321`) and `new ClapPluginInstance()` (`clap_host.cpp:396`) with `std::make_unique`, and record diagnostic failure reasons in `plugin_scanner.cpp` instead of empty `catch (...) {}` blocks."
    inputs:
      - "TASK-03 AudioProcessContext and TASK-10 crash_isolation.h"
    outputs:
      - "Updated VST3 and CLAP host wrappers, param queue, plugin window manager, and scanner"
    acceptance_criteria:
      - "`setParameterFromAudioThread` never calls `m_paramQueue.try_enqueue` in either `vst3_host.cpp` or `clap_host.cpp`."
      - "`ClapPluginInstance::setParameterValue` never calls `m_guiOutParamQueue.try_enqueue`."
      - "Zero `const_cast` expressions remain in `src/plugins/clap_host.cpp`."
      - "Zero `std::string` allocations occur inside `Vst3PluginInstance::process` or `ClapPluginInstance::process` when a fault is caught."
    verification_commands:
      - "! grep -n 'const_cast' src/plugins/clap_host.cpp"
      - "! grep -n 'std::string(\"VST3 crash' src/plugins/vst3_host.cpp"
      - "! grep -n 'std::string(\"CLAP crash' src/plugins/clap_host.cpp"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/plugins/vst3_host.h src/plugins/vst3_host.cpp src/plugins/vst3_param_queue.h src/plugins/clap_host.h src/plugins/clap_host.cpp src/plugins/plugin_window.h src/plugins/plugin_window.cpp src/plugins/plugin_scanner.cpp"
    risk_level: HIGH
    stop_conditions:
      - "Any crash isolation test in `test_challenger_m2_2.cpp` fails."

  - id: TASK-09
    title: "Refactor QuickLooper for O(1) Buffer Swaps & Wait-Free loadWavFile, and Fix AudioPlayer UI Hazard Bypass"
    workstream: WS-2-RealTime-Audio-Core
    cluster: RCC-02
    findings_addressed: ["F-03", "F-04", "F-25"]
    agent_role: rt-audio-architect
    depends_on: ["TASK-01"]
    blocks: ["TASK-19", "TASK-21"]
    files_owned:
      - "src/tools/quick_looper.h"
      - "src/tools/quick_looper.cpp"
      - "src/tools/audio_player.h"
      - "src/tools/audio_player.cpp"
    files_read_only:
      - "src/utils/wav_reader.h"
    objective: >
      Eliminate the 23–46 MB synchronous `std::fill`/`std::memcpy` stalls on the audio thread and UI/Audio
      concurrent buffer data races in `QuickLooper` (F-03), eliminate the UI-thread Use-After-Free in `AudioPlayer`
      where `seek`, `durationSeconds`, `positionSeconds`, and `positionNormalized` dereference `m_activeBuffer`
      without hazard protection (F-04), and resolve all `-Wconversion` warnings in `quick_looper.cpp` and
      `audio_player.cpp` (F-25).
    instructions:
      - "In `src/tools/quick_looper.cpp:45-138` (`executeCommand`), remove all bulk `std::fill` and `std::memcpy` calls from the real-time audio path: on `Recording` start, set `m_loopLength = 0` and `m_playhead = 0` without zeroing 23 MB (since `Recording` writes `m_loopBufferL[head]` and `m_loopBufferR[head]` sample-by-sample up to `m_loopLength`); during `Overdubbing` inside `QuickLooper::process` (`lines 277-281`), copy the pre-overdub sample `m_undoBufferL[head] = m_loopBufferL[head]; m_undoBufferR[head] = m_loopBufferR[head];` sample-by-sample as `head` advances; and on `Undo` / `Redo`, execute $O(1)$ pointer swaps (`m_loopBufferL.swap(m_undoBufferL); m_loopBufferR.swap(m_undoBufferR);`)."
      - "In `QuickLooper::triggerAction`, `stop`, `clear`, `undo`, and `redo` (`lines 147-186`), remove the racy `if (m_isProcessingAudio.load())` branch: instead, track `std::atomic<uint64_t> m_lastProcessEpoch{0}` (or `m_audioStreamActive`) and when `process()` is not being driven on a separate thread in single-threaded unit tests, drain `m_cmdQueue` safely under a `std::atomic_flag m_stateLock` spin-try guard so UI and Audio threads can NEVER enter `executeCommand` or mutate `m_loopBufferL/R` simultaneously!"
      - "In `QuickLooper::loadWavFile` (`lines 310-365`), construct the resampled `std::vector<float> newL, newR` locally on the caller thread, and hand them to the looper under the same atomic guard / SPSC swap queue (retiring the old vectors outside the audio callback) so `m_loopBufferL.resize()` is never called on an active audio buffer."
      - "In `src/tools/audio_player.h` and `src/tools/audio_player.cpp:160-163, 203-225`, add `std::atomic<size_t> m_activeNumFrames{0};` and `std::atomic<double> m_activeSampleRate{48000.0};`. Update both atomics in `executeLoad` before exchanging `m_activeBuffer`, and update `seek()`, `durationSeconds()`, `positionSeconds()`, and `positionNormalized()` to read `m_activeNumFrames` and `m_activeSampleRate` directly without ever calling `m_activeBuffer.load()` on the UI thread!"
      - "Fix all `-Wconversion` warnings in `src/tools/quick_looper.cpp:229,326,331,334` and `src/tools/audio_player.cpp:93,103,106,206,271,273` using explicit `static_cast<double>` conversions (fixing the float24 frame precision loss for tracks > 5.8 minutes)."
    inputs:
      - "src/tools/quick_looper.h, src/tools/quick_looper.cpp, src/tools/audio_player.h, src/tools/audio_player.cpp"
    outputs:
      - "Wait-free, O(1) QuickLooper and UAF-free AudioPlayer with 0 -Wconversion warnings"
    acceptance_criteria:
      - "Zero `std::memcpy` or `std::fill` calls remain inside `QuickLooper::executeCommand`."
      - "`AudioPlayer::seek`, `durationSeconds`, `positionSeconds`, and `positionNormalized` do not call `m_activeBuffer.load()`."
      - "`g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -c src/tools/quick_looper.cpp src/tools/audio_player.cpp` compiles with 0 warnings."
    verification_commands:
      - "g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -c src/tools/quick_looper.cpp -o /tmp/ql.o && g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -c src/tools/audio_player.cpp -o /tmp/ap.o"
      - "! grep -n 'std::memcpy' src/tools/quick_looper.cpp"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/tools/quick_looper.h src/tools/quick_looper.cpp src/tools/audio_player.h src/tools/audio_player.cpp"
    risk_level: MEDIUM
    stop_conditions:
      - "Any QuickLooper state-machine test in `test_challenger_m4_2.cpp` fails."

  - id: TASK-12
    title: "Optimize InstrumentTuner YIN Loop, Metronome Phasor Synthesis & BuiltIn DSP Fast Saturators"
    workstream: WS-2-RealTime-Audio-Core
    cluster: RCC-04
    findings_addressed: ["F-15", "F-23"]
    agent_role: rt-audio-architect
    depends_on: ["TASK-03"]
    blocks: ["TASK-16", "TASK-21"]
    files_owned:
      - "src/tools/tuner.h"
      - "src/tools/tuner.cpp"
      - "src/tools/metronome.h"
      - "src/tools/metronome.cpp"
      - "src/plugins/builtin_dsp.h"
      - "src/plugins/builtin_dsp.cpp"
    files_read_only:
      - "src/audio/dsp_utils.h"
    objective: >
      Eliminate the $O(N^2)$ inner-loop modulo bottleneck in `InstrumentTuner::detectPitchYin` (`src/tools/tuner.cpp:311-320`),
      replace `TunerResult::noteName` (`std::string`) with a fixed-size zero-allocation char buffer so `currentResult()`
      never allocates on the UI render thread (F-23), replace per-sample `std::exp`/`std::sin` in `Metronome::process`
      with a recurrence phasor, and replace per-sample `std::tanh` in `OverdriveEffect` and `TubeAmpEffect` with
      `DspUtils::fastTanh` (F-15).
    instructions:
      - "In `src/tools/tuner.h:20`, change `TunerResult::noteName` from `std::string` to a zero-allocation struct/fixed buffer or provide `std::array<char, 8> noteNameBuf{'-', '\\0'}` with a `const char* c_str() const noexcept` (and `operator==`/`!=` with `const char*` and `std::string_view` so all existing UI callers `.c_str()`, `.empty()`, and test assertions `res.noteName == \"A4\"` compile seamlessly without heap allocation!)."
      - "In `src/tools/tuner.h` and `src/tools/tuner.cpp:304-322` (`detectPitchYin`), add a pre-allocated `std::vector<float> m_linearWindow` resized to `m_bufferSize` in `prepare()`. At the start of `detectPitchYin`, unwrap `m_ringBuffer` starting at `startIdx` into `m_linearWindow` in at most two `std::memcpy` spans (`m_bufferSize - startIdx` and `startIdx`), then compute the YIN difference function over contiguous pointers `const float* w0 = m_linearWindow.data(); const float* wTau = w0 + tau;` with zero `% m_bufferSize` modulo operations so the compiler auto-vectorizes the inner loop with SSE2/AVX2."
      - "In `src/tools/tuner.cpp:74` (`BiquadFilter::process`), guard against `NaN`/`Inf` poisoning (`if (!std::isfinite(out)) { m_z1 = 0.0f; m_z2 = 0.0f; return 0.0f; }`)."
      - "In `src/tools/metronome.cpp:58-61`, replace per-sample `std::exp` and `std::sin` with multiplicative decay and complex phasor rotation initialized on click trigger."
      - "In `src/plugins/builtin_dsp.cpp:70, 173-174`, use `audio::DspUtils::fastTanh`."
    inputs:
      - "TASK-03 updated dsp_utils.h"
    outputs:
      - "SIMD-vectorized tuner.cpp with zero-allocation TunerResult, optimized metronome.cpp, and fast builtin_dsp.cpp"
    acceptance_criteria:
      - "Zero `% m_bufferSize` operations remain inside the nested `tau`/`j` loop of `InstrumentTuner::detectPitchYin`."
      - "`InstrumentTuner::currentResult()` performs 0 heap allocations."
      - "`g++ -std=c++20 -Isrc -O2 -Wall -Wextra -Wpedantic -Wconversion -c src/tools/tuner.cpp src/tools/metronome.cpp` compiles with 0 warnings."
    verification_commands:
      - "g++ -std=c++20 -Isrc -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -c src/tools/tuner.cpp -o /tmp/tuner.o && g++ -std=c++20 -Isrc -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -c src/tools/metronome.cpp -o /tmp/metro.o"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/tools/tuner.h src/tools/tuner.cpp src/tools/metronome.h src/tools/metronome.cpp src/plugins/builtin_dsp.h src/plugins/builtin_dsp.cpp"
    risk_level: LOW
    stop_conditions:
      - "Pitch detection accuracy across 44.1 kHz–192 kHz in `test_challenger_m1_2.cpp` deviates by > 0.5 cents."

  - id: TASK-07
    title: "Harden AppConfig INI Serialization with Escaping Codec & Add KeyMap / LiveThumbnail Persistence"
    workstream: WS-4-State-And-DAW-Interop
    cluster: RCC-05
    findings_addressed: ["F-19", "F-20", "F-21"]
    agent_role: plugin-state-engineer
    depends_on: ["TASK-02"]
    blocks: ["TASK-16", "TASK-17"]
    files_owned:
      - "src/state/app_config.h"
      - "src/state/app_config.cpp"
    files_read_only:
      - "src/utils/parse_utils.h"
    objective: >
      Prevent delimiter corruption in `config.ini` by applying `escapeIniField` / `unescapeIniField` to all string
      lists (`cachedPlugins`, `favoritePlugins`, `recentPlugins`, `vst3Paths`, `clapPaths`) in `AppConfig::save`
      and `AppConfig::load` (F-19), and add persistent configuration fields for `enableLiveThumbnails` (F-20)
      and `keyBindings` (F-21).
    instructions:
      - "In `src/state/app_config.h`, add `bool enableLiveThumbnails{false};` and `struct KeyBindingEntry { int key{0}; int slotIndex{-1}; int branchIndex{-1}; int branchSlotIndex{-1}; uint32_t targetType{0}; uint32_t paramId{0}; }; std::vector<KeyBindingEntry> keyBindings;`."
      - "In `src/state/app_config.cpp:80-111` (`save`) and `lines 203-260` (`load`), wrap plugin names, vendors, categories, paths, and favorite/recent entries in `utils::escapeIniField` on write and `utils::unescapeIniField` on read, and serialize/deserialize `enableLiveThumbnails` and `keyBindings`."
    inputs:
      - "TASK-02 updated src/utils/parse_utils.h"
    outputs:
      - "Escaped, injection-proof src/state/app_config.h and src/state/app_config.cpp"
    acceptance_criteria:
      - "A `PluginDescriptor` whose `name` or `vendor` contains `|`, `=`, or `,` round-trips through `AppConfig::save` and `AppConfig::load` without field shifting."
    verification_commands:
      - "g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -c src/state/app_config.cpp -o /tmp/app_config.o"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/state/app_config.h src/state/app_config.cpp"
    risk_level: LOW
    stop_conditions:
      - "Corrupted INI fuzzing in `test_challenger_m2_1.cpp` fails."

  - id: TASK-08
    title: "Eliminate Live UI-Thread loadState() Races in SceneManager & Escape Preset INI Serialization"
    workstream: WS-4-State-And-DAW-Interop
    cluster: RCC-02
    findings_addressed: ["F-02", "F-19"]
    agent_role: plugin-state-engineer
    depends_on: ["TASK-02", "TASK-04", "TASK-06"]
    blocks: ["TASK-14", "TASK-20"]
    files_owned:
      - "src/state/scene_manager.h"
      - "src/state/scene_manager.cpp"
    files_read_only:
      - "src/audio/graph_engine.h"
      - "src/utils/parse_utils.h"
    objective: >
      Eliminate the critical data race in `SceneManager::applyGraphSnapshotInPlace` (`src/state/scene_manager.cpp:85-100, 410-444`)
      where `slot.automationRegister().reset()` and `inner->loadState(rawState)` are called on the UI thread while
      the audio thread is actively running `process()` (F-02), and apply `escapeIniField`/`unescapeIniField` to
      `SceneManager::saveToFile` and `loadFromFile` (F-19).
    instructions:
      - "In `src/state/scene_manager.cpp:85-164` (`applyPresetToSlot`), add a `bool allowBinaryStateReload` parameter. When `applyGraphSnapshotInPlace` is called on a live `GraphEngine` (`lines 410-444`), NEVER call `slot.automationRegister().reset()` (which races with audio-thread `drainDirty`). Furthermore, only allow `applyGraphSnapshotInPlace` when either (a) the slot is a `BuiltIn` effect (whose parameters can be updated via `slot.setParameter(id, val)`), or (b) for third-party `VST3`/`CLAP` plugins, `sp.state` matches the slot's currently loaded binary state (or `sp.state.empty()`). When a third-party `VST3`/`CLAP` slot has a modified binary chunk `sp.state`, `canApplyInPlace` must return `false` so `applyGraphSnapshot` builds the new nodes off-thread and transitions glitch-free via `graph.crossfadeToNodes(std::move(newNodes), 10.0f)`!"
      - "In `SceneManager::saveToFile` (`src/state/scene_manager.cpp:782-870`) and `loadFromFile` (`lines 872-1003`), wrap preset names, categories, slot names, plugin paths, and macro names in `utils::escapeIniField` / `utils::unescapeIniField` so `|` or `=` characters in plugin names never corrupt `presets.ini`."
    inputs:
      - "TASK-02 parse_utils.h, TASK-04 graph_engine.h, TASK-06 vst3_host/clap_host"
    outputs:
      - "Thread-safe, race-free src/state/scene_manager.h and src/state/scene_manager.cpp"
    acceptance_criteria:
      - "`applyGraphSnapshotInPlace` never calls `automationRegister().reset()` or `inner->loadState()` on an active third-party VST3/CLAP plugin."
      - "Preset names and plugin names containing `|` and `=` round-trip losslessly through `SceneManager::saveToFile` and `loadFromFile`."
    verification_commands:
      - "! grep -n 'automationRegister().reset()' src/state/scene_manager.cpp"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/state/scene_manager.h src/state/scene_manager.cpp"
    risk_level: HIGH
    stop_conditions:
      - "Any UndoManager or SceneManager test in `test_praccy_v2_1.cpp` fails."

  - id: TASK-14
    title: "Fix CLAP/BuiltIn Plugin Type Erasure & Parallel Branch Loss in ReaperAdapter and AbletonAdapter"
    workstream: WS-4-State-And-DAW-Interop
    cluster: RCC-05
    findings_addressed: ["F-19"]
    agent_role: plugin-state-engineer
    depends_on: ["TASK-02", "TASK-08"]
    blocks: ["TASK-21"]
    files_owned:
      - "src/state/daw_interop/reaper_adapter.cpp"
      - "src/state/daw_interop/ableton_adapter.cpp"
    files_read_only:
      - "src/state/daw_interop/reaper_adapter.h"
      - "src/state/daw_interop/ableton_adapter.h"
      - "src/utils/xml_parser.h"
    objective: >
      Fix `ReaperAdapter::exportFromScene` (`src/state/daw_interop/reaper_adapter.cpp:280`) and `AbletonAdapter::exportFromScene`
      (`src/state/daw_interop/ableton_adapter.cpp:281`) which unconditionally emit VST3 blocks even for `CLAP` and `BuiltIn`
      plugins, and preserve parallel branch gain/pan/mute/solo/phase attributes across round-trip import/export.
    instructions:
      - "In `src/state/daw_interop/reaper_adapter.cpp:270-310`, check `slot.type`: emit `<CLAP \"CLAP: ...\"` for `CLAP` plugins (and parse `<CLAP ` chunks in `ReaperAdapter::importToScene`), emit a documented comment/JS or VST3 tag with `PRACCY_TYPE` metadata for `BuiltIn` plugins, and restore `slot.type` accurately on import."
      - "In `src/state/daw_interop/ableton_adapter.cpp:270-320`, persist `slot.type`, `slot.path`, and parallel branch mixer properties (`gainDb`, `pan`, `muted`, `soloed`, `phaseInverted`) in `<AudioEffectBranch>` XML nodes and restore them in `AbletonAdapter::importToScene`."
    inputs:
      - "src/state/daw_interop/reaper_adapter.cpp, src/state/daw_interop/ableton_adapter.cpp"
    outputs:
      - "Format-accurate Reaper and Ableton DAW interop adapters"
    acceptance_criteria:
      - "A `ScenePreset` containing `VST3`, `CLAP`, and `BuiltIn` slots plus a parallel split block preserves all `slot.type` values and branch gain/pan settings across both Reaper `.RfxChain` and Ableton `.adg` round-trips."
    verification_commands:
      - "grep -n 'CLAP' src/state/daw_interop/reaper_adapter.cpp src/state/daw_interop/ableton_adapter.cpp"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/state/daw_interop/reaper_adapter.cpp src/state/daw_interop/ableton_adapter.cpp"
    risk_level: LOW
    stop_conditions:
      - "Any existing test in `tests/test_interop_roundtrip.cpp` or `tests/test_interop_fuzz.cpp` fails."

  - id: TASK-15
    title: "Add Semantic Shadow Token, DPI Pixel Helper, Per-Window Widget Reset Storage & High-DPI Tuner Modal Layout"
    workstream: WS-5-Dear-ImGui-UX-And-DPI
    cluster: RCC-07
    findings_addressed: ["F-22", "F-23"]
    agent_role: imgui-ux-engineer
    depends_on: ["TASK-00"]
    blocks: ["TASK-16"]
    files_owned:
      - "src/ui/design_tokens.h"
      - "src/ui/ui_helpers.h"
      - "src/ui/widgets/rotary_knob.h"
      - "src/ui/modals/tuner_settings_modal.h"
      - "src/ui/modals/tuner_settings_modal.cpp"
    files_read_only: []
    objective: >
      Add a centralized `cardShadow` token and `ui::px(float)` DPI helper in `design_tokens.h`, replace the global
      non-thread-safe `s_suppressResetId` in `ui_helpers.h` and `rotary_knob.h` with ImGui state storage, and fix
      all hardcoded pixel offsets (`SameLine(130.0f)`, `SameLine(320.0f)`) and per-frame heap allocations in
      `TunerSettingsModal` (F-22, F-23).
    instructions:
      - "In `src/ui/design_tokens.h`, add `ColorToken cardShadow{0, 0, 0, 48};` to `SurfaceTokens` across all 4 theme palettes (`ObsidianStudio`, `WarmAnalog`, `CyberNord`, `DaylightStudio`), and add `[[nodiscard]] inline float px(float basePixels) noexcept { return std::round(basePixels * dpiScale()); }`."
      - "In `src/ui/ui_helpers.h:72` and `src/ui/widgets/rotary_knob.h:72`, replace the global static `s_suppressResetId` with `ImGuiStorage* storage = ImGui::GetStateStorage();` keyed by `ImGui::GetID(\"##PraccySuppressResetId\")` so multiple ImGui contexts/windows never collide."
      - "In `src/ui/modals/tuner_settings_modal.cpp:107, 129, 192, 259, 272, 275`, scale all `ImGui::SameLine(...)`, `SetNextItemWidth(...)`, and window size coordinates by `ui::dpiScale()` (`ui::px(130.0f)`, `ui::px(320.0f)`), and cache `m_cachedStringNotes` and `m_cachedFullTuningTitle` in `TunerSettingsModal` so `render()` performs 0 `std::vector` or `std::ostringstream` allocations on steady-state frames."
    inputs:
      - "src/ui/design_tokens.h, src/ui/ui_helpers.h, src/ui/widgets/rotary_knob.h, src/ui/modals/tuner_settings_modal.*"
    outputs:
      - "Updated design_tokens.h, ui_helpers.h, rotary_knob.h, and DPI-scaled zero-alloc tuner_settings_modal.*"
    acceptance_criteria:
      - "`python3 scripts/wcag_audit.py` and `python3 scripts/check_hardcoded_colors.py` pass with 0 errors."
      - "Zero hardcoded unscaled `SameLine(130.0f)` or `SameLine(320.0f)` calls remain in `tuner_settings_modal.cpp`."
    verification_commands:
      - "python3 scripts/wcag_audit.py && python3 scripts/check_hardcoded_colors.py"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/ui/design_tokens.h src/ui/ui_helpers.h src/ui/widgets/rotary_knob.h src/ui/modals/tuner_settings_modal.h src/ui/modals/tuner_settings_modal.cpp"
    risk_level: LOW
    stop_conditions:
      - "WCAG contrast ratio oracle in `test_challenger_m3_1.cpp` fails."

  - id: TASK-16
    title: "Refactor RackView for Peak-Hold/Clip Metering, Gated Live Thumbnails, High-DPI Scaling & Zero Per-Frame Allocations"
    workstream: WS-5-Dear-ImGui-UX-And-DPI
    cluster: RCC-06
    findings_addressed: ["F-20", "F-22", "F-23", "F-24"]
    agent_role: imgui-ux-engineer
    depends_on: ["TASK-04", "TASK-07", "TASK-12", "TASK-15"]
    blocks: ["TASK-17"]
    files_owned:
      - "src/ui/rack_view.h"
      - "src/ui/rack_view.cpp"
      - "src/ui/thumbnail_manager.h"
      - "src/ui/thumbnail_manager.cpp"
      - "src/ui/modals/settings_modal.cpp"
    files_read_only:
      - "src/ui/design_tokens.h"
      - "src/state/app_config.h"
      - "src/audio/dsp_utils.h"
    objective: >
      Wire `LevelMeter` peak-hold (`holdLeft`/`holdRight`) and sticky clip (`isClipped`/`resetClip`) indicators
      into `RackView::renderMeter` (F-24), gate `ThumbnailManager` polling and texture rendering in `renderPluginCard`
      behind `AppConfig::enableLiveThumbnails` (F-20), apply `ui::dpiScale()` to all `RackView` card/wire dimensions
      (F-22), and eliminate per-frame `std::string`/`std::vector` heap allocations and the inline `ColorToken(0,0,0,48)`
      literal in `RackView` (F-23).
    instructions:
      - "In `src/ui/rack_view.cpp:63`, replace `ColorToken(0, 0, 0, 48)` with `themeTokens().surfaces.cardShadow`."
      - "In `src/ui/rack_view.h:122` and `src/ui/rack_view.cpp:3150-3251`, add an overload `void renderMeter(const char* id, float level, float holdLevel, bool isClipped, float width, float height, bool* outClicked = nullptr)` and update all callers (`lines 1661-1662, 1690-1691, 2065-2066`) to pass `meter.holdLeft()`/`meter.holdRight()` and `meter.isClipped()`, drawing a 2px peak-hold tick line and a sticky red top clip cap that calls `meter.resetClip()` when clicked."
      - "In `src/ui/rack_view.cpp:358, 2358, 2363` and `src/ui/modals/settings_modal.cpp:533`, add a checkbox in `SettingsModal::renderPluginsTab` bound to `m_config.enableLiveThumbnails`. Only call `m_thumbnailMgr->update()` and `captureWindowAsync()` when `m_config.enableLiveThumbnails` is `true`, and inside `RackView::renderPluginCard` (`line 1838`), if `m_config.enableLiveThumbnails` is `true` and `m_thumbnailMgr->getThumbnail(pluginPath)` returns a non-null `ID3D11ShaderResourceView*`, draw the live thumbnail texture with `ImGui::GetWindowDrawList()->AddImage(...)` before falling back to `DeviceFaceplateBadge::render`."
      - "In `src/ui/rack_view.cpp:1224-1231` and `renderPluginCard` (`lines 1695-2005`), scale `cardW`, `serialCardH`, `inputCardW`, `outputCardW`, `insertCardW`, `wireW`, `modH`, `envHeaderH`, and internal `SetCursorPos` coordinates by `ui::dpiScale()`."
      - "In `src/ui/rack_view.h` and `src/ui/rack_view.cpp:948, 1051, 1066, 1500-1503, 2469-2472`, replace `std::unordered_map<std::string, bool> m_branchMinimized` with `std::unordered_map<uint64_t, bool> m_branchMinimized` keyed by `(static_cast<uint64_t>(blockIdx) << 32) | static_cast<uint32_t>(branchIdx)` (zero `std::string` construction per frame!), format scene-bar button labels using stack `char buf[96]` + `std::snprintf`, and cache `m_cachedUserPresetNames` invalidated on preset save/delete."
    inputs:
      - "TASK-04 graph_engine.h, TASK-07 app_config.h, TASK-12 tuner.h, TASK-15 design_tokens.h"
    outputs:
      - "High-DPI scaled, zero-alloc RackView with full Peak-Hold/Clip metering and gated live thumbnails"
    acceptance_criteria:
      - "`python3 scripts/check_hardcoded_colors.py` passes and zero `ColorToken(` constructor calls exist in `src/ui/rack_view.cpp`."
      - "`RackView::renderMeter` renders peak-hold ticks and sticky clip indicators and resets clipping on click."
      - "`m_thumbnailMgr->getThumbnail` is called in `RackView::renderPluginCard` when `enableLiveThumbnails` is enabled."
      - "Zero `std::to_string` calls remain inside `RackView::renderSignalRack` or `RackView::renderParallelBlock`."
    verification_commands:
      - "python3 scripts/check_hardcoded_colors.py"
      - "! grep -n 'ColorToken(' src/ui/rack_view.cpp"
      - "grep -n 'getThumbnail' src/ui/rack_view.cpp"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/ui/rack_view.h src/ui/rack_view.cpp src/ui/thumbnail_manager.h src/ui/thumbnail_manager.cpp src/ui/modals/settings_modal.cpp"
    risk_level: MEDIUM
    stop_conditions:
      - "Viewport centering math in `test_challenger_m3_2.cpp` regresses."

  - id: TASK-17
    title: "Implement Functional Key Map Mode (Ctrl+K), Context-Aware 'Q' Spotlight & Single-Target .wav Drag-and-Drop"
    workstream: WS-5-Dear-ImGui-UX-And-DPI
    cluster: RCC-06
    findings_addressed: ["F-21"]
    agent_role: imgui-ux-engineer
    depends_on: ["TASK-16"]
    blocks: ["TASK-18"]
    files_owned:
      - "src/ui/rack_view.h"
      - "src/ui/rack_view.cpp"
      - "src/ui/drag_drop_router.h"
      - "src/ui/drag_drop_router.cpp"
    files_read_only:
      - "src/state/app_config.h"
      - "src/ui/modals/plugin_browser_modal.h"
    objective: >
      Complete the `Key Map Mode` (`Ctrl+K`) implementation so clicking a control in Key Map Mode awaits a computer
      key press and binds that key to toggle/nudge the control (persisted in `AppConfig::keyBindings`), make the
      focused `Q` shortcut pass the focused card/branch coordinates to `PluginBrowserModal::open`, and fix
      `DragDropRouter` so only `.wav` is classified as `AudioTrack` and dropped `.wav` files load into a single
      target instead of both `AudioPlayer` and `QuickLooper` simultaneously.
    instructions:
      - "In `src/ui/rack_view.h/.cpp`, add `m_awaitingKeyTarget` state when `m_keyMapMode` is active. When a user clicks a slot's Bypass, Mute, Solo, or parameter knob while `m_keyMapMode == true`, highlight the control and capture the next alphanumeric/function key press (`ImGuiKey_0`..`ImGuiKey_Z`, `ImGuiKey_F1`..`ImGuiKey_F12`), saving the binding into `m_config.keyBindings` and showing a confirmation toast (`\"KEY BOUND: [Key] -> [Slot/Param]\"`). During normal performance (`!m_keyMapMode && !ImGui::GetIO().WantTextInput`), dispatch pressed bound keys to toggle Bypass/Mute/Solo or step the bound parameter."
      - "In `src/ui/rack_view.cpp:481-483`, update the `Q` shortcut handler when `m_focusedSlot >= 0` to check if the focused node is a `ParallelSplitMergeBlock` and pass `(m_focusedSlot, 0)` (or the active branch index) to `m_pluginBrowserModal->open(blockIdx, branchIdx)`."
      - "In `src/ui/drag_drop_router.cpp:25-26`, restrict `DroppedFileType::AudioTrack` classification strictly to `.wav` (since `WavReader` only supports RIFF WAVE PCM/Float). In `DragDropRouter::handleDrop` (`lines 120-127`), add `bool preferLooper{false}` to `DragDropContext`: load into `ctx.looper` only when `ctx.preferLooper` is `true`, otherwise load exclusively into `ctx.player`."
    inputs:
      - "TASK-16 updated rack_view.h and rack_view.cpp"
    outputs:
      - "Functional Key Map Mode, context-aware Q shortcut, and single-target .wav DragDropRouter"
    acceptance_criteria:
      - "`m_keyMapMode` is checked by interactive rack controls to register and trigger computer key bindings."
      - "`DragDropRouter::classifyFile(\"song.mp3\")` returns `DroppedFileType::Unknown` while `classifyFile(\"loop.wav\")` returns `DroppedFileType::AudioTrack`."
      - "`DragDropRouter::handleDrop` for a `.wav` file invokes either `ctx.player` or `ctx.looper`, never both simultaneously."
    verification_commands:
      - "g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -c src/ui/drag_drop_router.cpp -o /tmp/ddr.o"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout HEAD~1 -- src/ui/rack_view.h src/ui/rack_view.cpp src/ui/drag_drop_router.h src/ui/drag_drop_router.cpp"
    risk_level: LOW
    stop_conditions:
      - "Any drag-and-drop test in `test_praccy_v2_1.cpp` or `test_challenger_m4_2.cpp` fails."

  - id: TASK-18
    title: "Wire Transport Context, Thread-Safe MIDI Dispatch & Deferred WM_DPICHANGED Font Atlas Rebuild in main.cpp"
    workstream: WS-5-Dear-ImGui-UX-And-DPI
    cluster: RCC-01
    findings_addressed: ["F-05", "F-12", "F-22"]
    agent_role: imgui-ux-engineer
    depends_on: ["TASK-04", "TASK-05", "TASK-06", "TASK-17"]
    blocks: ["TASK-20", "TASK-21"]
    files_owned:
      - "src/main.cpp"
    files_read_only:
      - "src/audio/graph_engine.h"
      - "src/midi/midi_manager.h"
      - "src/ui/design_tokens.h"
    objective: >
      Connect the architectural fixes in `src/main.cpp`: pass live `Metronome` BPM, time signature, and running
      sample position into `AudioProcessContext` inside the ASIO callback (F-12), replace the unsafe raw-pointer
      MIDI callback (`src/main.cpp:551-588`) with `g_graphEngine.dispatchParameterThreadSafe(...)` (F-05), and
      implement deferred pre-frame D3D11 font atlas rebuilding when `WM_DPICHANGED` fires (F-22).
    instructions:
      - "In `src/main.cpp:494-502` (inside the ASIO audio callback), maintain an audio-thread sample counter `s_projectTimeSamples += numSamples;` and populate `ctx.tempoBpm = g_metronome.bpm(); ctx.timeSigNumerator = g_metronome.beatsPerBar(); ctx.timeSigDenominator = 4; ctx.projectTimeSamples = s_projectTimeSamples;`."
      - "In `src/main.cpp:551-588`, replace the manual `g_graphEngine.getNode(...)` / `block->getBranch(...)` pointer traversal inside `g_midiManager.setBindingCallback` with a single call to `g_graphEngine.dispatchParameterThreadSafe(binding.targetSlotIndex, binding.targetBranchIndex, binding.targetBranchSlotIndex, binding.paramId, normalizedValue)`."
      - "In `src/main.cpp:796-808` (`WM_DPICHANGED`), store the new DPI scale into `static std::atomic<float> g_pendingDpiScale{0.0f};`. At the top of the main Win32 message/render loop (before `ImGui_ImplDX11_NewFrame()`), check if `float newDpi = g_pendingDpiScale.exchange(0.0f); newDpi > 0.0f`: if so, call `ui::setDpiScale(newDpi); ui::applyTheme(g_appConfig.settings().themeId);`, clear and rebuild `io.Fonts` (`g_fontUI` at `std::round(15.0f * newDpi)` and `g_fontMono` at `std::round(13.0f * newDpi)`), and call `ImGui_ImplDX11_InvalidateDeviceObjects(); ImGui_ImplDX11_CreateDeviceObjects();`."
    inputs:
      - "TASK-04 GraphEngine, TASK-05 MidiManager, TASK-06 VST3/CLAP hosts, TASK-17 RackView/DragDropRouter"
    outputs:
      - "Updated src/main.cpp with live transport, thread-safe MIDI parameter dispatch, and dynamic High-DPI font rebuild"
    acceptance_criteria:
      - "`src/main.cpp` never calls `g_graphEngine.getNode` or `innerNode->setParameterValue` from the MIDI callback."
      - "`ctx.tempoBpm` and `ctx.timeSigNumerator` are populated in the audio callback in `src/main.cpp`."
      - "`WM_DPICHANGED` triggers a pre-frame font atlas rebuild and `ImGui_ImplDX11_InvalidateDeviceObjects()`."
    verification_commands:
      - "grep -n 'dispatchParameterThreadSafe' src/main.cpp"
      - "grep -n 'ImGui_ImplDX11_InvalidateDeviceObjects' src/main.cpp"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- src/main.cpp"
    risk_level: MEDIUM
    stop_conditions: []

  - id: TASK-19
    title: "Expand Challenger M1.2, M2.2 & M4.2 Suites with ASIO Int32/Float32 UBSan & Multi-Threaded Looper/Crash Tests"
    workstream: WS-6-Verification-And-Gates
    cluster: RCC-08
    findings_addressed: ["F-03", "F-08", "F-10", "F-11", "F-26"]
    agent_role: verification-architect
    depends_on: ["TASK-09", "TASK-10", "TASK-11"]
    blocks: ["TASK-21"]
    files_owned:
      - "tests/test_challenger_m1_2.cpp"
      - "tests/test_challenger_m2_2.cpp"
      - "tests/test_challenger_m4_2.cpp"
    files_read_only:
      - "src/audio/asio_manager.h"
      - "src/plugins/crash_isolation.h"
      - "src/tools/quick_looper.h"
    objective: >
      Close the verification gaps in `test_challenger_m1_2.cpp`, `test_challenger_m2_2.cpp`, and `test_challenger_m4_2.cpp`
      (F-26) by testing `AsioManager::packInt32LSB` and `packFloat32LSB` at `+1.0f`, `±1.5f`, `NaN`, and `±Inf`
      under UBSan (F-10), asserting zero heap allocations when latching plugin crash descriptions via `formatFaultReason`
      (F-08), and stress-testing `QuickLooper` with concurrent UI commands (`triggerAction`, `undo`, `redo`, `clear`,
      `loadWavFile`) while an audio thread continuously calls `process()` with a 0-allocation trap (F-03).
    instructions:
      - "In `tests/test_challenger_m1_2.cpp:140-160`, add assertions for `AsioManager::packInt32LSB` and `AsioManager::packFloat32LSB` across `{-1.5f, -1.0f, 0.0f, +1.0f, +1.5f, NAN, INFINITY, -INFINITY}`, asserting that `+1.0f` packs to `+2147483647` (`INT32_MAX`, never `-2147483648`)."
      - "In `tests/test_challenger_m2_2.cpp:635-659`, replace the vulnerable `std::string(\"VST3 crash: \")` test with an assertion verifying that `plugins::formatFaultReason` executes with `0` heap allocations on the audio thread."
      - "In `tests/test_challenger_m4_2.cpp`, add a multi-threaded stress test running an audio thread calling `looper.process()` for 10,000 blocks while a concurrent UI thread triggers `triggerAction()`, `undo()`, `redo()`, `clear()`, and `loadWavFile()`, asserting `0` audio-thread heap allocations and `0` TSan/ASan faults."
    inputs:
      - "TASK-09 QuickLooper, TASK-10 crash_isolation.h, TASK-11 AsioManager"
    outputs:
      - "Hardened test_challenger_m1_2.cpp, test_challenger_m2_2.cpp, and test_challenger_m4_2.cpp"
    acceptance_criteria:
      - "`test_challenger_m1_2.cpp` explicitly asserts `packInt32LSB(1.0f) == 2147483647`."
      - "`test_challenger_m2_2.cpp` asserts `0` allocations during crash reason formatting."
      - "`test_challenger_m4_2.cpp` runs concurrent UI + Audio threads on `QuickLooper` with `0` races."
    verification_commands:
      - "grep -n 'packInt32LSB' tests/test_challenger_m1_2.cpp"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- tests/test_challenger_m1_2.cpp tests/test_challenger_m2_2.cpp tests/test_challenger_m4_2.cpp"
    risk_level: LOW
    stop_conditions: []

  - id: TASK-20
    title: "Replace Mocked Tests in test_praccy_v2_1.cpp, test_challenger_stage1_2.cpp & test_challenger_stage1_audio.cpp with Production Assertions"
    workstream: WS-6-Verification-And-Gates
    cluster: RCC-08
    findings_addressed: ["F-01", "F-02", "F-04", "F-05", "F-06", "F-07", "F-24", "F-26"]
    agent_role: verification-architect
    depends_on: ["TASK-04", "TASK-05", "TASK-08", "TASK-18"]
    blocks: ["TASK-21"]
    files_owned:
      - "tests/test_praccy_v2_1.cpp"
      - "tests/test_challenger_stage1_2.cpp"
      - "tests/test_challenger_stage1_audio.cpp"
    files_read_only:
      - "src/audio/graph_engine.h"
      - "src/midi/midi_manager.h"
      - "src/state/scene_manager.h"
      - "src/audio/dsp_utils.h"
    objective: >
      Eliminate "verification theatre" in `test_praccy_v2_1.cpp`, `test_challenger_stage1_2.cpp`, and
      `test_challenger_stage1_audio.cpp` (F-26) by removing local mock classes (`DinPpmBallistics`, stubbed
      `SceneManager` methods) and testing the real production `audio::LevelMeter`, `MidiManager::stopLearnAndBind`
      + `handleControlChange` (with >21-char IDs and concurrent seqlock writers), `ParamAutomationRegister`
      with 32-bit IDs `>= 1024`, and `GraphEngine` reclamation under >32 rapid slot deletions.
    instructions:
      - "In `tests/test_praccy_v2_1.cpp:1160-1305`, replace the local `DinPpmBallistics` mock class with tests against production `praccy::audio::LevelMeter` (`calculatePeak`, `peakLeft`, `holdLeft`, `isClipped`, `resetClip`)."
      - "In `tests/test_praccy_v2_1.cpp:1442-1463` (`test_T3_R6_MidiDispatch_ZeroHeapAlloc`), test bindings created via production `mgr.stopLearnAndBind()` with 4-digit `paramId = 2048` (generating a >21-char binding ID) and concurrent `addBinding` writer threads, asserting `t_allocationsCount == 0` and `0` torn seqlock reads."
      - "In `tests/test_challenger_stage1_2.cpp:25-31`, remove the no-op `SceneManager` stubs or verify `UndoManager` coupled with production `SceneManager::canApplyInPlace` state-hash guards."
      - "In `tests/test_challenger_stage1_audio.cpp:643-780`, extend `test_RT_ParamAutomation_Concurrent_ZeroAlloc` to include 32-bit VST3/CLAP parameter IDs (`>= 1024`) and extend `test_RT_GraphEngine_Concurrent_StructuralMutations_ZeroAlloc` to stress `ParallelSplitMergeBlock` branch additions/deletions and >64 rapid slot removals."
    inputs:
      - "TASK-04 GraphEngine, TASK-05 MidiManager, TASK-08 SceneManager, TASK-18 main.cpp"
    outputs:
      - "Production-grounded test_praccy_v2_1.cpp, test_challenger_stage1_2.cpp, and test_challenger_stage1_audio.cpp"
    acceptance_criteria:
      - "`DinPpmBallistics` in `test_praccy_v2_1.cpp` is replaced by assertions on `audio::LevelMeter`."
      - "`test_T3_R6_MidiDispatch_ZeroHeapAlloc` tests learned bindings (`stopLearnAndBind`) with `paramId >= 1000` and asserts `0` allocations."
    verification_commands:
      - "! grep -n 'class DinPpmBallistics' tests/test_praccy_v2_1.cpp"
      - "grep -n 'stopLearnAndBind' tests/test_praccy_v2_1.cpp"
    reviewer_role: independent-reviewer
    rollback_procedure: "git checkout main -- tests/test_praccy_v2_1.cpp tests/test_challenger_stage1_2.cpp tests/test_challenger_stage1_audio.cpp"
    risk_level: LOW
    stop_conditions: []

  - id: TASK-21
    title: "Execute Final End-to-End Static Analysis, Sanitizer (ASan/UBSan/TSan) & Real-Time Allocation Gate"
    workstream: WS-6-Verification-And-Gates
    cluster: RCC-08
    findings_addressed:
      - "F-01"
      - "F-02"
      - "F-03"
      - "F-04"
      - "F-05"
      - "F-06"
      - "F-07"
      - "F-08"
      - "F-09"
      - "F-10"
      - "F-11"
      - "F-12"
      - "F-13"
      - "F-14"
      - "F-15"
      - "F-16"
      - "F-17"
      - "F-18"
      - "F-19"
      - "F-20"
      - "F-21"
      - "F-22"
      - "F-23"
      - "F-24"
      - "F-25"
      - "F-26"
    agent_role: verification-architect
    depends_on: ["TASK-13", "TASK-14", "TASK-18", "TASK-19", "TASK-20"]
    blocks: []
    files_owned: []
    files_read_only:
      - "CMakeLists.txt"
      - "src/"
      - "tests/"
      - "scripts/"
    objective: >
      Run the complete verification gate across the remediated repository: strict compiler warning audit
      (`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor`), UI color/WCAG scripts,
      ASan + UBSan execution of `test_remediation_characterization --verify-fixed` and portable test suites,
      and TSan concurrency verification across `ParamAutomationRegister`, `QuickLooper`, `AudioPlayer`,
      and `MidiManager`.
    instructions:
      - "Run `python3 scripts/check_hardcoded_colors.py` and `python3 scripts/wcag_audit.py` and verify 0 errors."
      - "Compile all portable translation units with `g++ -std=c++20 -Isrc -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` and verify 0 warnings."
      - "Compile and execute `tests/test_remediation_characterization.cpp` under `-fsanitize=address,undefined` and `-fsanitize=thread`, verifying 0 sanitizer errors and 100% assertion pass."
    inputs:
      - "Merged main branch containing TASK-00 through TASK-20"
    outputs:
      - "Final verification telemetry report confirming all 26 findings are resolved"
    acceptance_criteria:
      - "0 compiler warnings under `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor`."
      - "0 ASan, UBSan, or TSan errors across all characterization and stress tests."
      - "All 26 findings in the Traceability Matrix are verified `CLOSED`."
    verification_commands:
      - "python3 scripts/check_hardcoded_colors.py && python3 scripts/wcag_audit.py"
      - "g++ -std=c++20 -Isrc -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_remediation_characterization.cpp src/tools/quick_looper.cpp src/tools/audio_player.cpp src/tools/tuner.cpp src/tools/metronome.cpp src/utils/xml_parser.cpp -o /tmp/verify_asan && /tmp/verify_asan --verify-fixed"
      - "g++ -std=c++20 -Isrc -O1 -g -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor -fsanitize=thread -fno-omit-frame-pointer tests/test_remediation_characterization.cpp src/tools/quick_looper.cpp src/tools/audio_player.cpp src/tools/tuner.cpp src/tools/metronome.cpp src/utils/xml_parser.cpp -o /tmp/verify_tsan && /tmp/verify_tsan --verify-fixed"
    reviewer_role: independent-reviewer
    rollback_procedure: "N/A (read-only verification gate)"
    risk_level: LOW
    stop_conditions:
      - "Any compiler warning, sanitizer error, or test assertion failure."

```

### 9. SAFETY AND ROLLBACK PLAN

#### Pre-Work Baseline & Characterization

1. Before any code changes, `TASK-00` commits the unmodified repository baseline to `main` and tags it `baseline-pre-remediation`.
2. `TASK-01` writes `tests/test_remediation_characterization.cpp`, capturing current behavior and deterministic reproductions of the concurrency, UBSan, and serialization bugs before Wave 1 begins.

#### Per-Task Merge Gates

Every task branch (`task/TASK-XX`) must pass four automated checks before `independent-reviewer` approval and merge:

1. **Ownership Gate:** `git diff --name-only main...task/TASK-XX` must be a strict subset of `files_owned`.
2. **Compiler Gate:** Zero warnings under `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor`.
3. **Sanitizer Gate:** Relevant unit and characterization tests pass under `-fsanitize=address,undefined` and `-fsanitize=thread`.
4. **Design System Gate (for UI tasks):** `python3 scripts/check_hardcoded_colors.py` and `python3 scripts/wcag_audit.py` exit with code `0`.

#### Rollback & Staged Rollout Strategy

* **Per-Task Rollback:** Every task merges as a single atomic commit on `main` (`git merge --no-ff task/TASK-XX`) and carries an explicit `rollback_procedure` (`git revert -m 1 <merge-sha>` or file checkout).
* **Per-Workstream Rollback:** Tag `main` at the completion of each Wave (`wave-0-green` through `wave-5-green`). If a downstream integration conflict arises in Wave $N$, the orchestrator can reset `main` cleanly to `wave-(N-1)-green`.
* **Config & Feature Gating:** Live window thumbnail scraping (`F-20`) defaults to `enableLiveThumbnails = false` in `AppConfig` so GDI+ / D3D11 screen capture is opt-in and cannot degrade baseline frame times.
* **Tactical Mitigations Required:** None. Because foundational abstractions (`TASK-02`, `TASK-03`, `TASK-10`) land in Wave 1 before their consumers in Waves 2–4, every finding is resolved directly at its architectural root cause without temporary patches.

#### Stop-the-Line Conditions (Orchestrator Halt & Human Escalation Triggers)

The orchestrator must immediately halt execution and escalate to a human engineer if any of the following occur:

1. **Unattributable Regression:** Any existing test in `tests/` fails after a task merge and cannot be resolved within 2 retry cycles by the task's agent.
2. **Ownership Boundary Violation:** A task requires modifying a file not listed in its `files_owned` manifest, which would create a write conflict with a concurrent task in the same wave.
3. **Audio Thread Allocation or Lock Detection:** Any test instrumented with the audio-thread `operator new` hook or TSan reports a non-zero allocation count or data race inside `process()`.
4. **Audio Signal Corruption:** Total Harmonic Distortion (THD) or phase-cancellation nulls in `test_challenger_stage1_audio.cpp` degrade beyond `-90 dBFS`.

### 10. RISK REGISTER

| Risk ID | Workstream | Description of What Could Go Wrong | Likelihood | Impact | Detection Mechanism | Mitigation Strategy |
| --- | --- | --- | --- | --- | --- | --- |
| **`RSK-01`** | `WS-2-RealTime-Audio-Core` | CAS overflow table in `ParamAutomationRegister` (`TASK-03`) experiences slot collisions if a plugin automates $>64$ distinct high-ID parameters (`paramId >= 1024`) within a single 1.33 ms audio block. | Low | Medium | Unit test saturating $>64$ high-ID parameters in `TASK-20`. | Use linear probing across a 128-slot power-of-two atomic array (`MAX_OVERFLOW_PARAMS = 128`), which exceeds realistic per-block human/MIDI automation density. |
| **`RSK-02`** | `WS-2-RealTime-Audio-Core` | Dual-chain crossfade evaluation in `GraphEngine::process` (`TASK-04`) doubles plugin CPU load during the 10 ms (`480`-sample) scene transition window. | Medium | Low | Audio callback timing telemetry in `test_challenger_m4_1.cpp`. | Bound crossfade duration to `min(rampMs, 10.0f)` and bypass dual-chain evaluation if the outgoing chain was silent (`peak < 1e-5f`). |
| **`RSK-03`** | `WS-3-Plugin-Host-And-OS-Boundaries` | Refactoring `crash_isolation.h` (`TASK-10`) breaks MSVC `__try`/`__except` compilation while fixing MinGW VEH `longjmp`. | Low | High | Static inspection of `#if defined(_MSC_VER)` branches and `test_challenger_m2_2.cpp`. | Keep both `_MSC_VER` (`__try`/`__except`) and MinGW VEH paths routed through the exact same `detail::sehTrampoline(void(*)(void*) noexcept, void*, DWORD*)` signature. |
| **`RSK-04`** | `WS-4-State-And-DAW-Interop` | Escaping ` | `and`=`in`AppConfig`and`SceneManager` (`TASK-07`, `TASK-08`) fails to load legacy unescaped `.ini` presets. | Low | High | Round-trip test with legacy `.ini` fixtures in `test_challenger_m2_1.cpp`. |
| **`RSK-05`** | `WS-5-Dear-ImGui-UX-And-DPI` | Calling `ImGui_ImplDX11_InvalidateDeviceObjects()` inside `WndProc` during `WM_DPICHANGED` crashes D3D11 if a frame is mid-render. | Medium | High | Inspection of `src/main.cpp` render loop placement in `TASK-18`. | Never rebuild fonts inside `WndProc`; only set `g_pendingDpiScale` in `WndProc` and execute the D3D11 font rebuild at the top of the main loop before `ImGui_ImplDX11_NewFrame()`. |

### 11. VERIFICATION AND PREVENTION PLAN

#### Per-Finding Proof & Regression Guards

* **Real-Time Concurrency & Zero-Allocation Proof (`F-01`, `F-02`, `F-03`, `F-04`, `F-05`, `F-06`, `F-07`, `F-08`):**
* Proven fixed by running `test_remediation_characterization --verify-fixed` and `test_praccy_v2_1` under **ThreadSanitizer (`-fsanitize=thread`)** and a global `operator new` audio/MIDI-thread allocation trap asserting `0` TSan races and `0` heap allocations across 100,000 audio blocks and 10,000 learned MIDI CC dispatches.


* **Undefined Behavior & Crash-Isolation Proof (`F-09`, `F-10`, `F-11`, `F-16`, `F-17`, `F-18`):**
* Proven fixed by running `test_remediation_characterization` and `test_challenger_m1_2` under **AddressSanitizer + UndefinedBehaviorSanitizer (`-fsanitize=address,undefined`)**, verifying that `AsioManager::packInt32LSB(1.0f) == 2147483647` (zero signed integer overflow), `packFloat32LSB` clamps `NaN`/`±Inf`, `UpdateChecker` destroys cleanly during active requests, and `ClapPluginInstance::getPreferredSize` contains zero `const_cast`.


* **DSP Efficiency & Transport Accuracy Proof (`F-12`, `F-13`, `F-14`, `F-15`):**
* Proven fixed by verifying `AudioProcessContext` propagates `tempoBpm` and time signature into `Vst3PluginInstance` and `ClapPluginInstance`, `PluginSlot::process` performs a single dry copy, and `InstrumentTuner::detectPitchYin` executes over contiguous `m_linearWindow` pointers with zero `% m_bufferSize` modulo operations.


* **Serialization, UI/UX & Build Hygiene Proof (`F-19`, `F-20`, `F-21`, `F-22`, `F-23`, `F-24`, `F-25`, `F-26`):**
* Proven fixed by round-tripping adversarial plugin names (`"Waves|Shell=v14,Pro\n"`) through `AppConfig` and `SceneManager`, round-tripping CLAP/BuiltIn chains through `ReaperAdapter` and `AbletonAdapter`, running `scripts/check_hardcoded_colors.py` and `scripts/wcag_audit.py`, and compiling with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor` with `0` warnings.



#### Definition of Done (Final E2E Closure Criteria)

1. All 22 tasks (`TASK-00` through `TASK-21`) are merged to `main` and approved by `independent-reviewer`.
2. Every finding `F-01` through `F-26` in the Traceability Matrix is verified `CLOSED`.
3. `TASK-21` verification commands exit with code `0` (zero compiler warnings under strict flags, zero ASan/UBSan/TSan errors, zero hardcoded color or WCAG violations).

### 12. TRACEABILITY MATRIX

| Finding ID | Root-Cause Cluster | Remediating Tasks | Architectural Fix Summary | Verification Test / Command | Status |
| --- | --- | --- | --- | --- | --- |
| **`F-01`** | `RCC-01` | `TASK-03`, `TASK-04`, `TASK-06` | 32-bit CAS overflow bus in `ParamAutomationRegister`; strict single-producer SPSC queue separation in VST3/CLAP hosts. | `TASK-20` (`test_RT_ParamAutomation_Concurrent_ZeroAlloc`) + `TASK-21` TSan gate | **PLANNED** |
| **`F-02`** | `RCC-02` | `TASK-04`, `TASK-08`, `TASK-20` | State-hash guard in `SceneManager::canApplyInPlace`; live dual-chain `m_fadingOutNodes` evaluation in `GraphEngine::process`. | `! grep 'automationRegister().reset()' src/state/scene_manager.cpp` + `TASK-20` | **PLANNED** |
| **`F-03`** | `RCC-02` | `TASK-09`, `TASK-19` | $O(1)$ `std::vector::swap` for Undo/Redo, sample-by-sample overdub backup, atomic command serialization, and wait-free `loadWavFile` handoff in `QuickLooper`. | `TASK-19` (`test_challenger_m4_2.cpp` concurrent stress) + `TASK-21` TSan gate | **PLANNED** |
| **`F-04`** | `RCC-02` | `TASK-09`, `TASK-20` | Atomic `m_activeNumFrames` and `m_activeSampleRate` cache in `AudioPlayer` eliminating UI-thread `m_activeBuffer` dereferences. | `TASK-21` (`test_remediation_characterization --verify-fixed` under TSan/ASan) | **PLANNED** |
| **`F-05`** | `RCC-01` | `TASK-04`, `TASK-18`, `TASK-20` | `GraphEngine::dispatchParameterThreadSafe` holding `m_uiMutex` internally; SPSC branch command queue in `ParallelSplitMergeBlock`. | `grep 'dispatchParameterThreadSafe' src/main.cpp` + `TASK-20` | **PLANNED** |
| **`F-06`** | `RCC-01` | `TASK-05`, `TASK-20` | Full seqlock write/read verification on `m_ccTableSeq` and zero-allocation `FlatCcBinding` callback dispatch in `MidiManager`. | `TASK-20` (`test_T3_R6_MidiDispatch_ZeroHeapAlloc` with `stopLearnAndBind`) | **PLANNED** |
| **`F-07`** | `RCC-02` | `TASK-04`, `TASK-20` | Pre-reserved `m_deferredReclaim` buffer and 1024-entry `m_reclaimQueue`; deleted nodes never re-inserted into `m_activeSlots`/`m_activeNodes`. | `! grep 'm_activeSlots.push_back' src/audio/graph_engine.cpp | grep -v AddSlot` | **PLANNED** |
| **`F-08`** | `RCC-03` | `TASK-10`, `TASK-06`, `TASK-19` | Fixed `char[128]` fault buffer formatted via `std::snprintf` (`formatFaultReason`) with zero audio-thread heap allocations. | `TASK-19` (`test_challenger_m2_2.cpp` Test 4.3 allocation check == 0) | **PLANNED** |
| **`F-09`** | `RCC-03` | `TASK-10`, `TASK-19` | Pure `noexcept` C-trampoline for `setjmp`/`longjmp` and post-unwind `_resetstkoflw()` in `crash_isolation.h`. | `TASK-19` (`test_challenger_m2_2.cpp` Test 6 nested unwind check) | **PLANNED** |
| **`F-10`** | `RCC-03` | `TASK-11`, `TASK-19` | Exact `double` scaling (`* 2147483647.0`) for `ASIOSTInt32LSB` and `std::isfinite` + `[-1.0f, 1.0f]` clamping for `ASIOSTFloat32LSB`. | `TASK-19` (`test_challenger_m1_2.cpp`) + `TASK-21` UBSan gate | **PLANNED** |
| **`F-11`** | `RCC-03` | `TASK-11`, `TASK-19` | `thread_local` RAII MMCSS guard on the ASIO callback thread and guarded `AsioManager::s_instance` ownership. | `TASK-19` (`test_challenger_m1_2.cpp` Test 2) | **PLANNED** |
| **`F-12`** | `RCC-01` | `TASK-03`, `TASK-06`, `TASK-18` | `AudioProcessContext` transport fields wired from `Metronome` in `main.cpp` to VST3 `ProcessContext` and CLAP `clap_event_transport_t`. | `grep 'tempoBpm' src/audio/audio_node.h src/plugins/vst3_host.cpp src/plugins/clap_host.cpp` | **PLANNED** |
| **`F-13`** | `RCC-04` | `TASK-03`, `TASK-04` | Unified `DspUtils::fastTanh` soft-knee saturator at `0.95` threshold with matching documentation. | `TASK-04` verification + `test_challenger_m4_1.cpp` | **PLANNED** |
| **`F-14`** | `RCC-04` | `TASK-04` | Single dry buffer copy in `PluginSlot::process` using `m_inputScaledBuffer` when `inGain != 1.0f`. | Inspection & benchmark of `PluginSlot::process` in `src/audio/graph_engine.cpp` | **PLANNED** |
| **`F-15`** | `RCC-04` | `TASK-03`, `TASK-12` | Contiguous `m_linearWindow` unwrapping in `InstrumentTuner::detectPitchYin` (zero `%` in inner loop), phasor `Metronome`, and `fastTanh`. | `TASK-12` compile & pitch accuracy verification | **PLANNED** |
| **`F-16`** | `RCC-03` | `TASK-06` | Removal of `const_cast` and premature GUI creation from `ClapPluginInstance::getPreferredSize`. | `! grep 'const_cast' src/plugins/clap_host.cpp` | **PLANNED** |
| **`F-17`** | `RCC-03` | `TASK-06` | Atomic COM reference counting with `detachHost()` in `vst3_host.cpp`, `std::make_unique`, and logged catch blocks. | `grep 'm_refCount' src/plugins/vst3_host.cpp` | **PLANNED** |
| **`F-18`** | `RCC-03` | `TASK-13` | Joinable `std::jthread` members with `std::stop_token` in `UpdateChecker` and bounded worker queue in `wav_writer.h`. | `! grep 'detach()' src/ui/update_checker.cpp src/utils/wav_writer.h` | **PLANNED** |
| **`F-19`** | `RCC-05` | `TASK-02`, `TASK-07`, `TASK-08`, `TASK-14` | `escapeIniField`/`unescapeIniField` in `AppConfig` & `SceneManager`, UTF-8 XML hex entity decoding, and CLAP/BuiltIn support in DAW adapters. | `TASK-21` (`test_remediation_characterization --verify-fixed`) | **PLANNED** |
| **`F-20`** | `RCC-06` | `TASK-07`, `TASK-16` | `AppConfig::enableLiveThumbnails` setting gating `ThumbnailManager` updates and rendering `getThumbnail()` in `RackView::renderPluginCard`. | `grep 'getThumbnail' src/ui/rack_view.cpp` | **PLANNED** |
| **`F-21`** | `RCC-06` | `TASK-07`, `TASK-17` | Complete `Key Map Mode` (`Ctrl+K`) binding capture/dispatch, context-aware `Q` shortcut, and `.wav`-only single-target drop routing. | `TASK-17` compile & `DragDropRouter::classifyFile` assertions | **PLANNED** |
| **`F-22`** | `RCC-07` | `TASK-15`, `TASK-16`, `TASK-18` | `ui::dpiScale()` applied to `RackView` and `TunerSettingsModal` dimensions; pre-frame D3D11 font atlas rebuild on `WM_DPICHANGED`. | `grep 'ImGui_ImplDX11_InvalidateDeviceObjects' src/main.cpp` | **PLANNED** |
| **`F-23`** | `RCC-07` | `TASK-12`, `TASK-15`, `TASK-16` | Fixed `char[8]` in `TunerResult::noteName`, `uint64_t` keys for `m_branchMinimized`, `cardShadow` token, and `ImGuiStorage` reset ID. | `python3 scripts/check_hardcoded_colors.py` + `! grep 'ColorToken(' src/ui/rack_view.cpp` | **PLANNED** |
| **`F-24`** | `RCC-06` | `TASK-16` | `RackView::renderMeter` updated to render `LevelMeter::holdLeft()`/`holdRight()` ticks and click-to-reset `isClipped()` LED caps. | Inspection of `RackView::renderMeter` in `src/ui/rack_view.cpp` | **PLANNED** |
| **`F-25`** | `RCC-08` | `TASK-00`, `TASK-09` | Consolidated `praccy_core` target in `CMakeLists.txt`, sanitizer options, `0` `-Wconversion` warnings, and unified `2.6.0` version strings. | `TASK-00` & `TASK-09` `-Wconversion` compile checks | **PLANNED** |
| **`F-26`** | `RCC-08` | `TASK-01`, `TASK-19`, `TASK-20`, `TASK-21` | Replacement of local toy mocks (`DinPpmBallistics`, stubbed `SceneManager`) with production-linked assertions under ASan/UBSan/TSan. | `TASK-21` full sanitizer & characterization suite execution | **PLANNED** |

### 13. ORCHESTRATOR OPERATING INSTRUCTIONS

1. **Manifest Parsing & Initialization:**
* Parse the YAML block in Section 8 (`tasks`). Validate that the DAG (`depends_on` / `blocks`) is acyclic.
* Ensure the target directory is a clean git repository on branch `main` and create tag `baseline-pre-remediation`.


2. **Wave Scheduling & Worktree Dispatch:**
* A task is `READY` when all task IDs in its `depends_on` list have status `MERGED`.
* Before spawning concurrent agents for `READY` tasks, compute the pairwise intersection of their `files_owned` lists. If any two `READY` tasks share a path in `files_owned`, serialize them in topological ID order.
* For each dispatched task `TASK-XX`, create an isolated worktree:
`git worktree add ../wt-TASK-XX -b task/TASK-XX main`
* Provide the assigned agent (`agent_role`) with the task's YAML specification, read-only access to the repository, and write access **restricted exclusively** to `files_owned`.


3. **Verification & Independent Review Protocol:**
* When an implementing agent marks `TASK-XX` complete, the orchestrator runs:
1. `git diff --name-only main...task/TASK-XX` and verifies every changed file is in `TASK-XX.files_owned`.
2. Every shell command in `TASK-XX.verification_commands`.


* If both automated checks pass, dispatch an `independent-reviewer` agent (with a fresh context window) to audit the diff against `TASK-XX.acceptance_criteria` and the cluster invariants in Section 5.
* Only after `independent-reviewer` emits `APPROVED` may the orchestrator merge `task/TASK-XX` into `main` (`git merge --no-ff task/TASK-XX -m "Merge TASK-XX"`), remove the worktree (`git worktree remove ../wt-TASK-XX`), and mark `TASK-XX` as `MERGED`.


4. **Failure Handling, Retries & Rollback:**
* If `verification_commands` or `independent-reviewer` rejects a task, return the structured stderr/review feedback to the implementing agent in the same worktree for retry.
* Allow at most **2 retries** per task (3 total attempts).
* If a task fails after 2 retries or triggers any `stop_conditions` entry, execute its `rollback_procedure`, mark downstream tasks in `blocks` as `BLOCKED`, halt dispatching new waves, and escalate to a human operator with the task ID, diff, and failure log.


5. **Progress Reporting & Completion:**
* After each wave completes, tag `main` (`wave-N-green`) and emit a structured status table listing each task's state (`PENDING`, `IN_PROGRESS`, `IN_REVIEW`, `MERGED`, `BLOCKED`, `FAILED`) and the updated status of findings `F-01` through `F-26`.
* Declare the remediation complete only when `TASK-21` transitions to `MERGED` with all verification commands returning exit code `0`.