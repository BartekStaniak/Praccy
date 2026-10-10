## 2026-10-06T18:48:00Z
You are a Worker subagent for Milestone 1 of the Praccy v2.0 Architectural Blueprint.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m1/.
Project root is f:/Projects/Praccy.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

You MUST read these files before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m1_1/handoff.md (SPSC queue blueprint)
4. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m1_2/handoff.md (Tuner decoupling blueprint)
5. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m1_3/handoff.md (24-bit unpack & test blueprint)

Exclusive Write Ownership:
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

Your tasks:
1. Feature 1: Implement the lock-free SPSC command queue and reclamation queue for `ParallelBranch::m_slots` using `readerwriterqueue/readerwriterqueue.h` in `src/audio/graph_engine.h` and `src/audio/graph_engine.cpp`. Ensure plugin destructors NEVER execute on the audio callback thread by using the reclamation return queue. Expose `collectReclaimedSlots()` through `ParallelSplitMergeBlock` and `GraphEngine::processReclamation()`, and invoke it at the start of `RackView::renderSignalRack()`.
2. Features 2 & 3: Decouple `InstrumentTuner` in `src/tools/tuner.h` and `src/tools/tuner.cpp`. Implement `AudioRingBuffer` for wait-free sample ingestion (`pushSamples`), 60 Hz background thread computing YIN pitch detection, seqlock atomic snapshot for `currentResult()`, pre-allocation in `prepare(sampleRate, maxBlockSize)`. Update `src/main.cpp` to call `tuner.pushSamples()` and completely eliminate `s_tunerMixBuf.resize()`.
3. Features 4 & 5: Implement 24-bit sample unpacking and packing for `ASIOSTInt24LSB` and `ASIOSTInt32LSB24` as static methods and in `processAudio()` in `src/audio/asio_manager.h` and `src/audio/asio_manager.cpp`. Fix MMCSS lifecycle by registering `AvSetMmThreadCharacteristicsW` in `AsioManager::start()` and reverting in `stop()` on the control thread.
4. Feature 6: Add `GraphEngineTest.ConcurrentParallelMutation` and `AsioManagerTest.Format24BitUnpack` in `tests/test_praccy.cpp`. Update `CMakeLists.txt` so `test_praccy` compiles with `asio_manager.cpp`, `winmm`, and `avrt`.
5. Run the build and test suite (`test_praccy.exe`), verify all tests pass with exit code 0.

Deliverable:
Author a complete handoff report at f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m1/handoff.md following the Handoff Protocol (Observation, Logic Chain, Caveats, Conclusion, Verification Method). Include build and test execution results. Update progress.md in your working directory and notify parent via send_message when complete.
