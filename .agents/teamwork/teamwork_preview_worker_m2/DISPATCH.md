## 2026-10-12T18:11:50Z
You are the Shared Infrastructure & State Engine Worker (Milestone 2) for Praccy V2.4 Stage 1.
Your working directory is F:\Projects\Praccy\.agents\teamwork\teamwork_preview_worker_m2.

MANDATORY: Read the authoritative user request at:
F:\Projects\Praccy\.agents\teamwork\ORIGINAL_REQUEST.md
Also read the project architecture and interface contracts at:
F:\Projects\Praccy\.agents\teamwork\PROJECT.md
And the infrastructure and UI survey reports at:
F:\Projects\Praccy\.agents\teamwork\spec_miner_survey_infra\survey_report.md
F:\Projects\Praccy\.agents\teamwork\explorer_survey_ui\survey_report.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Your write ownership (exclusive):
- src/utils/archive_utils.h
- src/utils/archive_utils.cpp
- src/ui/update_checker.cpp
- src/ui/drag_drop_router.h
- src/ui/drag_drop_router.cpp
- src/main.cpp
- src/midi/midi_manager.h
- src/midi/midi_manager.cpp
- src/state/app_config.h
- src/state/app_config.cpp
- src/state/undo_manager.h
- src/state/undo_manager.cpp
- CMakeLists.txt

Objectives for Milestone 2:
1. Implement `src/utils/archive_utils.h` and `archive_utils.cpp`:
   - Centralized archive utility wrapping vendored `miniz`.
   - Native Win32 path sanitization (defense against Zip Slip: disallow `..`, absolute paths, leading slashes/drive letters, colons, null bytes).
   - Enforce decompression bomb guards:
     - `MAX_SINGLE_FILE = 30 * 1024 * 1024` (30MB, specifically disallowing XML/uncompressed files over 30MB)
     - `MAX_TOTAL_UNCOMPRESSED = 128 * 1024 * 1024` (128MB archive total)
     - `MAX_COMPRESSION_RATIO = 100` (100:1 ratio guard)
     - `MAX_FILE_COUNT = 100` files max
   - Refactor `src/ui/update_checker.cpp` to use `archive_utils::extractZipArchive`.
2. Implement `src/ui/drag_drop_router.h` and `drag_drop_router.cpp`:
   - Centralized drag-drop router replacing inline `WM_DROPFILES` in `src/main.cpp`.
   - Classification logic:
     - Audio tracks (`.wav`, `.wave`, `.flac`, `.mp3`) -> dispatch to AudioPlayer / QuickLooper.
     - Presets (`.pracpreset`, `.ini`) -> dispatch to SceneManager.
     - Signal chains (`.pracchaim`, `.chain`) -> dispatch to RackView / GraphEngine.
   - Wire into `src/main.cpp` `WM_DROPFILES` handler.
3. Optimize `src/midi/midi_manager.h` and `midi_manager.cpp`:
   - Replace $O(N)$ vector traversal with a flat $O(1)$ MIDI CC lookup table `FlatCcBinding m_ccTable[17][128];` (channels 0-16, CC 0-127).
   - Zero runtime heap allocations during MIDI CC dispatch.
4. Update `src/state/app_config.h` and `app_config.cpp`:
   - Add `uint32_t retroactiveBufferMinutes{2};` (supported values: 1, 2, 5, 10).
   - Persist to/from configuration INI file.
5. Implement `src/state/undo_manager.h` and `undo_manager.cpp`:
   - Full-fidelity transactional `UndoManager` with up to 64 history levels capturing `ScenePreset` snapshots.
   - Coalescing rules: snapshot on mouse release (`IsItemDeactivatedAfterEdit`), structural rack mutations, scene switches, and rapid continuous parameter/MIDI CC streams using a 400ms idle inactivity threshold.
   - `undo()`, `redo()`, `canUndo()`, `canRedo()`.
6. Update `CMakeLists.txt`:
   - Add new source files (`archive_utils.cpp`, `drag_drop_router.cpp`, `undo_manager.cpp`).
   - Fix commented-out `test_asio_driver` definition with `-Wno-mismatched-new-delete`.
7. Verify:
   - Build project with `cmake --build build --config Release`.
   - Run CTest `ctest --output-on-failure` in `build/`. Ensure all 10 existing test targets pass 100%.

Document all changes and test outputs in `handoff.md` in your working directory.
When finished, send a completion message with handoff details to your orchestrator.
