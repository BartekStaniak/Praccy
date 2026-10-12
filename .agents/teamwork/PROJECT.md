# Project: Praccy V2.4 Stage 1 (Musician Velocity & Pro-Audio Workflow Engine)

## Architecture
Praccy is an ultra-low latency Windows ASIO VST3/CLAP host and digital practice suite.
V2.4 Stage 1 introduces a high-velocity, low-latency musician workflow across four key subsystems and shared infrastructure:
1. **Core Audio DSP & Real-Time Engine**:
   - `ParamAutomationRegister` with atomic dirty bitmask (`uint64_t`) and flat atomic float table, draining wait-free on the audio callback.
   - Dynamic phase-coherent Parallel Delay Compensation (PDC) supporting $\Delta L \le 16384$ samples across parallel branches with 64-sample equal-power dynamic latency crossfade.
   - Lock-free SPSC command and reclamation queues for `GraphEngine` replacing audio thread `m_graphMutex` locking.
   - Pre-allocated `RetroactiveRiffBuffer` (1, 2, 5, 10 min) with $<0.15\text{ms}$ write-head latch & copy, and async 24-bit PCM WAV encoding via header-only `src/utils/wav_writer.h`.
2. **Shared Infrastructure & State Subsystems**:
   - Centralized `archive_utils` wrapping `miniz` with path sanitization and decompression bomb guards (30MB XML, 128MB archive, 100:1 ratio, 100 files).
   - Centralized `DragDropRouter` for audio tracks, presets, and chains.
   - Flat $O(1)$ MIDI CC lookup table (`FlatCcSlot [17][128]`) with 0 runtime heap allocations in `MidiManager`.
   - Full-fidelity transactional `UndoManager` with 64 history levels capturing `ScenePreset` snapshots, 400ms idle inactivity coalescing, and mouse-release commits.
3. **UI Ergonomics & Tactile Rack Physics**:
   - Front-panel device macro mini-knobs (4 per card) with fuzzy regex heuristics (`Gain/Drive`, `Tone/EQ`, `Volume/Master`, `Mix/Blend`) and right-click reassignment.
   - Horizontal drag-and-drop card reordering via ImGui payload drag physics with ghost card preview and block-boundary micro-fade node reordering.
   - Global MIDI & Key Map Mode (`Ctrl+M`, `Ctrl+K`) with pulsating violet border, strictly guarded by `!io.WantTextInput`.
   - Focused card shortcuts (`[`, `]`, `Q`, `0`), A/B Tone Snapshot toggle in SceneBar (10ms crossfade), and 800-point overview waveform strip in Backing Track with click-to-seek and loop points.
4. **Visual Polish, Design Tokens & List Virtualization**:
   - Spotlight list row virtualization via `ImGuiListClipper` maintaining constant 144 FPS across 1,500+ plugins.
   - High-DPI layout math eliminating footer text collisions across 100%–200% scaling.
   - Spotlight Favorites & Pinned Scope (Tab cycle, F key star toggle, search boost).
   - Visual hierarchy polish: modal scrim 0.75 alpha, dark emerald active scene buttons, `#9CA3AF` looper idle text, 6x4 ribbon padding, DIN IEC 60268-10 PPM ballistics, and audio-reactive Hermite cable glow.

## Feature Inventory
| # | Feature | Description | Milestone | Status |
|---|---------|-------------|-----------|--------|
| 1 | ParamAutomationRegister | Lock-free atomic dirty bitmask (`uint64_t`) and atomic float table draining on audio thread with 0 allocations | M1 | DONE |
| 2 | Parallel Delay Compensation (PDC) | Dynamic phase-coherent PDC supporting $\Delta L \le 16384$ samples across branches with 0-sample phase delta on sum | M1 | DONE |
| 3 | Dynamic Latency Crossfader | 64-sample equal-power crossfade for dynamic latency changes (`kLatencyChanged`) with 0 clicks/pitch modulation | M1 | DONE |
| 4 | Lock-free SPSC GraphEngine Updates | Eliminate `m_graphMutex` on audio callback thread using SPSC command queue and reclamation queue | M1 | DONE |
| 5 | PluginSlot Latency Reporting | Override `PluginSlot::latencySamples()` forwarding inner node latency | M1 | DONE |
| 6 | Retroactive Audio Ring Buffer | Pre-allocated non-blocking circular buffer for 1, 2, 5, 10 min durations with 0 audio thread allocations | M1 | DONE |
| 7 | Staging Buffer & Tear-Free Latch | Atomically latch write head and copy requested slice into scratch staging buffer in $<0.15\text{ms}$ | M1 | DONE |
| 8 | Header-Only In-Process WAV Writer | Asynchronous 24-bit RIFF/WAV encoder (`src/utils/wav_writer.h`) on background thread saving to `%USERPROFILE%\Documents\Praccy\Riffs\` | M1 | DONE |
| 9 | Decompression Bomb & Path Guards | `src/utils/archive_utils.*` wrapping `miniz` with 30MB XML, 128MB archive, 100:1 ratio, 100 files limits and path sanitization | M2 | DONE |
| 10 | Centralized DragDropRouter | `src/ui/drag_drop_router.*` classifying audio tracks, presets, and chains replacing inline `WM_DROPFILES` | M2 | DONE |
| 11 | Flat O(1) MIDI CC Table | $17 \times 128$ flat table in `MidiManager` with 0 runtime allocations replacing $O(N)$ vector traversal | M2 | DONE |
| 12 | AppConfig Retroactive Setting | Store `retroactiveBufferMinutes` in `AppConfig` with default and serialization | M2 | DONE |
| 13 | Full-Fidelity UndoManager | Transactional `UndoManager` with 64 history levels capturing `ScenePreset` snapshots | M2 | DONE |
| 14 | Undo Coalescing Engine | 400ms idle inactivity coalescing threshold and `IsItemDeactivatedAfterEdit` mouse release commits | M2 | DONE |
| 15 | CMakeLists CTest Cleanup | Fix commented-out `test_asio_driver` definition in `CMakeLists.txt` with `-Wno-mismatched-new-delete` | M2 | DONE |
| 16 | Rotary Knob Renderer | Implementation of `renderRotaryKnob()` in `src/ui/rack_view.cpp` | M3 | DONE |
| 17 | Front-Panel Mini-Knob Macros | 4 mini-knobs per card populated via fuzzy regex heuristics with right-click context menu reassignment | M3 | DONE |
| 18 | Horizontal Drag-and-Drop Card Reordering | ImGui payload drag physics (`"RACK_CARD_PAYLOAD"`) with ghost card preview and block-boundary micro-fade node reordering | M3 | DONE |
| 19 | Global MIDI & Key Map Mode | `Ctrl+M` and `Ctrl+K` map mode with pulsating violet canvas border, strictly guarded by `!io.WantTextInput` | M3 | DONE |
| 20 | Focused Card Shortcuts | `[` / `]` for adjacent preset cycling with 10ms crossfade, `Q` for pre-filtered Spotlight, `0` for bypass | M3 | DONE |
| 21 | Backing Track Waveform Strip | Interactive 800-point overview waveform strip in Backing Track with click-to-seek and loop points | M3 | DONE |
| 22 | Retroactive Riff UI Controls | Settings modal duration & RAM HUD; dock button `[★ Save Last Riff]` & `Ctrl+Shift+R` global shortcut with HUD toast | M3 | DONE |
| 23 | A/B Tone Snapshot Comparison | SceneBar instant toggle with 10ms click-free crossfade between two preset memory states | M3 | DONE |
| 24 | Undo/Redo UI Wiring | `Ctrl+Z` / `Ctrl+Y` global hotkeys with floating HUD toasts guarded by `!io.WantTextInput` | M3 | DONE |
| 25 | Spotlight List Virtualization | `ImGuiListClipper` virtualization maintaining constant 144 FPS across 1,500+ plugins | M4 | DONE |
| 26 | High-DPI Footer Layout | Dynamic layout measurement eliminating Spotlight footer text collisions across 100%–200% DPI | M4 | DONE |
| 27 | Spotlight Favorites & Pinned Scope | Tab cycle (`All`/`Favorites`/`Recents`), F key star toggle, score boost, zero-query pinned sorting | M4 | DONE |
| 28 | Visual Hierarchy Polish | Deepen scrim to 0.75 alpha, dark emerald active scene buttons, `#9CA3AF` looper idle contrast, 6x4 ribbon padding | M4 | DONE |
| 29 | PPM Ballistics & Cable Glow | 4-segment vertex-colored DIN IEC 60268-10 PPM ballistics (0ms attack, $\tau = 350\text{ms}$, 1.2s hold) and audio-reactive Hermite glow | M4 | DONE |
| 30 | Stage 1 Test Harness | Authored `tests/test_praccy_v2_1.cpp` testing real production classes across Tiers 1-4 and registered in CTest | E2E | DONE |
| 31 | Full E2E & Hardening Gate | Pass 100% of all 11 CTest targets, verify zero color violations, zero WCAG violations, and Tier 5 adversarial pass | M5 | DONE |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| M1 | Core Audio DSP & Real-Time Engine | Features 1–8 | none | DONE |
| M2 | Shared Infrastructure & State Engine | Features 9–15 | none | DONE |
| M3 | UI Ergonomics & Tactile Rack Physics | Features 16–24 | M1, M2 | DONE |
| M4 | Visual Polish, Tokens & List Virtualization | Features 25–29 | none | DONE |
| M5 | Final Milestone: Full Regression & Hardening | Feature 31 + Tier 1-4 100% pass + Tier 5 adversarial pass | M1, M2, M3, M4, E2E | DONE |

## E2E Testing Track
- Status: **DONE**
- Test Target: `tests/test_praccy_v2_1.cpp` (52/52 tests passing in CTest regression suite).
- All 11 CTest targets pass 100% in 14.50 seconds.
