# BRIEFING — 2026-10-06T22:00:00Z

## Mission
Implement Milestone 2: SecOps, Hardening & Crash Isolation (Features 7, 8, 9, 10, 11, 12, 13) for Praccy v2.0.

## 🔒 My Identity
- Archetype: worker
- Roles: implementer, qa, specialist
- Working directory: f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m2/
- Original parent: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Milestone: Milestone 2 (SecOps, Hardening & Crash Isolation)

## 🔒 Key Constraints
- Integrity mandate: No cheating, no hardcoded test outputs, no facades, no skipping logic.
- Vendor standard miniz in third_party/miniz.
- Complete removal of std::system, cmd.exe, powershell.exe, batch scripts.
- SEH/VEH plugin isolation with real-time safe execution (<15ns, no heap allocs) and dry audio pass-through on crash.
- Non-throwing std::from_chars parsing replacing std::stoul/stof/stoi.
- Zero compiler warnings under /W4 /WX (MSVC) or -Wall -Wextra -Werror (GCC).
- All tests must pass with exit code 0.

## Current Parent
- Conversation ID: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Updated: 2026-10-06T20:34:24Z

## Task Summary
- **What to build**: In-process ZIP extraction (miniz), safe direct updater via Win32 CreateProcessW, Win32 SEH/VEH plugin crash isolation, std::from_chars parsing, window placement persistence, compiler hardening (/W4 /WX), and test suite expansion.
- **Success criteria**: Clean compilation with /W4 /WX, all unit tests passing, zero shell/script updater calls, robust crash isolation catching hardware access violations.
- **Interface contracts**: PROJECT.md, Explorer handoffs m2_1, m2_2, m2_3.
- **Code layout**: Project root f:/Projects/Praccy.

## Key Decisions Made
- Vendored official miniz v3.1.2 in `third_party/miniz/` with compilation isolated to `-w` / `/W0`.
- Implemented portable Win32 crash isolation (`src/plugins/crash_isolation.h`): C leaf `sehExecuteLeaf` with 0 local objects for MSVC (C2712 compliant) and thread-local intrusive VEH stack + `setjmp`/`longjmp` for GCC.
- Replaced all legacy throwing `std::stoul`, `std::stof`, `std::stoi`, `std::atoi`, `std::atof` with `src/utils/parse_utils.h` non-throwing functions.
- Implemented multi-monitor safe window placement persistence (`GetWindowPlacement`/`SetWindowPlacement`) with `MonitorFromRect` fallback.
- Replaced updater batch script / `cmd.exe` execution with direct `Praccy.exe --apply-update <pid> "<target>"` via Win32 `CreateProcessW`.

## Change Tracker
- **Files modified**:
  - `CMakeLists.txt`: Hardened flags (`/W4 /WX`, `-Wall -Wextra -Werror`), isolated `THIRD_PARTY_SOURCES` with `/W0`/`-w`.
  - `third_party/miniz/miniz.h`, `miniz.c`: Vendored miniz v3.1.2.
  - `src/utils/parse_utils.h`: Non-throwing parsing utilities.
  - `src/state/scene_manager.h`, `scene_manager.cpp`: Non-throwing parsing for preset INI, added `scenes()` getter.
  - `src/state/app_config.h`, `app_config.cpp`: Window placement fields and non-throwing parsing.
  - `src/plugins/crash_isolation.h`: Win32 SEH & VEH crash isolation framework.
  - `src/plugins/plugin_base.h`: Added fault tracking (`m_faulted`, `m_faultReason`, `isFaulted()`).
  - `src/audio/audio_node.h`: Added virtual `isFaulted()`.
  - `src/audio/graph_engine.h`, `graph_engine.cpp`: Dry audio pass-through on faulted inner plugins in `PluginSlot`.
  - `src/plugins/vst3_host.cpp`, `clap_host.cpp`: Crash-isolated audio, GUI, parameter, and state operations.
  - `src/plugins/plugin_scanner.cpp`, `asio_manager.cpp`: Compiler warning cleanup (`FARPROC` casts, `snprintf`).
  - `src/ui/update_checker.h`, `update_checker.cpp`: Zip Slip defense, in-process extraction, direct updater invocation.
  - `src/main.cpp`: Window placement restore/save, `--apply-update` updater worker loop.
  - `src/ui/rack_view.cpp`: Crimson fault alert badge, dry bypass notice, reload button.
  - `tests/test_praccy.cpp`: 5 new comprehensive test cases (19 tests total).
- **Build status**: PASS (both `test_praccy.exe` and `Praccy.exe` compiled with zero warnings/errors).
- **Pending issues**: None.

## Quality Status
- **Build/test result**: PASS (19/19 tests passing with exit code 0).
- **Lint status**: 0 warnings under `-Wall -Wextra -Werror` / `/W4 /WX`.
- **Tests added/modified**: Zip Slip traversal, in-process extraction & Zip Bomb, zero prohibited shell commands (45 files inspected), plugin crash isolation & dry bypass, corrupted presets resilience.

## Artifact Index
- DISPATCH.md — Dispatch assignment
- progress.md — Task heartbeat
- handoff.md — Final 5-component handoff report
