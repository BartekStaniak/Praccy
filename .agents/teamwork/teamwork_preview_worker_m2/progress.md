# Progress — Milestone 2

Last visited: 2026-10-06T21:59:45Z

- [x] Initialized DISPATCH.md, BRIEFING.md, and progress.md
- [x] Read foundational files:
  - [x] ORIGINAL_REQUEST.md
  - [x] orchestrator/PROJECT.md
  - [x] explorer_m2_1 handoff (Features 7 & 8)
  - [x] explorer_m2_2 handoff (Feature 9)
  - [x] explorer_m2_3 handoff (Features 10, 11, 12, 13)
- [x] Feature 7 & 8: In-Process ZIP Extraction & Direct Updater Execution
  - Vendored miniz v3.1.2 in `third_party/miniz/`
  - Implemented `sanitizeZipEntryPath` with Zip Slip defense
  - Implemented `extractZipArchive` with in-process miniz and Zip Bomb quotas
  - Implemented `applyUpdateAndRestart` via Win32 `CreateProcessW` directly invoking `Praccy.exe --apply-update`
  - Implemented `--apply-update` updater loop in `src/main.cpp`
  - Eliminated all occurrences of `std::system`, `cmd.exe`, `powershell.exe`, and batch files
- [x] Feature 9: Win32 SEH/VEH Plugin Crash Isolation
  - Implemented `src/plugins/crash_isolation.h` with MSVC C2712 leaf SEH and MinGW VEH + setjmp/longjmp context stack
  - Added fault tracking (`m_faulted`, `m_faultReason`, `isFaulted()`) to `AudioNode` and `IPluginInstance`
  - Wrapped VST3 and CLAP audio processing, parameter queries, GUI, and state calls in crash isolation
  - Implemented instant dry signal bypass in `PluginSlot::process()`
  - Added crimson fault alert badge, dry signal indicator, and reload button in `src/ui/rack_view.cpp`
- [x] Features 10, 11, 12: Non-throwing std::from_chars parsing, window placement, compiler hardening
  - Created `src/utils/parse_utils.h` with `parseInteger`, `parseFloat`, `parseDouble`, `parseHexByte`, `hexToBytes`
  - Refactored `src/state/scene_manager.cpp` and `src/state/app_config.cpp` to non-throwing parsing
  - Implemented `saveWindowPlacement` and `restoreWindowPlacement` with `MonitorFromRect` multi-monitor validation
  - Hardened `CMakeLists.txt` with `/W4 /WX` and `-Wall -Wextra -Werror` while cleanly isolating third-party sources
- [x] Feature 13 & Testing: Unit tests in tests/test_praccy.cpp, build & test verification
  - Added `testZipSlipSanitization()`
  - Added `testInProcessMinizArchiveExtraction()`
  - Added `testZeroProhibitedCommandsInCodebase()` (45 files inspected)
  - Added `testPluginCrashIsolation()`
  - Added `testCorruptedPresetsIni()`
  - Verified 19/19 tests passing cleanly
- [x] Final handoff report authored and parent notified
