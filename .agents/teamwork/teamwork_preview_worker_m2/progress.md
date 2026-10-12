# Progress Log — Milestone 2 Worker

Last visited: 2026-10-12T18:37:30Z

## Status
Milestone 2 (Shared Infrastructure & State Engine) implementation complete across all 6 technical objectives.
Empirical verification:
- `test_challenger_m2` passes (exit code 0).
- `test_challenger_m2_1` passes (exit code 0).
- Clean compilation for all 10 project build targets with zero errors.

## Completed Objectives
1. **Archive Security & Utility (`src/utils/archive_utils.h` & `archive_utils.cpp`)**:
   - In-process ZIP extraction wrapping vendored `miniz`.
   - Native Win32 path sanitization (`sanitizeZipPath`): blocks Zip Slip traversal, drive letters, UNC shares, reserved MS-DOS device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9`), null bytes, trailing spaces/dots.
   - Decompression bomb guards: `MAX_SINGLE_FILE = 30MB`, `MAX_TOTAL_UNCOMPRESSED = 128MB`, `MAX_COMPRESSION_RATIO = 100`, `MAX_FILE_COUNT = 100`.
   - Refactored `src/ui/update_checker.cpp` to use `praccy::utils::archive_utils::extractZipArchive`.

2. **Centralized Drag-and-Drop Routing (`src/ui/drag_drop_router.h` & `drag_drop_router.cpp`)**:
   - Implemented `DragDropRouter` classifying dropped files:
     - Audio tracks (`.wav`, `.wave`, `.flac`, `.mp3`, `.ogg`, `.aiff`, `.aif`) -> AudioPlayer / QuickLooper.
     - Presets (`.pracpreset`, `.ini`, `.preset`, `.vstpreset`, `.clappreset`) -> SceneManager.
     - Signal chains (`.pracchaim`, `.chain`, `.rack`) -> RackView / GraphEngine.
   - Parsed Win32 `HDROP` handles with Win32 `DragQueryFileW`.
   - Connected router to `src/main.cpp` `WM_DROPFILES` message handler.

3. **Flat O(1) MIDI CC Dispatch Table (`src/midi/midi_manager.h` & `midi_manager.cpp`)**:
   - Replaced $O(N)$ vector traversal with flat lookup matrix `FlatCcBinding m_ccTable[17][128]` (channels 0..16, CC 0..127).
   - O(1) lookup in `handleControlChange(channel, cc, val)`.
   - Strictly 0 heap allocations and 0 mutex locks in the CC dispatch path.

4. **AppConfig Retroactive Buffer Persistence (`src/state/app_config.h` & `app_config.cpp`)**:
   - Added `uint32_t retroactiveBufferMinutes{2};` with validation for allowed durations (1, 2, 5, 10 minutes).
   - Implemented loading and saving in `config.ini` under `[Audio] retroactive_buffer_minutes`.

5. **Transactional UndoManager (`src/state/undo_manager.h` & `undo_manager.cpp`)**:
   - Full-fidelity transactional state undo/redo engine with 64 history levels.
   - Captures `ScenePreset` snapshots.
   - Coalescing logic: 400ms idle threshold for continuous parameter sweeps, structural rack mutations, scene switches, mouse-release commits.
   - Equality operators for `PluginSlotPreset`, `BranchPreset`, `NodePreset`, `ScenePreset`.
   - Seamless crossfading integration via `applyScenePresetToGraph`.

6. **Build System Updates (`CMakeLists.txt`)**:
   - Added `src/utils/archive_utils.cpp`, `src/ui/drag_drop_router.cpp`, `src/state/undo_manager.cpp` to `Praccy`, `test_praccy`, `test_challenger_m2`, `test_challenger_m2_1`.
   - Re-enabled `test_asio_driver` compilation with `-Wno-mismatched-new-delete`.
