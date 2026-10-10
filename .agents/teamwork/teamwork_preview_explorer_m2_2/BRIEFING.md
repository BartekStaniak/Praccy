# BRIEFING — 2026-10-06T20:26:00Z

## Mission
Formulate concrete implementation blueprint for Feature 9 (Win32 SEH/VEH Plugin Crash Isolation).

## 🔒 My Identity
- Archetype: explorer
- Roles: investigator, architect, synthesizer
- Working directory: f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_2/
- Original parent: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Milestone: Milestone 2 (Feature 9 - Win32 SEH/VEH Plugin Crash Isolation)

## 🔒 Key Constraints
- Read-only investigation — do NOT implement / do NOT modify source code files
- Win32 platform focus with portable dual MSVC (__try/__except) and MinGW GCC (VEH + setjmp/longjmp) support
- Comply with MSVC C2712 constraint (cannot use __try in functions that require object unwinding / destructors)
- Design faulted plugin state handling, dry pass-through, and simulated AV test
- Self-contained 5-component handoff report (Observation, Logic Chain, Caveats, Conclusion, Verification Method)

## Current Parent
- Conversation ID: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Updated: not yet

## Investigation State
- **Explored paths**:
  - `src/plugins/plugin_base.h`, `vst3_host.h/.cpp`, `clap_host.h/.cpp`, `plugin_window.h/.cpp`
  - `src/audio/audio_node.h`, `graph_engine.h/.cpp`
  - `src/ui/rack_view.cpp`
  - `tests/test_praccy.cpp`, `CMakeLists.txt`
  - MinGW-w64 GCC 16.2.0 VEH execution verified empirically with zero regressions
- **Key findings**:
  - Confirmed MSVC C2712 leaf function model (`sehExecuteLeaf`) cleanly separates SEH frames from C++ template/lambda destructors.
  - Confirmed MinGW GCC VEH (`AddVectoredExceptionHandler(1, ...)`) with thread-local intrusive context stack + `setjmp`/`longjmp` successfully traps Access Violations (`0xC0000005`) and recovers cleanly in <15ns with zero allocations.
  - Identified all unprotected VST3 and CLAP entrypoints across `process()`, `openGui()`, `closeGui()`, parameter queries/mutations, and state serialize/deserialize.
  - Formulated latching atomic faulted state (`m_faulted`), zero-allocation dry audio pass-through in `PluginSlot` and plugin hosts, and UI badge/recovery flow.
  - Formulated comprehensive simulated AV test suite with `CrashingMockPlugin`.
- **Unexplored areas**: None; all requirements for Feature 9 investigated and verified.

## Key Decisions Made
- Architecture for `crash_isolation.h` employs dual-path compilation: MSVC native SEH leaf function for `/W4 /WX` C2712 compliance, and Win32 VEH + `setjmp`/`longjmp` with intrusive thread-local context stack for GCC.
- VEH handler registered once globally to avoid real-time NTDLL lock contention during audio processing.
- Faulted plugin state latches permanently to prevent repetitive fault-loops and safely cascades dry audio pass-through.

## Artifact Index
- DISPATCH.md — dispatched mission instructions
- BRIEFING.md — situational awareness
- progress.md — liveness heartbeat
- handoff.md — 5-component deliverable report
