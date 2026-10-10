# Project: Praccy v2.0 Architectural Blueprint (PRAC-2026-V2-SPEC)

## Architecture
Praccy is an ultra-low latency Windows ASIO VST3/CLAP host and digital practice suite.
The v2.0 Blueprint transforms the core engine across four interconnected subsystems:
1. **Audio DSP Engine**: Real-time safe lock-free SPSC command queues (`moodycamel::ReaderWriterQueue`), decoupled background YIN pitch detection (60Hz thread), 24-bit ASIO audio unpacking (`ASIOSTInt24LSB`, `ASIOSTInt32LSB24`), and MMCSS thread priority.
2. **SecOps & Crash Isolation**: Native in-process archive extraction (`miniz`), elimination of shell commands (`std::system`, `cmd.exe`, `powershell.exe`), Win32 SEH/VEH plugin crash isolation, non-throwing C++20 `std::from_chars` parsing, and Win32 `GetWindowPlacement` persistence.
3. **Design Tokens & Responsive Canvas**: Tokenized design system (`design_tokens.h`) with 4 WCAG AA switchable themes, embedded TrueType fonts (Inter, JetBrains Mono via `RT_RCDATA`), dynamic horizontal & vertical rack viewport centering, and cubic Hermite spline cables with animated pulse dots.
4. **Modular UI & Practice Suite**: Separation of monolithic modals into `src/ui/modals/`, 240x224px cards with sliding pill toggles and monospace readouts, Spotlight command palette with Win32 `IFileOpenDialog`, circular progress ring Quick Looper with WAV drag-and-drop (`WM_DROPFILES`), and 10ms click-free preset crossfading with floating HUD toasts.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | Lock-free SPSC Queue for ParallelBranch | SPSC command queue and reclamation queue for `ParallelBranch::m_slots` using `readerwriterqueue` | M1 | R1, survey_1 [DONE] |
| 2 | Decoupled InstrumentTuner Threading | 60Hz worker thread + lock-free ring buffer for YIN pitch detection, dropping idle load <0.01% | M1 | R1, survey_1 [DONE] |
| 3 | Pre-allocated Tuner Buffers | Eliminate dynamic heap allocations (`s_tunerMixBuf.resize`) on the audio callback thread | M1 | R1, survey_1 [DONE] |
| 4 | 24-Bit ASIO Unpack & Pack | Sample unpacking and packing for `ASIOSTInt24LSB` and `ASIOSTInt32LSB24` in `asio_manager.cpp` | M1 | R1, survey_1 [DONE] |
| 5 | MMCSS Thread Priority in AsioManager::start | Invoke `AvSetMmThreadCharacteristicsW(L"Pro Audio", ...)` upon `AsioManager::start()` | M1 | R1, survey_1 [DONE] |
| 6 | Concurrency & 24-Bit Unit Tests | Add `GraphEngineTest.ConcurrentParallelMutation` and `AsioManagerTest.Format24BitUnpack` to test suite | M1 | R1, survey_1 [DONE] |
| 7 | In-Process Archive Extraction | Replace `std::system()` (tar/powershell) with in-process `miniz` ZIP extractor with Zip Slip defense | M2 | R2, survey_2 |
| 8 | Direct Updater Restart (No cmd.exe) | Eliminate `cmd.exe` and `apply_update.bat`, using `Praccy.exe --apply-update` directly | M2 | R2, survey_2 |
| 9 | Win32 SEH/VEH Plugin Crash Isolation | Wrap VST3 and CLAP `process()` and GUI calls in SEH/VEH to isolate access violations without crashing | M2 | R2, survey_2 |
| 10 | Non-Throwing Numeric String Parsing | Replace throwing `std::stoul`/`std::stof` with C++20 `std::from_chars` in `scene_manager.cpp` and `app_config.cpp` | M2 | R2, survey_2 |
| 11 | Window Placement Persistence | Persist window geometry and maximized state via Win32 `GetWindowPlacement`/`SetWindowPlacement` | M2 | R2, survey_2 |
| 12 | Build Warning Hardening (/W4 /WX) | Configure `/W4 /WX` (MSVC) / `-Wall -Wextra -Werror` (GCC), isolating third-party includes as `SYSTEM` | M2 | R2, survey_2 |
| 13 | SecOps Unit Tests | Tests for corrupted `presets.ini` and simulated plugin crash isolation in `test_praccy.cpp` | M2 | R2, survey_2 |
| 14 | Semantic Design Tokens (`design_tokens.h`) | Semantic structs for surfaces, borders, text, signal states, and cables across 4 switchable themes | M3 | R3, survey_3 |
| 15 | 4 Production Themes (WCAG AA Compliant) | Obsidian Studio, Cyber/Midnight, Nordic Slate, Vintage Console with >=4.5:1 contrast | M3 | R3, survey_3 |
| 16 | Elimination of Raw `IM_COL32` Literals | Replace 170 raw color literals in `src/ui/` with tokens; author `scripts/check_hardcoded_colors.py` | M3 | R3, survey_3 |
| 17 | Embedded Fonts in Resource Script | Embed Inter and JetBrains Mono in `resources/praccy.rc` (`RT_RCDATA`) and load via `AddFontFromMemoryTTF` | M3 | R3, survey_3 |
| 18 | Dynamic Viewport Centering | Dynamic horizontal and vertical centering of rack signal chain based on dynamic window dimensions | M3 | R3, survey_3 |
| 19 | Cubic Hermite Spline Cables & Pulse Dots | Smooth cables with distance-adaptive horizontal tangents and animated signal pulse dots | M3 | R3, survey_3 |
| 20 | Modal Translation Unit Decoupling | Extract modals into `src/ui/modals/plugin_browser_modal.cpp`, `settings_modal.cpp`, `practice_tools_modal.cpp` | M4 | R4, survey_3 |
| 21 | Redesigned 240x224px Plugin Cards | Format tag, monospace parameter readouts, thumbnail frame glows, sliding pill toggle, remove inert knobs | M4 | R4, survey_3 |
| 22 | Spotlight Command Palette Browser | Centered keyboard-focused plugin search with fuzzy filtering across title, developer, category | M4 | R4, survey_3 |
| 23 | Native Win32 Folder Picker | Replace manual text path entry with Win32 `IFileOpenDialog` (`FOS_PICKFOLDERS`) | M4 | R4, survey_3 |
| 24 | Quick Looper Circular Progress Ring | Circular ring visualizing loop progress colored by state (Recording: Red, Overdubbing: Amber, Playing: Green) | M4 | R4, survey_3 |
| 25 | WAV File Drag-and-Drop | Support WAV drag-and-drop onto main window via Win32 `WM_DROPFILES` | M4 | R4, survey_3 |
| 26 | Click-Free EqualPowerRamp Preset Switching | 10ms click-free crossfade ramping on scene preset hotkeys (1-8) | M4 | R4, survey_3 |
| 27 | Floating HUD Toast Notification | Centered floating HUD toast notification on preset switch fading after 1.8s (zero layout shift) | M4 | R4, survey_3 |
| 28 | Headless Audio Regression Suite | Mock ASIO driver harness (`test_asio_driver.cpp`) pumping 20,000 blocks with random bypass toggles | M5 | Acceptance Criteria |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Audio DSP Concurrency & Real-Time Engine | Features 1–6 (R1: SPSC queue, tuner thread, 24-bit unpack, MMCSS, unit tests) | none | DONE |
| M2 | SecOps, Crash Isolation & Hardening | Features 7–13 (R2: miniz, SEH/VEH, std::from_chars, window placement, /W4 /WX) | none | DONE |
| M3 | Unified Design Tokens & Responsive Canvas | Features 14–19 (R3: design_tokens.h, 4 themes, embedded fonts, centering, Hermite splines) | none | DONE |
| M4 | Modular UI Refactoring & Practice Suite | Features 20–27 (R4: modals decoupling, 240x224 cards, command palette, circular looper, crossfading) | M3 | DONE |
| M5 | Final Milestone: Headless Regression & Validation | Feature 28 + static analysis + color audit + full E2E validation | M1, M2, M4 | IN_PROGRESS |

## Interface Contracts
### `ParallelBranch` Concurrency (`src/audio/graph_engine.h`)
- UI Thread pushes commands:
  - `AddSlotCommand { std::unique_ptr<PluginSlot> slot }`
  - `RemoveSlotCommand { size_t index, PluginSlot* targetSlot }`
- Audio Thread in `ParallelBranch::process()`:
  - Drains commands from SPSC queue wait-free.
  - Pushes removed slots to reclamation queue for safe destruction off the audio callback thread.
  - Emergency stash absorbs saturation.
- UI thread calls `collectReclaimedSlots()` at 60 Hz in `RackView::renderSignalRack()`.

### `InstrumentTuner` Decoupling (`src/tools/tuner.h`)
- Audio callback thread invokes `tuner.pushSamples()` (wait-free, zero allocation, stereo downmix in-place).
- Dedicated 60Hz worker thread consumes samples from lock-free buffer and executes YIN pitch detection.
- Analysis window dynamically scaled based on sample rate (2048 to 8192) supporting low E at 192 kHz.
- UI thread calls `tuner.currentResult()` reading atomic seqlock snapshot wait-free.
- `tuner.prepare(double sampleRate, uint32_t maxBlockSize)` safely stops worker before reallocating scratch buffers.

### `CrashIsolation` (`src/plugins/crash_isolation.h`)
- `template<typename Func> bool safeCallPluginAudio(Func&& f)`
- `template<typename Func> bool safeCallPluginGui(Func&& f)`
- Catches hardware access violations (SEH `0xC0000005` on MSVC, VEH on MinGW GCC).
- Flags `plugin->isFaulted() = true`, safely bypassing audio and preventing crashes.

### `DesignTokens` (`src/ui/design_tokens.h`)
- Token structs: `SurfaceTokens`, `BorderTokens`, `TextTokens`, `SignalStateTokens`, `CableTokens`.
- `void applyTheme(ThemeId id)` updates global token references and ImGui style colors.
- All colors accessed via `themeTokens().surfaces.*`, `themeTokens().borders.*`, etc.

## Code Layout
- `src/audio/`: `graph_engine.h/.cpp`, `asio_manager.h/.cpp`, `dsp_utils.h`
- `src/plugins/`: `crash_isolation.h`, `vst3_host.h/.cpp`, `clap_host.h/.cpp`, `plugin_slot.h/.cpp`
- `src/tools/`: `tuner.h/.cpp`, `quick_looper.h/.cpp`, `audio_player.h/.cpp`
- `src/state/`: `scene_manager.h/.cpp`, `app_config.h/.cpp`
- `src/utils/`: `parse_utils.h`
- `src/ui/`: `design_tokens.h`, `theme.h`, `rack_view.h/.cpp`
- `src/ui/modals/`: `plugin_browser_modal.h/.cpp`, `settings_modal.h/.cpp`, `practice_tools_modal.h/.cpp`
- `third_party/`: `readerwriterqueue/`, `miniz/`
- `resources/`: `praccy.rc`, `fonts/Inter-Regular.ttf`, `fonts/JetBrainsMono-Regular.ttf`
- `tests/`: `test_praccy.cpp`, `test_asio_driver.cpp`, `test_challenger_m1.cpp`, `test_challenger_m1_2.cpp`
- `scripts/`: `check_hardcoded_colors.py`
