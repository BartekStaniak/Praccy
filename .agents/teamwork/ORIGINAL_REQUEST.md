# Original User Request

## Initial Request — 2026-10-06T18:22:42Z

Execute the complete end-to-end Praccy v2.0 Architectural Blueprint (PRAC-2026-V2-SPEC), addressing real-time DSP concurrency safety, auto-updater security hardening, unified design system theming and dynamic canvas centering, crash isolation (SEH), and practice suite ergonomics.

Working directory: f:/Projects/Praccy
Integrity mode: development

Specification Reference: PRAC-2026-V2-SPEC (Praccy: V2.0 Architectural Blueprint)

## Requirements

### R1. Audio DSP Concurrency, Thread Safety & Real-Time Engine
- Eliminate the data race on `ParallelBranch::m_slots` between UI slot insertions/removals and the audio callback by implementing a lock-free Single-Producer Single-Consumer (SPSC) command queue using `third_party/readerwriterqueue`.
- Decouple `InstrumentTuner` pitch detection from the real-time ASIO audio callback by having the callback push samples into a lock-free ring buffer while a 60 Hz background thread computes YIN pitch detection.
- Add unpacking routines for 24-bit audio formats (`ASIOSTInt24LSB` and `ASIOSTInt32LSB24`) in `asio_manager.cpp`, and invoke `AvSetMmThreadCharacteristicsW` upon `AsioManager::start()`.
- Eliminate dynamic heap allocations (`s_tunerMixBuf.resize`) on the audio callback thread; pre-allocate buffers during `InstrumentTuner::prepare()`.

### R2. SecOps, Hardening & Crash Isolation
- Remove all calls to `std::system()` invoking `tar` or `powershell.exe` in `src/ui/update_checker.cpp`; implement in-process archive extraction using native Win32 Shell APIs or `miniz`.
- Wrap VST3 and CLAP `process()` and GUI calls in Win32 structured exception handling (`__try` / `__except`) blocks to isolate third-party plugin access violations and gracefully bypass faulty plugins without crashing Praccy.
- Replace throwing `std::stoul` and `std::stoi` calls in `scene_manager.cpp` and `app_config.cpp` with non-throwing `std::from_chars`.
- Implement window position and maximized state persistence via Win32 `GetWindowPlacement` and `SetWindowPlacement` in `app_config.cpp`.

### R3. Unified Design Tokens, Embedded Typography & Responsive Canvas
- Author `src/ui/design_tokens.h` with semantic token structs for surfaces, borders, text, and signal states across 4 switchable production themes: Obsidian Studio, Cyber/Midnight, Nordic Slate, and Vintage Console, following a 4px/8px grid.
- Embed Inter (UI) and JetBrains Mono (Audio Readouts) as binary resources in `resources/praccy.rc` and load via `AddFontFromMemoryTTF` in `src/main.cpp`.
- Implement dynamic viewport centering algorithm in `RackView::renderSignalRack()`, auto-centering the horizontal footprint and vertically centering the signal chain based on dynamic window dimensions.
- Implement cubic Hermite spline cable rendering with distance-adaptive control tangents and animated signal pulse dots.

### R4. Modular UI Refactoring, Navigation & Practice Suite Overhaul
- Split modal dialogs from the monolithic `src/ui/rack_view.cpp` into dedicated compilation units: `src/ui/modals/plugin_browser_modal.cpp`, `settings_modal.cpp`, and `practice_tools_modal.cpp`.
- Redesign plugin cards to 240x224px cards with parameter readout headers, thumbnail preview frames with border glows, and active/bypass pill toggles, eliminating inert drawn knobs.
- Rebuild plugin browser as a Spotlight-style keyboard-driven command palette overlay with fuzzy search filtering across plugin title, developer, and category, supporting double-click and Enter key insertion.
- Replace manual folder typing in the Search Paths child window with native Win32 `IFileOpenDialog` (`FOS_PICKFOLDERS`).
- Overhaul Quick Looper with a circular progress ring indicating states (Recording: Red, Overdubbing: Amber, Playing: Green) and support WAV file drag-and-drop via `WM_DROPFILES`.
- Implement click-free crossfade ramping (`EqualPowerRamp`, 10ms) on scene preset hotkeys (1-8) and replace layout-shifting status indicators with a centered floating HUD toast notification fading after 1.8s.

## Acceptance Criteria

### Audio DSP Concurrency & Real-Time Safety
- [ ] `GraphEngineTest.ConcurrentParallelMutation` passes with zero data races reported under ThreadSanitizer or stress testing.
- [ ] `AsioManagerTest.Format24BitUnpack` passes for both `ASIOSTInt24LSB` and `ASIOSTInt32LSB24`.
- [ ] Tuner idle DSP load drops below 5% at 256 samples / 48 kHz.
- [ ] Zero dynamic memory allocations occur within the audio callback loop.

### SecOps, Crash Resilience & Code Quality
- [ ] Static analysis string inspection confirms zero occurrences of `std::system`, `cmd.exe`, or `powershell.exe` in binary/updater code.
- [ ] Corrupted or malformed `presets.ini` files parse safely without throwing unhandled exceptions or crashing.
- [ ] Simulated access violation inside hosted plugin DLL isolates the fault and bypasses the plugin without terminating Praccy.
- [ ] Clean build under MSVC with `/W4 /WX` (warnings as errors) and zero memory management static analysis warnings.

### Layout, Theming & UI Integrity
- [ ] Zero undeclared `IM_COL32` literals in `src/ui/`, verified via `scripts/check_hardcoded_colors.py`.
- [ ] Signal chain remains centered and readable across 1080p (100% DPI), 1440p (125% DPI), and 4K (150% and 200% DPI) without clipped text or overlapping controls.
- [ ] All 4 themes (Obsidian Studio, Cyber/Midnight, Nordic Slate, Vintage Console) satisfy WCAG AA minimum 4.5:1 text contrast ratio.
- [ ] Scene preset switching completes with zero audible clicks/transients and zero visual layout jitter.

### Headless Audio Regression Suite
- [ ] Headless regression harness boots Praccy with mock ASIO driver (`test_asio_driver.cpp`), pumps 20,000 sample blocks with randomized bypass toggles, and asserts zero memory corruption, zero NaN/Inf samples, and zero deadlocks.

## Follow-up — 2026-10-06T19:10:41Z

USER REQUEST: Pause all swarm work now and hold further milestone execution for 45 minutes until instructed to resume.

## Follow-up — 2026-10-06T19:35:29Z

USER REQUEST: Resume work immediately. Quota and tokens have been restored. Please proceed with all teamwork operations: evaluate Milestone 1 review gates, resolve any pending items, and advance through Milestone 2 (SecOps & Crash Isolation), Milestone 3 (Design Tokens & Responsive Canvas), Milestone 4 (Modular UI & Practice Suite), and final E2E verification.

## Follow-up — 2026-10-06T19:56:48Z

Quota reset window has passed. Resume all teamwork multi-agent swarm operations now: check the status of Milestone 1 review gates, conclude any remaining checks, and advance to Milestone 2 (SecOps & Crash Isolation).

## Follow-up — 2026-10-07T07:02:17Z

USER REQUEST: Resume work now. Quota is fully restored. Please resume the teamwork preview swarm immediately: revive/check the orchestrator and worker_m2_fix, complete the Milestone 2 remediation and review gate pass, and proceed through Milestone 3 (Design Tokens & Responsive Canvas), Milestone 4 (Modular UI & Practice Suite Overhaul), and the final E2E verification gate.

## Follow-up — 2026-10-12T17:57:38Z

Implement Praccy V2.4 Stage 1 (Musician Velocity & Pro-Audio Workflow Engine) to deliver a high-velocity, low-latency electric guitar practice experience on Windows 10/11 x64.

Working directory: F:\Projects\Praccy
Integrity mode: development

## Requirements

### R1. Real-Time Audio DSP Safety, SPSC Automation & Parallel Delay Compensation
- Lock-free, race-free parameter automation register (`ParamAutomationRegister`) utilizing an atomic dirty bitmask and flat atomic float table, draining on the audio thread with zero duplicate allocations.
- Dynamic phase-coherent Parallel Delay Compensation (PDC) supporting up to 16,384 samples of latency differential ($\Delta L = |L_A - L_B|$) across parallel branches, crossfading dynamic latency updates (`kLatencyChanged`) over 64 samples using an equal-power ramp without audible clicks or pitch modulation.
- Real-time safety contract: strictly 0 heap allocations, 0 mutex locks, and 0 blocking file I/O in the ASIO audio callback. Slot swaps and structural updates execute at audio block boundaries via lock-free SPSC command queues with a 64-sample equal-power micro-crossfade.

### R2. Configurable Retroactive Riff Capture & In-Process WAV Export
- Non-blocking circular audio buffer pre-allocated during `prepare()` with configurable duration (1, 2, 5, or 10 minutes) stored in `AppConfig`.
- Tear-free export mechanism: atomically latch write head and copy requested slice into a scratch staging buffer in $<0.15\text{ms}$, followed by asynchronous 24-bit RIFF/WAV encoding on a background worker thread via header-only in-process writer (`src/utils/wav_writer.h`).
- UI controls: Retroactive Buffer duration selection and live RAM footprint HUD in Settings Modal; bottom dock `[★ Save Last Riff]` button and `Ctrl+Shift+R` global shortcut saving to `%USERPROFILE%\Documents\Praccy\Riffs\` with floating HUD toast.

### R3. Transactional Undo/Redo & State Management
- Full-fidelity transactional `UndoManager` with up to 64 undo/redo history levels, capturing `ScenePreset` state snapshots.
- Coalescing rules: snapshot on mouse release (`IsItemDeactivatedAfterEdit`), structural rack mutations (add/remove/reorder), scene switches, and rapid continuous parameter/MIDI CC streams using a 400ms idle inactivity threshold.
- Instant A/B Tone Snapshot comparison toggle in SceneBar allowing seamless 10ms click-free crossfade between two preset memory states.

### R4. Front-Panel Macros, Tactile Rack Physics & Performance Ergonomics
- Front-panel device macro controls (4 mini-knobs per card) populated via fuzzy regex heuristic discovery (`Gain/Drive`, `Tone/EQ`, `Volume/Master`, `Mix/Blend`) with right-click context menu reassignment.
- Horizontal drag-and-drop card reordering via ImGui payload drag physics with ghost card preview and block-boundary micro-fade node reordering.
- Global MIDI & Key Map Mode (`Ctrl+M`, `Ctrl+K`) with pulsating violet canvas border and click-to-learn; keyboard mappings must be strictly guarded by `!ImGui::GetIO().WantTextInput` so text editing never fires hotkeys.
- Preset hot-swapping via focused card shortcuts: `[` / `]` for adjacent preset cycling with 10ms crossfade, `Q` for pre-filtered Spotlight, and `0` for bypass toggle.
- Interactive 800-point overview waveform strip in Backing Track with click-to-seek and loop points.

### R5. Visual Polish, Design Token Strictness & List Virtualization
- Eliminate Spotlight footer text collisions across 100%-200% DPI; virtualize row rendering via `ImGuiListClipper` maintaining constant 144 FPS across 1,500+ plugins.
- Spotlight Favorites & Pinned Scope: Tab cycles `All` / `Favorites` / `Recents`, F key toggles star status, zero-query pinned sorting, and search score boosting.
- Clean visual hierarchy: eliminate VST3 double-badging and suffix clutter; modal window dimming scrim deepened to 0.75 alpha; active scene button restyled to dark emerald background with focus border; looper idle text contrast promoted to `#9CA3AF`; top ribbon modules standardized to FramePadding 6x4.
- Audio visualizers: 4-segment vertex-colored meters with DIN IEC 60268-10 PPM ballistics ($0\text{ms}$ attack, $\tau = 350\text{ms}$ decay, $1.2\text{s}$ peak hold) and audio-reactive Hermite cable glow.

### R6. Shared Infrastructure Subsystems
- Centralized in-process archive utility (`src/utils/archive_utils.*`) wrapping vendored `miniz` with path sanitization and decompression bomb guards (30MB XML, 128MB archive, 100:1 ratio, 100 files).
- Centralized `DragDropRouter` (`src/ui/drag_drop_router.*`) replacing inline `WM_DROPFILES` handling to classify audio tracks, presets, and chains.
- Flat $O(1)$ MIDI CC lookup table in `MidiManager` with zero runtime allocations.

## Acceptance Criteria

### Audio Engine Real-Time Integrity (Gate 1.1, 1.2, 1.3)
- [ ] 60 minutes continuous audio playback with 0 dropouts and DSP load under 8%.
- [ ] 0 heap allocations and 0 mutex locks on ASIO audio thread during macro dragging, card reordering, slot swapping, and playback.
- [ ] Parallel Delay Compensation synchronizes branch latencies with 0 sample phase delta; dynamic latency changes crossfade click-free.

### Design System & Layout Verification (Gate 1.4, 1.5)
- [ ] `scripts/check_hardcoded_colors.py` passes with 0 violations (100% token purity from `src/ui/design_tokens.h`).
- [ ] Spotlight modal displays without text collisions across 100%, 125%, 150%, and 200% DPI scaling.
- [ ] Spotlight search maintains 144 FPS across 1,500+ plugins using `ImGuiListClipper`.

### Feature & Workflow Verification
- [ ] Retroactive Riff Capture exports tear-free 24-bit RIFF WAV files to `%USERPROFILE%\Documents\Praccy\Riffs\` upon dock button click or `Ctrl+Shift+R`.
- [ ] Transactional undo/redo accurately restores previous states and coalesces continuous sweeps with >400ms idle threshold.
- [ ] Global hotkeys and Map Mode hotkeys do not fire when typing into active ImGui text input fields.
- [ ] Card reordering via drag-and-drop executes smoothly with zero audible pops or glitches.

### Automated Test Suite
- [ ] All existing 10 CTest test targets in `build` pass with 100% success rate.
- [ ] New Stage 1 test harness `tests/test_praccy_v2_1.cpp` authored and passing in CTest regression suite.
