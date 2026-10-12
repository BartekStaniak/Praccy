# E2E Test Infra: Praccy V2.4 Stage 1 Architectural Blueprint

## Test Philosophy & Methodology
Praccy V2.4 Stage 1 (Musician Velocity & Pro-Audio Workflow Engine) employs an opaque-box, requirement-driven, 4-tier testing methodology with adversarial verification and real-time audio thread safety assertion:
1. **Tier 1 — Feature Coverage**: Validates standard functional happy paths, core interfaces, and architectural contracts for all new subsystems.
2. **Tier 2 — Boundary & Corner Cases**: Exercises extreme latency differentials ($\Delta L = 16384$), quantization boundaries, buffer wraparounds, out-of-range IDs, adversarial paths, and clipping thresholds.
3. **Tier 3 — Cross-Feature Interactions**: Validates multi-subsystem concurrency (e.g., parameter automation drain under active audio streaming, undo coalescing under mouse release and idle timeouts, MIDI CC routing to automation registers, micro-crossfades during card reordering).
4. **Tier 4 — Real-World Scenarios**: Simulates long-duration practice sessions (20,000 blocks), rapid slider/macro tweaking, high-DPI scaling rendering, high-load GUI frame evaluation, and decompression bomb attacks.
5. **Real-Time Memory Safety Hooks**: Overrides global `operator new/delete` tracking `t_disallowAllocations = true` to assert strictly zero heap allocations on the audio callback thread.

---

## Acceptance Criteria & Requirement Mapping

### R1. Real-Time Audio DSP Safety, SPSC Automation & Parallel Delay Compensation
- `test_T1_R1_ParamAutomationRegister_BasicDrain`: Lock-free atomic dirty bitmask draining wait-free across 64-bit word boundaries.
- `test_T1_R1_Pdc_PhaseCoherence_ZeroSampleDelta`: Sample-accurate constructive interference and $\Delta \theta < 10^{-5}$ rad phase alignment between unequal branches.
- `test_T1_R1_Pdc_EqualPowerLatencyCrossfade`: 64-sample equal-power dynamic latency crossfade satisfying energy conservation ($\cos^2(\theta) + \sin^2(\theta) = 1.0 \pm 10^{-4}$).
- `test_T1_R1_Spsc_BlockBoundary_SlotSwap`: Single-Producer Single-Consumer queue mutating nodes strictly at block boundaries.
- `test_T2_R1_ParamAutomationRegister_BoundsAndCoalescing`: Safe boundary clamping for parameter IDs, empty drain no-op, and in-place coalescing.
- `test_T2_R1_Pdc_LatencyDifferentialExtremes`: Extreme differential testing at $\Delta L = 0$, $\Delta L = 1$, and maximum supported $\Delta L = 16,384$ samples.
- `test_T2_R1_Crossfade_SampleDiscontinuity`: Sample continuity analysis across crossfade boundaries with zero NaNs, zero Infs, and zero pops.
- `test_T3_R1_ParamAutomation_Concurrent_AudioDrain_ZeroAlloc`: Multi-threaded UI parameter setter with audio thread draining asserting strictly 0 heap allocations.
- `test_T3_R1_Pdc_With_DynamicAutomation`: Simultaneous dynamic latency changes and parameter register draining.
- `test_T4_R1_Continuous_Stream_Stress`: Long-stream continuous audio simulation (20,000 blocks) verifying zero audio dropouts, zero deadlocks, and zero memory corruption.

### R2. Configurable Retroactive Riff Capture & In-Process WAV Export
- `test_T1_R2_RetroactiveBuffer_CircularAudioStorage`: Pre-allocated circular storage supporting 1, 2, 5, 10 min durations.
- `test_T1_R2_AtomicLatch_SliceCapture`: Tear-free write-head latching and slicing in chronological order across circular buffer wrap boundaries.
- `test_T1_R2_WavWriter_24BitPcmRiffHeader`: Validates 24-bit PCM little-endian RIFF header structure (`RIFF`, `WAVE`, `fmt `, `data`, byte rate 288,000, align 6).
- `test_T2_R2_Buffer_Capacity_Durations`: Memory footprint verification across 1 min (2.88M frames), 2 min (5.76M frames), 5 min (14.4M frames), and 10 min (28.8M frames).
- `test_T2_R2_Wav24Bit_Quantization_Precision`: 24-bit PCM quantization bounds ($+1.0 \rightarrow 0x7FFFFF$, $-1.0 \rightarrow 0x800000$, noise floor $< 1/2^{23}$).
- `test_T3_R2_AudioThread_ZeroAlloc_ConcurrentPushAndCapture`: Concurrent worker thread slice extraction while audio thread writes under zero-allocation assertion.
- `test_T4_R2_LatchCopy_PerformanceBenchmark`: Slice extraction latency benchmark verifying slice copying latency is $< 0.15\text{ms}$ ($150\,\mu\text{s}$).
- `test_T4_R2_EndToEnd_WavExport_Validation`: End-to-end WAV encoding, decoding, and Signal-to-Noise Ratio (SNR) verification ($> 120\text{dB}$).

### R3. Transactional Undo/Redo & State Management
- `test_T1_R3_UndoRedo_BasicTransaction`: Full-fidelity state restoration and `canUndo()` / `canRedo()` lifecycle.
- `test_T1_R3_SceneBar_ToneSnapshotAB_Toggle`: Dual snapshot comparison toggle between Tone A and Tone B with 10ms click-free crossfade.
- `test_T2_R3_HistoryStack_Capacity64_Eviction`: Strict 64-level ring buffer capacity eviction of oldest records.
- `test_T2_R3_RedoInvalidation_OnNewSnapshot`: Redo stack branch truncation when new actions occur post-undo.
- `test_T3_R3_Coalescing_400ms_IdleThreshold`: Inactivity coalescing state machine combining rapid parameter updates (<400ms) into single undo levels.
- `test_T3_R3_MouseRelease_Commit`: Mouse release (`IsItemDeactivatedAfterEdit`) committing active sweep transactions.
- `test_T3_R3_StructuralMutation_DiscretePush`: Structural rack mutations (add/delete/reorder) bypassing coalescing.
- `test_T4_R3_LiveStudio_EditingSession`: Multi-operation editing session (sweeps, adds, reorders, A/B toggles, 15 undos, 5 redos).

### R4. Front-Panel Macros & Rack Ergonomics
- `test_T1_R4_Macro_RegexHeuristic_Discovery`: Fuzzy regex discovery mapping plugin parameters to Gain/Drive, Tone/EQ, Volume/Master, Mix/Blend.
- `test_T1_R4_Hotkey_WantTextInput_Guard`: Strict suppression of global hotkeys (`Ctrl+M`, `Ctrl+K`, `Q`, `0`, `[`, `]`) while typing into active ImGui text input fields.
- `test_T1_R4_OverviewWaveform_800Points`: Decimation of audio buffers into exactly 800 peak points normalized to $[0.0, 1.0]$.
- `test_T2_R4_Macro_Regex_EdgeCases_Adversarial`: Mixed casing, special characters, duplicate parameters, and missing categories.
- `test_T2_R4_Waveform_BoundaryCases`: Zero-length buffers, small audio buffers, and out-of-bounds seek coordinate clamping.
- `test_T2_R4_FocusedShortcuts_Boundaries`: Preset cycling bounds (`[` / `]`), bypass toggling on empty racks.
- `test_T3_R4_CardDragDrop_MicroCrossfade`: Rack card reordering physics with 64-sample equal-power micro-crossfade.
- `test_T3_R4_MacroReassignment_ContextOverride`: Right-click context menu reassignment overriding regex heuristics.
- `test_T4_R4_FullPerformance_ErgonomicsSimulation`: Complete musician interaction lifecycle (text entry, preset cycling, Map Mode pulsating violet border $\alpha(t) = 0.45 + 0.45 \sin(4t)$, waveform seek).

### R5. Visual Polish, List Virtualization & Meters
- `test_T1_R5_Spotlight_ListClipper_Virtualization`: `ImGuiListClipper` virtualization maintaining constant 144 FPS with 1,500+ plugins.
- `test_T1_R5_Footer_HighDpi_Layout_NoCollision`: Dynamic layout measurement eliminating Spotlight footer text collisions across 100%–200% DPI.
- `test_T1_R5_Spotlight_Favorites_ScoreBoost`: Favorites scope boost (+250 points) and zero-query pinned sorting.
- `test_T1_R5_Ppm_Ballistics_Math`: DIN IEC 60268-10 Type I PPM ballistics (0ms attack, $\tau = 350\text{ms}$ decay, 1.2s peak hold).
- `test_T2_R5_Clipper_BoundaryCases`: Empty list, single item, and scroll out-of-bounds.
- `test_T2_R5_Ppm_ExtremeSignals`: Peak rectification, extreme overshoots, and NaN/Inf sanitization.
- `test_T3_R5_DesignTokens_VisualHierarchy_Purity`: Design token strictness across all 4 themes (Obsidian Studio, Cyber Midnight, Nordic Slate, Vintage Console).
- `test_T4_R5_HighLoad_GuiFrame_Simulation`: High-load 1,000-frame rendering simulation with 2,500 plugins (<1ms per frame).

### R6. Shared Infrastructure Subsystems
- `test_T1_R6_Archive_DecompressionBomb_Guards`: Enforces 30MB XML, 128MB archive, 100:1 ratio, and 100 files decompression bomb guards.
- `test_T1_R6_Archive_Win32PathSanitization`: Win32 Zip Slip path sanitization (rejects `..`, absolute paths, drive letters, UNC paths, DOS device names CON/PRN/AUX/NUL/COM/LPT, trailing dots/spaces).
- `test_T1_R6_DragDropRouter_Classification`: Classifies AudioTrack, Preset, Chain, and Unknown extensions case-insensitively.
- `test_T1_R6_MidiManager_FlatTable_O1`: Constant time $O(1)$ MIDI CC lookup table ($17 \times 128$) for direct channel and Omni dispatch.
- `test_T2_R6_Archive_AdversarialPathMatrix`: Comprehensive matrix of 25+ adversarial path traversal payloads rejected.
- `test_T2_R6_DragDrop_MultiFileRouting`: Simultaneous multi-file drop bundle routing to appropriate subsystem handlers.
- `test_T2_R6_Midi_Out_Of_Bounds`: Channel $>16$ and CC $>127$ clamping with slot saturation safety.
- `test_T3_R6_MidiDispatch_ZeroHeapAlloc`: High-frequency MIDI CC dispatch (10,000 messages) asserting strictly zero heap allocations.
- `test_T4_R6_FullInfrastructure_Pipeline`: Complete infrastructure pipeline linking archive extraction, drag-drop classification, and real-time MIDI expression routing.

---

## Test Executable Targets & Invocations

### Primary Test Executables
1. **`test_praccy_v2_1.exe`** (Praccy V2.4 Stage 1 Regression Suite):
   - Location: `tests/test_praccy_v2_1.cpp`
   - Total Test Cases: **52 / 52**
   - Execution command: `build\test_praccy_v2_1.exe`
2. **`test_praccy.exe`** (Core Audio Engine Suite):
   - Location: `tests/test_praccy.cpp`
   - Total Test Cases: **28 / 28**
   - Execution command: `build\test_praccy.exe`
3. **`test_asio_driver.exe`** (Headless Mock Driver Suite):
   - Location: `tests/test_asio_driver.cpp`
   - Execution command: `build\test_asio_driver.exe`
4. **Challenger Regression Targets**:
   - `test_challenger_m1.exe` (Parallel branch concurrency & SPSC saturation)
   - `test_challenger_m1_2.exe` (Tuner pitch stability across 40-1200 Hz)
   - `test_challenger_m2.exe` (Crash isolation concurrency & parse utils)
   - `test_challenger_m2_1.exe` (Zip Slip & bomb attack matrices)
   - `test_challenger_m3_1.exe` (WCAG 2.1 contrast ratio mathematical oracle)
   - `test_challenger_m3_2.exe` (Viewport centering & Hermite spline math)
   - `test_challenger_m4_1.exe` (Spotlight command palette search scoring)
   - `test_challenger_m4_2.exe` (Looper circular geometry & 10ms crossfade energy)

### Static Analysis & Linter Scripts
- `python scripts/check_hardcoded_colors.py`: Design token purity (0 undeclared literals).
- `python scripts/wcag_audit.py`: WCAG 2.1 AA text contrast compliance.

### Complete Regression Suite Command
```powershell
ctest --test-dir build --output-on-failure
```
All targets build cleanly with zero compiler warnings and execute with 100% pass rate.
