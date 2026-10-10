# BRIEFING — 2026-10-06T19:05:00Z

## Mission
Implement Milestone 1 of Praccy v2.0: Lock-free SPSC parallel branch mutation with reclamation queue, Tuner background thread & ring buffer decoupling, 24-bit ASIO unpack/pack & MMCSS lifecycle fix, and unit/concurrency tests.

## 🔒 My Identity
- Archetype: worker
- Roles: implementer, qa, specialist
- Working directory: f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m1
- Original parent: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Milestone: Milestone 1

## 🔒 Key Constraints
- DO NOT CHEAT: Genuine implementations only, no hardcoded results or dummy facades.
- Plugin destructors NEVER execute on audio callback thread (use reclamation queue).
- Tuner: AudioRingBuffer wait-free ingestion (pushSamples), 60 Hz background thread for YIN, atomic snapshot (seqlock), zero allocation in audio callback.
- ASIO: 24-bit unpack/pack (ASIOSTInt24LSB, ASIOSTInt32LSB24) as static methods and in processAudio(), MMCSS registered on start() and reverted on stop() on control thread.
- Tests: Add GraphEngineTest.ConcurrentParallelMutation and AsioManagerTest.Format24BitUnpack in tests/test_praccy.cpp, link asio_manager, winmm, avrt.
- Exclusive write ownership:
  - src/audio/graph_engine.h
  - src/audio/graph_engine.cpp
  - src/tools/tuner.h
  - src/tools/tuner.cpp
  - src/audio/asio_manager.h
  - src/audio/asio_manager.cpp
  - src/main.cpp
  - src/ui/rack_view.cpp
  - tests/test_praccy.cpp
  - CMakeLists.txt

## Current Parent
- Conversation ID: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Updated: 2026-10-06T19:05:00Z

## Task Summary
- **What to build**: Milestone 1 features (Parallel branch lock-free mutation & reclamation, Tuner worker thread decoupling, ASIO 24-bit unpack/pack & MMCSS lifecycle, tests).
- **Success criteria**: All tests pass in test_praccy.exe with exit code 0; real thread-safe non-blocking audio processing.
- **Interface contracts**: f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
- **Code layout**: f:/Projects/Praccy/

## Key Decisions Made
- Dual-tier slot management in ParallelBranch (`m_uiSlots` and `m_activeSlots`) with SPSC command queue and return reclamation queue.
- Fail-safe stashed reclamation array preventing any possibility of destructor execution on audio thread even under queue saturation.
- UI-thread pre-allocation of slots in `ParallelBranch::addSlot()` ensuring real-time thread never encounters unallocated buffers.
- Dedicated 60 Hz background thread for YIN pitch detection reading from power-of-2 `AudioRingBuffer` wait-free, with atomic seqlock snapshots for UI reads.
- Static helper methods in `AsioManager` for 24-bit unpacking and packing with bit-exact round-trip scaling and rounding.
- Control-thread MMCSS lifecycle management in `AsioManager::start()` and `stop()`, with thread-local registration inside callback.

## Artifact Index
- f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m1/DISPATCH.md
- f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m1/BRIEFING.md
- f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m1/progress.md
- f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m1/handoff.md

## Change Tracker
- **Files modified**:
  - `src/audio/graph_engine.h`: SlotCommand and ParallelBranch SPSC/reclamation queues.
  - `src/audio/graph_engine.cpp`: ParallelBranch SPSC queue drain, stash fail-safe, and processReclamation.
  - `src/tools/tuner.h`: AudioRingBuffer and decoupled InstrumentTuner interface.
  - `src/tools/tuner.cpp`: AudioRingBuffer implementation, 60Hz worker thread, and seqlock snapshot.
  - `src/audio/asio_manager.h`: 24-bit static unpack/pack declarations.
  - `src/audio/asio_manager.cpp`: 24-bit unpack/pack implementation and MMCSS start/stop lifecycle.
  - `src/main.cpp`: tuner.pushSamples() call and elimination of s_tunerMixBuf.
  - `src/ui/rack_view.cpp`: invocation of m_graph.processReclamation() in renderSignalRack().
  - `tests/test_praccy.cpp`: ConcurrentParallelMutation, Format24BitUnpack, and AsynchronousDecoupling tests.
  - `CMakeLists.txt`: compilation of test_praccy with asio_manager.cpp, winmm, and avrt.
- **Build status**: All targets (`test_praccy`, `Praccy`, `test_asio_driver`) build with 0 errors.
- **Pending issues**: None.

## Quality Status
- **Build/test result**: All 14/14 unit and concurrency tests passed in `test_praccy.exe`.
- **Lint status**: Clean, zero warnings on new code.
- **Tests added/modified**: `GraphEngineTest.ConcurrentParallelMutation`, `AsioManagerTest.Format24BitUnpack`, `InstrumentTuner Asynchronous Decoupling`.

## Loaded Skills
- None
