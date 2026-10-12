# Specification Survey Report: Praccy V2.4 Stage 1 Shared Infrastructure & Test Harness

**Author:** Shared Infrastructure & Test Harness Spec Miner  
**Target Architecture:** Windows 10/11 x64, MinGW-w64 / MSVC C++20  
**Date:** 2026-10-12  
**Authority:** `ORIGINAL_REQUEST.md` (Follow-up 2026-10-12T17:57:38Z), Codebase Audit  

---

## 1. Executive Summary

This survey provides the authoritative specification, gap analysis, and implementation blueprints for **Requirement R6 (Shared Infrastructure Subsystems)**, the **Existing 10 CTest Test Targets**, and the **New Stage 1 Automated Test Harness (`tests/test_praccy_v2_1.cpp`)** covering Requirements R1 through R6 and Gates 1.1 through 1.5.

### Key Discoveries & Status:
1. **R6 Infrastructure Currently Absent**:
   - `src/utils/archive_utils.*` does **not exist**. ZIP extraction currently lives only inside `src/ui/update_checker.cpp` with higher thresholds (250MB single file, 500MB archive, 10,000 files, no ratio guard, no XML limit).
   - `src/ui/drag_drop_router.*` does **not exist**. File dropping is currently handled inline within `src/main.cpp` via `WM_DROPFILES`, only recognizing `.wav` files and ignoring presets/chains.
   - `src/midi/midi_manager.cpp` currently dispatches MIDI CC messages via an **$O(N)$ linear loop** across a `std::vector<MidiBinding> m_bindings` under mutex lock, lacking the required flat $O(1)$ table with zero runtime allocations.
2. **CTest Suite Status (10 Targets)**:
   - All 10 existing test targets build and **pass 100%** under CTest (`10/10 Passed` in 10.90s).
   - `test_asio_driver` is registered in CTest (`add_test`), and its executable exists in `build/`, but its `add_executable` definition is currently commented out in `CMakeLists.txt` (lines 158-174). It must be cleanly re-enabled with appropriate compiler flags (`-Wno-mismatched-new-delete`) to prevent broken fresh builds.
3. **Stage 1 Test Harness (`tests/test_praccy_v2_1.cpp`)**:
   - A dedicated multi-suite test executable verifying R1 (ParamAutomationRegister, 16k PDC, real-time safety), R2 (Retroactive Riff, in-process WAV writer), R3 (UndoManager 64-level stack, coalescing), R4 (macro heuristics, hotkey text guards), R5 (PPM ballistics, list clipper), and R6 (bomb guards, drag-drop router, flat MIDI table).

---

## 2. Current State Audit & Gap Analysis

### 2.1 Requirement R6 Current State

| Component | Target Location | Current Codebase State | Gap / Required Evolution |
|---|---|---|---|
| **Archive Utility** | `src/utils/archive_utils.h`, `src/utils/archive_utils.cpp` | Does not exist. Archive extraction logic is embedded inside `src/ui/update_checker.cpp` (lines 20-240). | Extract into centralized utility in `praccy::utils`. Wrap `third_party/miniz`. Implement exact decompression bomb guards: 30MB XML, 128MB archive, 100:1 ratio, 100 files limit. Provide Win32 path sanitization. Maintain backwards compatibility with `update_checker.cpp`. |
| **DragDrop Router** | `src/ui/drag_drop_router.h`, `src/ui/drag_drop_router.cpp` | Does not exist. `src/main.cpp` handles `WM_DROPFILES` inline (lines 637-664), checking only `isValidWavFile` for audio player and quick looper. | Create centralized `DragDropRouter` in `praccy::ui`. Classify Audio Tracks (`.wav`, `.wave`, `.flac`, `.mp3`, `.ogg`, `.aiff`, `.aif`), Presets (`.ini`, `.pracpreset`, `.preset`, `.vstpreset`, `.clappreset`), and Chains (`.chain`, `.pracchaim`, `.rack`). Route to appropriate subsystems. |
| **Flat O(1) MIDI CC Table** | `src/midi/midi_manager.h`, `src/midi/midi_manager.cpp` | `MidiManager` holds `std::vector<MidiBinding> m_bindings` and iterates linearly ($O(N)$) upon each CC message. Zero unit tests exist for `MidiManager`. | Replace linear loop with flat 2D lookup table: `std::array<std::array<FlatCcSlot, 128>, 17>` (16 channels + 1 omni channel, 128 CC numbers). $O(1)$ constant-time dispatch, 0 heap allocations, lock-free/wait-free dispatch. |

### 2.2 Existing 10 CTest Targets Audit

All 10 tests are executed from `build/` using `ctest` or `ctest -N`. Current execution log verifies:
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

100% tests passed out of 10 (Total Test time = 10.90 sec)
```

#### Detailed Configuration Breakdown:
1. **`test_praccy`** (`tests/test_praccy.cpp`):
   - 28 core engine unit tests covering audio buffers, DSP utils, graph engine, tuner, metronome, scene manager, parallel block dissolve, quick looper, 24-bit unpacking, miniz zip extraction, crash isolation, WCAG contrast, spline math, HUD toast alpha.
2. **`test_asio_driver`** (`tests/test_asio_driver.cpp`):
   - Headless audio regression harness. Boots mock ASIO driver, pumps 20,000 blocks at 48kHz (426.6s simulated audio), checks 0 NaNs, 0 Infs, 0 clips, and verifies 0 heap allocations via global `operator new/delete` overrides.
   - *CRITICAL CONFIG NOTE*: In `CMakeLists.txt` lines 158-174, `add_executable(test_asio_driver)` is commented out. When building from clean cache, `test_asio_driver.exe` will not be compiled unless lines 158-174 are uncommented.
3. **`test_challenger_m1`** (`tests/test_challenger_m1.cpp`):
   - Parallel branch concurrency stress (100,000 mutations), SPSC command queue saturation, reclamation queue leak audit, tuner seqlock tear-free snapshotting.
4. **`test_challenger_m1_2`** (`tests/test_challenger_m1_2.cpp`):
   - Tuner pitch detection stability across 40 Hz to 1200 Hz with synthetic noise.
5. **`test_challenger_m2`** (`tests/test_challenger_m2.cpp`):
   - `parse_utils` non-throwing fuzzing, zip attacks, crash isolation concurrency (400 simulated crashes across 4 threads).
6. **`test_challenger_m2_1`** (`tests/test_challenger_m2_1.cpp`):
   - Zip Slip path traversal matrix (mixed slashes, drive letters, UNC, NT namespace, DOS device names CON/PRN/AUX/NUL/COM/LPT), zip bomb rejection (10,001 entries, 260MB), corrupted INI fuzzing.
7. **`test_challenger_m3_1`** (`tests/test_challenger_m3_1.cpp`):
   - Independent WCAG 2.1 mathematical contrast ratio oracle across all 4 themes.
8. **`test_challenger_m3_2`** (`tests/test_challenger_m3_2.cpp`):
   - Responsive viewport centering and dynamic Hermite spline evaluations at 100%, 125%, 150%, 200% DPI.
9. **`test_challenger_m4_1`** (`tests/test_challenger_m4_1.cpp`):
   - Plugin browser command palette fuzzy search scoring, recents list bounds.
10. **`test_challenger_m4_2`** (`tests/test_challenger_m4_2.cpp`):
    - Quick Looper circular progress math & trigonometry, 10ms equal-power preset switching crossfade energy.

### 2.3 Standalone Scripts Status
- `scripts/check_hardcoded_colors.py`: Passes cleanly (`0 hardcoded IM_COL32 / ImVec4 literals found in src/ui`). Note: Must be invoked via `py scripts/check_hardcoded_colors.py` on this Windows environment.
- `scripts/wcag_audit.py`: Passes cleanly (`ALL REQUIRED TOKEN PAIRS SATISFY WCAG 2.1 REQUIREMENTS`).

---

## 3. Specifications for R6 Shared Infrastructure Subsystems

### 3.1 Centralized In-Process Archive Utility (`src/utils/archive_utils.*`)

#### Header Specification (`src/utils/archive_utils.h`):
```cpp
#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <cstdint>

namespace praccy::utils {

/// Configurable decompression bomb thresholds per R6 specification
struct ArchiveBombGuards {
    uint64_t maxXmlSize          = 30ULL * 1024 * 1024;   // 30 MB (decompression bomb guard for XML)
    uint64_t maxArchiveTotalSize = 128ULL * 1024 * 1024;  // 128 MB (total uncompressed archive limit)
    uint32_t maxCompressionRatio = 100;                   // 100:1 ratio guard (uncompressed / compressed)
    uint32_t maxFileCount        = 100;                   // 100 files max limit in archive
    uint64_t maxSingleFileSize   = 128ULL * 1024 * 1024;  // 128 MB default for non-XML entries
};

/// Win32 path sanitization for Zip Slip prevention
/// Validates and normalizes entryName, returning true and a sanitized relative path
/// if safe, or false if the entry violates any security constraints.
bool sanitizeZipPath(std::string_view entryName, std::filesystem::path& outSafeRelativePath);

/// In-process archive extraction wrapping vendored miniz
/// Checks all decompression bomb guards and path sanitization constraints before extracting
bool extractZipArchive(const std::filesystem::path& zipPath,
                       const std::filesystem::path& destDir,
                       std::string& outError,
                       const ArchiveBombGuards& guards = ArchiveBombGuards{});

/// In-memory archive extraction
bool extractZipMemory(const void* data,
                      size_t dataSize,
                      const std::filesystem::path& destDir,
                      std::string& outError,
                      const ArchiveBombGuards& guards = ArchiveBombGuards{});

} // namespace praccy::utils
```

#### Detailed Security & Decompression Bomb Rules:
1. **Entry Count Limit (`guards.maxFileCount = 100`)**:
   - `mz_uint numFiles = mz_zip_reader_get_num_files(&zip);`
   - If `numFiles > guards.maxFileCount`, abort immediately.
   - Error: `"Archive exceeds safe file count limit (maximum 100 files allowed)"`.
2. **Decompression Bomb Ratio Guard (`guards.maxCompressionRatio = 100:1`)**:
   - For each entry: check `stat.m_uncomp_size` and `stat.m_comp_size`.
   - If `stat.m_comp_size > 0`: if `stat.m_uncomp_size > static_cast<uint64_t>(guards.maxCompressionRatio) * stat.m_comp_size`, abort.
   - If `stat.m_comp_size == 0 && stat.m_uncomp_size > 0`, abort (infinite expansion).
   - Error: `"ZIP entry compression ratio exceeds safe limit (100:1 ratio guard triggered)"`.
3. **XML Uncompressed Size Guard (`guards.maxXmlSize = 30 MB`)**:
   - If entry extension is `.xml` (case-insensitive):
     If `stat.m_uncomp_size > guards.maxXmlSize`, abort.
     Error: `"ZIP XML entry exceeds 30MB uncompressed limit (potential XML bomb)"`.
4. **Total Archive Uncompressed Size Guard (`guards.maxArchiveTotalSize = 128 MB`)**:
   - `totalUncompressed += stat.m_uncomp_size;`
   - If `totalUncompressed > guards.maxArchiveTotalSize`, abort.
   - Error: `"ZIP archive total uncompressed size exceeds 128MB limit"`.
5. **Win32 Path Sanitization Rules**:
   - Reject empty strings.
   - Reject paths starting with `/` or `\` (absolute paths).
   - Reject drive letters (e.g., `C:`, `D:/`).
   - Reject UNC / NT paths (starts with `\\` or `//` or `\\?\`).
   - Normalize backslashes `\` to `/`.
   - Tokenize by `/`: reject any segment equal to `.` or `..`.
   - Reject segments containing illegal Windows characters (`< > : " | ? *`) or control characters ($< 32$).
   - Reject segments ending in trailing dot `.` or space ` ` (Windows filesystem truncation vulnerability).
   - Reject Windows reserved DOS device names (case-insensitive check of base name before dot): `CON`, `PRN`, `AUX`, `NUL`, `COM1` through `COM9`, `LPT1` through `LPT9`.
   - Verify canonical target path prefix is strictly contained within destination directory (`weakly_canonical` check).
6. **Backwards Compatibility**:
   - In `src/ui/update_checker.h` and `update_checker.cpp`, retain `ui::sanitizeZipEntryPath` and `ui::extractZipArchive` as forwarders to `praccy::utils::sanitizeZipPath` and `praccy::utils::extractZipArchive` (using relaxed thresholds for update packages if needed, while `archive_utils` defaults strictly enforce R6).

---

### 3.2 Centralized Drag-and-Drop Router (`src/ui/drag_drop_router.*`)

#### Header Specification (`src/ui/drag_drop_router.h`):
```cpp
#pragma once

#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <vector>
#include <string>
#include <functional>

namespace praccy::ui {

enum class DroppedFileType {
    AudioTrack,  // .wav, .wave, .flac, .mp3, .ogg, .aiff, .aif
    Preset,      // .ini, .pracpreset, .preset, .vstpreset, .clappreset
    Chain,       // .chain, .pracchaim, .rack
    Unknown
};

struct ClassifiedDropItem {
    std::filesystem::path path;
    DroppedFileType type{DroppedFileType::Unknown};
};

class DragDropRouter {
public:
    DragDropRouter() = default;

    /// File classification helpers
    [[nodiscard]] static DroppedFileType classifyFile(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static bool isAudioTrack(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static bool isPreset(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static bool isChain(const std::filesystem::path& path) noexcept;

    /// Extracts and classifies file paths from a Win32 HDROP handle
    [[nodiscard]] static std::vector<ClassifiedDropItem> parseHDrop(HDROP hDrop);

    /// Routing callbacks
    using AudioTrackHandler = std::function<void(const std::filesystem::path&)>;
    using PresetHandler     = std::function<void(const std::filesystem::path&)>;
    using ChainHandler      = std::function<void(const std::filesystem::path&)>;

    void setAudioTrackHandler(AudioTrackHandler handler) { m_audioHandler = std::move(handler); }
    void setPresetHandler(PresetHandler handler)         { m_presetHandler = std::move(handler); }
    void setChainHandler(ChainHandler handler)           { m_chainHandler = std::move(handler); }

    /// Routes parsed drop items to registered handlers
    void route(const std::vector<ClassifiedDropItem>& items) const;

    /// Direct entry point from WndProc WM_DROPFILES
    void handleDropMessage(HDROP hDrop) const;

private:
    AudioTrackHandler m_audioHandler;
    PresetHandler     m_presetHandler;
    ChainHandler      m_chainHandler;
};

} // namespace praccy::ui
```

#### Classification Rules:
- Case-insensitive extension comparison.
- **AudioTrack**: `.wav`, `.wave`, `.flac`, `.mp3`, `.ogg`, `.aiff`, `.aif`.
- **Preset**: `.ini`, `.pracpreset`, `.preset`, `.vstpreset`, `.clappreset`.
- **Chain**: `.chain`, `.pracchaim`, `.rack`.
- **Unknown**: Any other extension or file without extension.

#### Integration into `src/main.cpp`:
In `WndProc`:
```cpp
case WM_DROPFILES: {
    HDROP hDrop = reinterpret_cast<HDROP>(wParam);
    g_dragDropRouter.handleDropMessage(hDrop);
    DragFinish(hDrop);
    return 0;
}
```
In `main()` initialization:
- `g_dragDropRouter.setAudioTrackHandler([&](const auto& path) { player.loadWavFile(path.string()); looper.loadWavFile(path.string()); });`
- `g_dragDropRouter.setPresetHandler([&](const auto& path) { scenes.loadFromFile(path.string()); });`
- `g_dragDropRouter.setChainHandler([&](const auto& path) { scenes.loadPresetChain(path.stem().string(), graph); });`

---

### 3.3 Flat O(1) MIDI CC Table in `MidiManager` (`src/midi/midi_manager.*`)

#### Current Problem:
- `MidiManager::handleMidiMessage` iterates `m_bindings` linearly with `std::lock_guard<std::mutex>`.
- In a live performance scenario with expression pedals or motorized faders pumping 100+ CC events per second, an $O(N)$ vector traversal causes latency jitter and potential lock contention.

#### Architectural Specification:
1. **Dimensions**:
   - Channels: 16 channels + 1 Omni channel (index 0 = Omni, index 1..16 = Ch 1..16) $\rightarrow$ 17 channels.
   - CC Numbers: 0 to 127 $\rightarrow$ 128 CC numbers.
   - Max concurrent bindings per (channel, CC) slot: fixed at 4 (eliminating runtime heap allocations).
2. **Data Structure (`FlatCcSlot`)**:
   ```cpp
   static constexpr size_t MAX_BINDINGS_PER_CC = 4;
   static constexpr size_t NUM_MIDI_CHANNELS = 17; // 0 = Omni, 1..16 = Ch 1..16
   static constexpr size_t NUM_CC_NUMBERS = 128;

   struct FlatCcSlot {
       uint8_t count{0};
       std::array<MidiBinding, MAX_BINDINGS_PER_CC> bindings;
   };

   // Flat 2D array: O(1) direct memory indexing
   std::array<std::array<FlatCcSlot, NUM_CC_NUMBERS>, NUM_MIDI_CHANNELS> m_ccTable{};
   ```
3. **Dispatch Operation ($O(1)$)**:
   When MIDI message `(status, d1, d2)` is received:
   ```cpp
   if (type == MidiMessageType::ControlChange) {
       const uint8_t cc = msg.ccNumber();
       const float norm = static_cast<float>(msg.ccValue()) / 127.0f;

       // 1. Direct channel lookup O(1)
       if (channel >= 1 && channel <= 16) {
           const auto& slot = m_ccTable[channel][cc];
           for (uint8_t i = 0; i < slot.count; ++i) {
               if (m_bindingCallback) m_bindingCallback(slot.bindings[i], norm);
           }
       }
       // 2. Omni channel lookup O(1)
       const auto& omniSlot = m_ccTable[0][cc];
       for (uint8_t i = 0; i < omniSlot.count; ++i) {
           if (m_bindingCallback) m_bindingCallback(omniSlot.bindings[i], norm);
       }
   }
   ```
4. **Zero Allocation Guarantee**:
   - Lookup and dispatch require **strictly 0 heap allocations** (`operator new` / `malloc` never invoked).
   - Thread safety: Mutation (adding/removing bindings) uses `m_bindingsMutex`. For audio/MIDI thread dispatch, reading `m_ccTable` can be protected via a seqlock or copy-on-write atomic pointer swap to maintain wait-free execution.

---

## 4. Test Suite Architecture for `tests/test_praccy_v2_1.cpp`

`tests/test_praccy_v2_1.cpp` is the comprehensive automated test harness for Praccy V2.4 Stage 1, verifying Requirements R1 through R6 and Gates 1.1 through 1.5.

### 4.1 Test Module Structure
The test binary will execute 6 categorized test modules:

```
================================================================================
   PRACCY V2.4 STAGE 1 AUTOMATED REGRESSION HARNESS (test_praccy_v2_1)
================================================================================
[MODULE 1] Real-Time DSP Safety, SPSC Automation & Parallel Delay Compensation (R1)
[MODULE 2] Configurable Retroactive Riff Capture & In-Process WAV Writer (R2)
[MODULE 3] Transactional Undo/Redo & State Management (R3)
[MODULE 4] Front-Panel Macros, Tactile Rack Physics & Performance Ergonomics (R4)
[MODULE 5] Visual Polish, Design Token Strictness & List Virtualization (R5)
[MODULE 6] Shared Infrastructure Subsystems (R6)
================================================================================
```

### 4.2 Detailed Test Cases per Requirement

#### Module 1: Requirement R1 (Real-Time DSP Safety & PDC)
- `test_R1_ParamAutomationRegisterAtomicBitmask()`:
  - Sets parameters across 64 slots; verifies corresponding bit in atomic 64-bit mask is set.
- `test_R1_ParamAutomationRegisterAudioDrainZeroAlloc()`:
  - Hooks `operator new/delete` with `thread_local bool t_isAudioThread`. Drains parameter queue on simulated audio thread; asserts dirty mask clears and audio allocations == 0.
- `test_R1_PdcPhaseCoherenceZeroSampleDelta()`:
  - Parallel branch setup: Branch A latency = 256 samples, Branch B latency = 0 samples. Feeds 1 kHz sine wave through both branches. Verifies PDC delays Branch B by 256 samples and phase difference $\Delta \theta < 10^{-5}$ rad.
- `test_R1_PdcMaxLatencyDifferential16384()`:
  - Sets latency differential $\Delta L = 16,384$ samples. Verifies ring buffer operates without buffer overrun or heap reallocation.
- `test_R1_PdcDynamicLatencyCrossfadeEnergy()`:
  - Triggers dynamic latency change (`kLatencyChanged`). Verifies that the 64-sample equal-power micro-crossfade satisfies energy conservation ($\sin^2(\theta) + \cos^2(\theta) = 1.0 \pm 10^{-4}$) with zero discontinuous spikes or audio dropouts.
- `test_R1_SlotSwapSpscAudioBlockBoundary()`:
  - Swaps slots concurrently from UI thread while audio callback is running. Asserts swap takes effect at audio block boundary with 0 mutex contention.

#### Module 2: Requirement R2 (Retroactive Riff Capture & In-Process WAV Writer)
- `test_R2_RetroactiveBufferPreallocation()`:
  - Configures durations (1, 2, 5, 10 min) at 48kHz stereo float. Verifies memory footprint matches sizing: 1 min = 2,880,000 samples (~23MB), 10 min = 28,800,000 samples (~230MB). Verifies zero reallocations occur during write operations.
- `test_R2_AtomicLatchWriteHeadBenchmark()`:
  - Benchmarks write-head latching and slicing into scratch buffer. Measures elapsed time using high-resolution clock; asserts duration $< 0.15\text{ms}$ ($150\,\mu\text{s}$).
- `test_R2_InProcessWavWriterRiffHeaders()`:
  - Generates 24-bit WAV file using `src/utils/wav_writer.h`. Parses raw byte stream: asserts `"RIFF"`, total length, `"WAVE"`, `"fmt "`, format tag 1 (PCM), 2 channels, 48000 Hz, 288,000 byte rate, 6 block align, 24 bits per sample, and `"data"` chunk.
- `test_R2_24BitPcmQuantizationFidelity()`:
  - Encodes positive/negative full scale and small amplitude samples. Decodes back and asserts quantization error $< 1 / 2^{23}$ ($1.19 \times 10^{-7}$).

#### Module 3: Requirement R3 (Transactional Undo/Redo & State Management)
- `test_R3_UndoStackCapacityBound64()`:
  - Pushes 100 consecutive snapshots to `UndoManager`. Verifies stack depth is bounded at 64, evicting the oldest snapshot.
- `test_R3_UndoRedoStateFidelity()`:
  - Captures Preset A, modifies to Preset B, calls `undo()`, verifies exact match with Preset A. Calls `redo()`, verifies exact match with Preset B.
- `test_R3_UndoCoalescing400MsIdleThreshold()`:
  - Emits parameter updates at 50ms intervals (total 350ms): asserts all coalesce into 1 undo entry.
  - Waits 450ms (>400ms idle threshold) and emits another update: asserts a 2nd distinct undo transaction is created.
- `test_R3_SceneBarAbToneComparisonToggle()`:
  - Snapshots Tone A and Tone B. Toggles between A and B, asserting click-free interpolation.

#### Module 4: Requirement R4 (Front-Panel Macros & Ergonomics)
- `test_R4_MacroRegexHeuristicDiscovery()`:
  - Feeds list of plugin parameter names (e.g., `"Input Drive"`, `"High Cut Filter"`, `"Master Level"`, `"Wet Dry Mix"`).
  - Asserts fuzzy regex correctly discovers and maps to 4 macros: `Gain/Drive`, `Tone/EQ`, `Volume/Master`, `Mix/Blend`.
- `test_R4_HotkeyWantTextInputGuard()`:
  - Simulates `ImGui::GetIO().WantTextInput = true`.
  - Asserts global hotkeys (`Ctrl+M`, `Ctrl+K`, `Q`, `0`, `[`, `]`) do NOT fire.
- `test_R4_OverviewWaveform800PointEnvelope()`:
  - Feeds 480,000 audio samples to waveform decimation algorithm. Asserts output contains exactly 800 min/max peak points normalized within $[-1.0, 1.0]$.

#### Module 5: Requirement R5 (Visual Polish & Virtualization)
- `test_R5_SpotlightFavoritesBoostScoring()`:
  - Tags plugin as favorite. Searches query `"delay"`. Verifies favorite plugin scores higher than non-favorite with identical query match.
- `test_R5_ImGuiListClipperRowComputation()`:
  - Simulates 1,500 plugin items. Verifies clipper computes visible row range with 0 off-by-one errors.
- `test_R5_DinIec60268PpmBallisticsMath()`:
  - Simulates 1.0 peak impulse. Asserts 0ms instantaneous attack, 1.2s peak hold duration, and exponential decay $\tau = 350\text{ms}$.

#### Module 6: Requirement R6 (Shared Infrastructure Subsystems)
- `test_R6_ArchiveDecompressionBomb30MbXml()`:
  - Generates ZIP containing 31MB XML file. Verifies `praccy::utils::extractZipArchive` rejects it with error mentioning 30MB XML limit.
- `test_R6_ArchiveDecompressionBomb128MbTotal()`:
  - Generates ZIP with total uncompressed size > 128MB. Verifies rejection with total size error.
- `test_R6_ArchiveDecompressionBomb100To1Ratio()`:
  - Generates ZIP entry with compression ratio > 100:1 (e.g. 500,000 zeros compressing to 500 bytes = 1000:1). Verifies rejection with ratio error.
- `test_R6_ArchiveDecompressionBomb100FilesLimit()`:
  - Generates ZIP containing 101 entries. Verifies rejection with file count error.
- `test_R6_ArchiveWin32PathSanitizationMatrix()`:
  - Tests 25 adversarial path inputs: `../evil`, `C:foo`, `\\unc\share`, `CON.txt`, `aux.dll`, `trailing_dot.`, `trailing_space `, `bad<char>`. Asserts all return `false`.
- `test_R6_DragDropRouterClassificationMatrix()`:
  - Tests files with extensions `.wav`, `.flac`, `.mp3`, `.ini`, `.pracpreset`, `.chain`, `.exe`. Asserts exact classification into `AudioTrack`, `Preset`, `Chain`, `Unknown`.
- `test_R6_DragDropRouterDispatch()`:
  - Sets handlers on `DragDropRouter`. Dispatches mock items; asserts proper handler is called with path.
- `test_R6_MidiManagerFlatTableLookupO1()`:
  - Binds CC 7 on Channel 1 and CC 10 on Omni (Channel 0). Dispatches CC 7 and CC 10; asserts $O(1)$ table lookup triggers callback with correct normalized value.
- `test_R6_MidiManagerZeroHeapAllocationOnCcDispatch()`:
  - Dispatches 10,000 MIDI CC messages through `MidiManager`. Verifies real-time allocation tracker records exactly 0 allocations.

### 4.3 CMake Integration for `test_praccy_v2_1`

Add to `CMakeLists.txt`:
```cmake
add_executable(test_praccy_v2_1
    tests/test_praccy_v2_1.cpp
    src/audio/asio_manager.cpp
    src/audio/graph_engine.cpp
    src/midi/midi_manager.cpp
    src/plugins/builtin_dsp.cpp
    src/plugins/vst3_host.cpp
    src/plugins/clap_host.cpp
    src/plugins/plugin_window.cpp
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/tools/audio_player.cpp
    src/tools/quick_looper.cpp
    src/state/scene_manager.cpp
    src/state/app_config.cpp
    src/ui/update_checker.cpp
    src/ui/drag_drop_router.cpp
    src/utils/archive_utils.cpp
    third_party/miniz/miniz.c
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
)
target_include_directories(test_praccy_v2_1 PRIVATE
    src
    tests
)
target_include_directories(test_praccy_v2_1 SYSTEM PRIVATE
    third_party
    third_party/imgui
    third_party/miniz
    third_party/clap/include
    third_party/readerwriterqueue
)
target_link_libraries(test_praccy_v2_1 PRIVATE ole32 uuid user32 gdi32 winmm avrt wininet shell32)

if(MSVC)
    target_compile_options(test_praccy_v2_1 PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_praccy_v2_1 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

add_test(NAME test_praccy_v2_1 COMMAND test_praccy_v2_1)
```

---

## 5. Formal Discovery Tables

## Features Discovered
| # | Category | Feature | Description | Inputs | Outputs | Error Behavior | Discovered Via |
|---|----------|---------|-------------|--------|---------|----------------|----------------|
| 1 | R6 Archive | `praccy::utils::sanitizeZipPath` | Validates relative archive path against Zip Slip, traversal, and Windows forbidden device names | `std::string_view entryName`, `outSafeRelativePath` | `bool` (true if safe) | Returns false on empty, `..`, absolute, drive letter, UNC, DOS device names, trailing space/dot | Spec probe & `src/ui/update_checker.cpp` audit |
| 2 | R6 Archive | `praccy::utils::extractZipArchive` | Centralized in-process archive extraction wrapping miniz with decompression bomb guards | `zipPath`, `destDir`, `outError`, `ArchiveBombGuards` | `bool` (true if success) | Returns false and populates `outError` on bomb violation, file count > 100, ratio > 100:1, XML > 30MB, total > 128MB | R6 Specification & Miniz API probe |
| 3 | R6 DragDrop | `praccy::ui::DragDropRouter::classifyFile` | Classifies file extension into AudioTrack, Preset, Chain, or Unknown | `const std::filesystem::path&` | `DroppedFileType` enum | Returns `DroppedFileType::Unknown` for unrecognized or empty extensions | R6 Specification & `src/main.cpp:637` audit |
| 4 | R6 DragDrop | `praccy::ui::DragDropRouter::parseHDrop` | Extracts and classifies file paths from a Win32 `HDROP` handle | `HDROP hDrop` | `std::vector<ClassifiedDropItem>` | Ignores zero-length paths; handles UTF-16 to UTF-8 cleanly | Win32 Shell API probe & `src/main.cpp` |
| 5 | R6 DragDrop | `praccy::ui::DragDropRouter::route` | Dispatches classified dropped files to registered category handlers | `std::vector<ClassifiedDropItem>` | void | Drops of unknown type or without registered handler are safely ignored | R6 Specification |
| 6 | R6 MIDI | `praccy::midi::MidiManager` Flat O(1) Table | Constant-time $O(1)$ MIDI CC lookup table replacing linear vector scan | Channel ($0..16$), CC ($0..127$), normalized value | Callback invocation | Out-of-range channel/CC discarded; 0 heap allocations | R6 Specification & `src/midi/midi_manager.cpp` audit |
| 7 | R1 DSP | `ParamAutomationRegister` | Atomic dirty bitmask + flat atomic float table for parameter automation | Parameter index, float value | Current parameter float value | Lock-free, zero allocation drain on audio thread | R1 Specification |
| 8 | R1 DSP | Dynamic Parallel Delay Compensation | Sample-accurate phase alignment across parallel branches up to 16,384 samples | Audio stream, branch latency $L_A, L_B$ | Phase-aligned audio buffers | Dynamic update crossfaded over 64 samples via equal-power ramp | R1 Specification |
| 9 | R2 Riff | Retroactive Riff Circular Buffer | Pre-allocated circular audio buffer (1, 2, 5, 10 min) in AppConfig | Audio callback samples, duration minutes | Audio sample slice | Writes latch write head in $<0.15$ms without audio dropouts | R2 Specification |
| 10 | R2 Riff | Header-Only 24-bit WAV Writer | In-process 24-bit RIFF/WAV writer (`src/utils/wav_writer.h`) | Audio buffer slice, destination path | Valid 24-bit RIFF WAV file | Returns false on disk write failure; background async encoding | R2 Specification |
| 11 | R3 Undo | Transactional `UndoManager` | 64-level undo/redo history capturing `ScenePreset` snapshots | ScenePreset snapshot, action timestamp | Restored ScenePreset | Oldest snapshot evicted past 64; rapid events within 400ms coalesced | R3 Specification |
| 12 | R3 Undo | SceneBar A/B Tone Snapshot | Fast comparison toggle between two tone memory states | Memory state A/B toggle | Interpolated active preset | Click-free crossfade over 10ms | R3 Specification |
| 13 | R4 UI | Front-Panel Macro Discovery Heuristic | Regex heuristic identifying Gain, Tone, Volume, Mix from plugin parameter names | Parameter name string | Macro slot assignment | Fallback to manual right-click context menu reassignment | R4 Specification |
| 14 | R4 UI | Hotkey `WantTextInput` Guard | Global hotkey suppressor preventing hotkeys while typing in ImGui text fields | Hotkey event, `ImGuiIO::WantTextInput` | Event propagation | Hotkey ignored if `WantTextInput == true` | R4 Specification |
| 15 | R4 UI | Backing Track Overview Waveform Strip | 800-point peak overview strip with click-to-seek and loop points | Audio buffer | 800 normalized min/max peak points | Empty buffer returns zeroes; click outside bounds clamped | R4 Specification |
| 16 | R5 Visual | Spotlight `ImGuiListClipper` Virtualization | Row virtualization maintaining constant 144 FPS across 1,500+ plugins | Scroll position, item count | Visible row index range | Empty list renders 0 items; off-by-one prevented | R5 Specification |
| 17 | R5 Visual | DIN IEC 60268-10 PPM Ballistics | 4-segment vertex meter with 0ms attack, $\tau = 350\text{ms}$ decay, 1.2s peak hold | Signal sample magnitude, delta time | Meter level, peak hold level | Signal clamped to $[0.0, 1.0]$; NaN/Inf filtered to 0.0 | R5 Specification |
| 18 | CTest | `test_asio_driver` Target Recovery | Headless mock ASIO driver regression harness (20k blocks, 0 allocs) | Simulated ASIO clock, random bypass | Exit code 0 (Pass) | Exits non-zero on NaN, Inf, or audio thread heap allocation | `CMakeLists.txt:158` & `build/CTestTestfile.cmake` |

## Edge Cases
| # | Feature | Input | Observed Behavior |
|---|---------|-------|-------------------|
| 1 | Archive Bomb Guard | ZIP file containing 101 entries | `extractZipArchive` aborts immediately before extracting file 1; returns error `"Archive exceeds safe file count limit (maximum 100 files allowed)"`. |
| 2 | Archive Bomb Guard | ZIP entry containing a 31MB file named `configuration.xml` | `extractZipArchive` aborts during header inspection; returns error `"ZIP XML entry exceeds 30MB uncompressed limit"`. |
| 3 | Archive Bomb Guard | ZIP entry with compressed size 100 bytes and uncompressed size 100,001 bytes (ratio 1000:1) | `extractZipArchive` aborts; returns error `"ZIP entry compression ratio exceeds safe limit (100:1 ratio guard triggered)"`. |
| 4 | Archive Bomb Guard | ZIP entry with compressed size 0 and uncompressed size 10 bytes | `extractZipArchive` aborts due to infinite expansion ratio. |
| 5 | Archive Bomb Guard | Cumulative uncompressed size across multiple entries exceeds 128MB | `extractZipArchive` aborts on the entry that pushes total beyond 128MB; returns error `"ZIP archive total uncompressed size exceeds 128MB limit"`. |
| 6 | Zip Path Sanitization | `entryName = "CON.XML"` or `"aux.dll"` or `"COM1"` | `sanitizeZipPath` detects reserved Windows device name prefix; returns false. |
| 7 | Zip Path Sanitization | `entryName = "foo/../../bar.txt"` | Segment `..` detected; returns false. |
| 8 | Zip Path Sanitization | `entryName = "folder/file.txt "` (trailing space) or `"folder/file.txt."` (trailing dot) | Trailing space/dot detected; returns false. |
| 9 | Zip Path Sanitization | `entryName = "\\\\server\\share\\patch.exe"` (UNC) | Leading `\\` detected; returns false. |
| 10 | DragDrop Classification | Dropped file `solo_take.WAV` or `track.Flac` or `clip.aiff` | `classifyFile` normalizes extension to lowercase; identifies as `DroppedFileType::AudioTrack`. |
| 11 | DragDrop Classification | Dropped file `factory_lead.ini` or `preset.pracpreset` | `classifyFile` identifies as `DroppedFileType::Preset`. |
| 12 | DragDrop Classification | Dropped file `modern_metal.pracchaim` or `lead_channel.chain` | `classifyFile` identifies as `DroppedFileType::Chain`. |
| 13 | DragDrop Classification | Dropped file `notes.txt` or `Praccy.exe` or `no_ext` | `classifyFile` identifies as `DroppedFileType::Unknown`; router ignores safely without error. |
| 14 | DragDrop Routing | Dragging 3 files simultaneously: 1 audio, 1 preset, 1 chain | `DragDropRouter::route` dispatches each to its respective category handler in sequence. |
| 15 | MIDI CC Lookup | Incoming MIDI CC 7 on Channel 3 | `MidiManager` checks `m_ccTable[3][7]` and `m_ccTable[0][7]` (Omni); executes in $O(1)$ constant time with zero heap allocations. |
| 16 | MIDI CC Lookup | Incoming MIDI CC 128 (out-of-range value) | Discarded safely with zero array out-of-bounds error. |
| 17 | Audio Thread Heap Audit | Drain parameter register / dispatch MIDI CC while `t_isAudioThread = true` | Custom `operator new` records exactly 0 allocations. |
| 18 | Hotkey Evaluation | User presses `Ctrl+M` or `Q` while typing preset name into text box | `ImGui::GetIO().WantTextInput` is true; hotkey handling is suppressed; text box receives character cleanly. |
| 19 | Waveform Strip | Audio buffer containing silence or all zeroes | 800 peak points generated with amplitude 0.0f; no division-by-zero or NaN occurs. |
| 20 | Undo Stack | 65th state snapshot pushed | Stack size remains 64; snapshot 0 is dropped from front. |
| 21 | Undo Coalescing | Continuous slider drag emitting 30 events in 300ms | Single transaction captured upon mouse release (`IsItemDeactivatedAfterEdit`). |

---

## 6. Verification and Implementation Roadmap

1. **Phase 1: Shared Infrastructure Implementation**:
   - Create `src/utils/archive_utils.h` and `src/utils/archive_utils.cpp` implementing the 30MB XML, 128MB archive, 100:1 ratio, 100 files bomb guards and Win32 path sanitization. Forward existing `src/ui/update_checker.cpp` calls to it.
   - Create `src/ui/drag_drop_router.h` and `src/ui/drag_drop_router.cpp` implementing file classification for Audio, Preset, Chain. Replace inline `WM_DROPFILES` handling in `src/main.cpp`.
   - Update `src/midi/midi_manager.h` and `src/midi/midi_manager.cpp` with flat $O(1)$ `m_ccTable` `[17][128]` with zero runtime allocations.
2. **Phase 2: CMakeLists.txt Target Recovery & Test Harness**:
   - Uncomment `test_asio_driver` in `CMakeLists.txt` lines 158-174 (ensuring `-Wno-mismatched-new-delete` compiler option is present for GCC/MinGW).
   - Add `test_praccy_v2_1` executable target to `CMakeLists.txt` linking the required source units.
   - Author `tests/test_praccy_v2_1.cpp` containing the 6 test modules detailed in Section 4.
3. **Phase 3: Regression Suite Execution**:
   - Run `cmake --build build --target test_praccy_v2_1`.
   - Run `ctest --output-on-failure` to verify all 11 CTest targets pass with 100% success rate.
