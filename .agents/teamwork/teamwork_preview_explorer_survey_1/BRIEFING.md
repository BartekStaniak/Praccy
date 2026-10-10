# BRIEFING — 2026-10-06T18:33:00Z

## Mission
Survey the Audio DSP Subsystem and Real-Time Engine (Requirement R1 and associated Acceptance Criteria) for Praccy v2.0 Architectural Blueprint.

## 🔒 My Identity
- Archetype: Teamwork explorer
- Roles: Read-only investigation, codebase analysis, synthesis report
- Working directory: f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_1
- Original parent: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Milestone: Architectural Blueprint Survey (Phase 1)

## 🔒 Key Constraints
- Read-only investigation — do NOT implement
- Adhere strictly to 5-component handoff report (Observation, Logic Chain, Caveats, Conclusion, Verification Method)
- File workspace convention: only write to `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_1/`

## Current Parent
- Conversation ID: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Updated: not yet

## Investigation State
- **Explored paths**:
  - `src/audio/graph_engine.h` and `src/audio/graph_engine.cpp` (`ParallelBranch::m_slots`, concurrency race)
  - `src/ui/rack_view.cpp` and `src/state/scene_manager.cpp` (UI mutations and slot queries)
  - `third_party/readerwriterqueue/` (`readerwriterqueue.h`, `atomicops.h`)
  - `src/tools/tuner.h`, `src/tools/tuner.cpp`, `src/main.cpp` (YIN algorithm, threading model, `s_tunerMixBuf.resize`)
  - `src/audio/asio_manager.h`, `src/audio/asio_manager.cpp`, `src/audio/asio_defs.h` (formats, MMCSS)
  - `tests/test_praccy.cpp`, `tests/test_asio_driver.cpp`, `CMakeLists.txt`, `build/`
- **Key findings**:
  1. `ParallelBranch::m_slots` is an unsynchronized `std::vector<std::unique_ptr<PluginSlot>>` mutated directly by UI thread while iterated without locks in `ParallelBranch::process()` on audio thread.
  2. `readerwriterqueue` is fully vendored in `third_party/readerwriterqueue/` with Cameron Desrochers' lock-free SPSC queue.
  3. `InstrumentTuner` runs $O(N^2/4)$ YIN (~1M ops) directly on real-time ASIO audio callback thread on every block (~190M ops/sec), causing severe DSP load; `main.cpp` calls `s_tunerMixBuf.resize()` on the audio callback thread.
  4. `asio_manager.cpp` lacks unpacking and packing for `ASIOSTInt24LSB` (type 17) and `ASIOSTInt32LSB24` (type 27), and does not call `AvSetMmThreadCharacteristicsW` in `start()`.
  5. Audio test suite currently passes 11 unit tests via `test_praccy.exe`, but lacks `GraphEngineTest.ConcurrentParallelMutation`, `AsioManagerTest.Format24BitUnpack`, and the mock ASIO driver headless regression harness in `test_asio_driver.cpp`.
- **Unexplored areas**: None for this survey scope.

## Key Decisions Made
- Fully documented all 5 survey objectives with line-level code citations, mathematical details, and architectural recommendations in `handoff.md`.

## Artifact Index
- `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_1/DISPATCH.md` — Record of initial dispatch message
- `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_1/progress.md` — Liveness and progress tracker
- `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_1/BRIEFING.md` — Working memory and survey findings
- `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_1/handoff.md` — Comprehensive survey report
