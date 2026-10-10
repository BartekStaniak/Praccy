# BRIEFING — 2026-10-06T18:48:00Z

## Mission
Develop concrete implementation strategy and interface design for Feature 1: Lock-free SPSC Queue for ParallelBranch::m_slots using third_party/readerwriterqueue (Milestone 1).

## 🔒 My Identity
- Archetype: Explorer
- Roles: Read-only investigation, architectural analysis, concrete implementation strategy & interface design
- Working directory: f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m1_1/
- Original parent: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Milestone: Milestone 1 - RT-Safety: SPSC Queue for Parallel Slots

## 🔒 Key Constraints
- Read-only investigation — do NOT implement / modify source code files
- Deliver handoff.md following 5-component Handoff Protocol
- Ensure real-time audio safety: no locks, no allocations, no deallocations on audio thread
- Plugin destructors must NEVER execute on audio thread (reclamation queue to UI thread)
- Thread-safe UI inspection without blocking audio thread

## Current Parent
- Conversation ID: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Updated: 2026-10-06T18:48:00Z

## Investigation State
- **Explored paths**:
  - `third_party/readerwriterqueue/readerwriterqueue.h`, `atomicops.h`
  - `src/audio/graph_engine.h`, `src/audio/graph_engine.cpp`
  - `src/ui/rack_view.cpp`, `src/ui/rack_view.h`
  - `src/state/scene_manager.cpp`
  - `tests/test_praccy.cpp`
  - `CMakeLists.txt`
- **Key findings**:
  - `ParallelBranch::m_slots` was a raw `std::vector<std::unique_ptr<PluginSlot>>` concurrently accessed by audio callback and UI thread.
  - Designed two lock-free SPSC queues: command queue (`SlotCommand` with `AddSlotCommand` and `RemoveSlotCommand`) and reclamation queue (`std::unique_ptr<PluginSlot>`).
  - Solved real-time destructor isolation via reclamation queue + fail-safe stashing.
  - Solved UI rendering and offline unit-test consistency via dual-tier tracking (`m_uiSlots` shadow vector).
  - Designed `GraphEngineTest.ConcurrentParallelMutation` unit test.
- **Unexplored areas**: None for Feature 1. Implementation is ready to be executed by implementer agent.

## Key Decisions Made
- Use `moodycamel::ReaderWriterQueue` with pre-allocated capacity (64) and active capacity (32).
- Use `SlotCommand` flat struct with pointer and index matching for bulletproof removal.
- Add `ParallelSplitMergeBlock::collectReclaimedSlots()` and `GraphEngine::processReclamation()` called at 60Hz from UI render loop.

## Artifact Index
- DISPATCH.md — Task dispatch log
- BRIEFING.md — Situational awareness working memory
- progress.md — Liveness heartbeat and progress tracking
- handoff.md — Final deliverable report with full blueprints and verification methods
