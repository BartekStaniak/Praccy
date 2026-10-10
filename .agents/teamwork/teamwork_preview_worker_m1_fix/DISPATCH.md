## 2026-10-06T20:04:44Z
You are a Worker subagent for Milestone 1 Remediation of the Praccy v2.0 Architectural Blueprint.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m1_fix/.
Project root is f:/Projects/Praccy.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

You MUST read these files before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_challenger_m1_1/handoff.md
4. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_challenger_m1_2/handoff.md

Exclusive Write Ownership:
- src/audio/graph_engine.cpp
- src/audio/graph_engine.h
- src/tools/tuner.cpp
- src/tools/tuner.h
- src/audio/asio_manager.cpp
- src/audio/asio_manager.h

Remediation Tasks:
1. In `src/audio/graph_engine.cpp` (`ParallelBranch::addSlot`):
   When `!m_commandQueue.try_enqueue(std::move(cmd))` fails, call `m_uiSlots.pop_back()` to roll back the shadow registry and prevent dangling pointers / use-after-free.
2. In `src/audio/graph_engine.cpp` (`ParallelBranch::process` `SlotCommandType::Remove`):
   When `cmd.targetSlot != nullptr`, do NOT fall back to index-based removal (`cmd.index`) if `targetSlot` was not found in `m_activeSlots`. Only allow index-based fallback if `cmd.targetSlot == nullptr`.
3. In `src/tools/tuner.cpp` (`InstrumentTuner::prepare`):
   - Call `stop()` before resizing or resetting any buffers to ensure the background worker thread is not actively reading memory.
   - Dynamically compute `m_bufferSize` based on `sampleRate` so that `m_bufferSize / 2 >= sampleRate / 40.0` (e.g. at 44.1k/48k -> 2048, at 88.2k/96k -> 4096, at 176.4k/192k -> 8192), ensuring low E (82.4 Hz) and sub-bass notes are detected reliably across all sample rates up to 192 kHz.
   - Resize `m_inputHistory` (`m_bufferSize`), `m_drainBuffer` (`m_bufferSize`), `m_differenceBuffer` (`m_bufferSize / 2`), and `m_cumulativeDiffBuffer` (`m_bufferSize / 2`).
   - Call `start()` at the end of `prepare()`.
4. In `src/audio/asio_manager.cpp` (`packInt24LSB` and `packInt32LSB24`):
   Safely handle NaNs: `if (std::isnan(src[s])) sample = 0.0f;` to prevent DC pop artifacts on hardware outputs.
5. Verification:
   Build all targets (`test_praccy`, `test_challenger_m1`, `test_challenger_m1_2`) and run all test binaries. Ensure all tests pass 100%.

Deliverable:
Author your completion report at f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m1_fix/handoff.md following the Handoff Protocol. Update progress.md and notify parent via send_message.
