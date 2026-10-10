# BRIEFING — 2026-10-06T18:43:00Z

## Mission
Develop concrete implementation strategy and interface design for Features 2 & 3: Decoupled InstrumentTuner pitch detection & Elimination of dynamic allocations.

## 🔒 My Identity
- Archetype: explorer
- Roles: investigation, design synthesis, architectural blueprinting
- Working directory: f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m1_2/
- Original parent: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Milestone: Milestone 1

## 🔒 Key Constraints
- Read-only investigation — do NOT implement
- Do NOT modify source code files
- Deliver handoff report with 5 components to f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m1_2/handoff.md
- Keep BRIEFING.md updated and progress.md heartbeat

## Current Parent
- Conversation ID: 6d04231a-d33e-4b26-b49d-7f9feca2b265
- Updated: 2026-10-06T18:43:00Z

## Investigation State
- **Explored paths**:
  - `src/tools/tuner.h`, `src/tools/tuner.cpp`
  - `src/main.cpp` (audio callback, tuner setup, routing, buffer allocation)
  - `tests/test_praccy.cpp` (pitch detection unit test)
  - `src/ui/rack_view.cpp` (tuner UI rendering)
  - `CMakeLists.txt` (C++20 standard, build targets)
  - `third_party/readerwriterqueue/`
- **Key findings**:
  - Audio callback currently runs $O(N^2)$ YIN pitch detection (over 1M loop operations per callback!) inside `asio.setAudioCallback`, taking up to 35-40% of the audio budget.
  - Callback performs dynamic allocation via `static thread_local std::vector<float> s_tunerMixBuf; s_tunerMixBuf.resize(in.numSamples())` when in stereo mode.
  - Tuner result reporting in `tuner.cpp` uses 4 relaxed atomics, subject to compiler/CPU reordering and torn cross-field updates during pitch changes.
  - Solution: Dedicated SPSC float ring buffer + 60Hz worker thread sleeping via cv with instant shutdown + dual mono/stereo `pushSamples` + atomic result snapshotting via seqlock/acquire-release + pre-allocated buffers in `prepare(sampleRate, maxBlockSize)`.
- **Unexplored areas**: None for Features 2 & 3.

## Key Decisions Made
- Design a high-performance power-of-2 SPSC ring buffer for floats (`AudioRingBuffer`) embedded in `InstrumentTuner`.
- Overload `pushSamples(const float* mono, uint32_t numSamples)` and `pushSamples(const float* left, const float* right, uint32_t numSamples)` to eliminate `s_tunerMixBuf` in `main.cpp` completely.
- Keep `process()` synchronous for deterministic backward compatibility with `test_praccy.cpp`, while transitioning `main.cpp` to non-blocking `pushSamples()`.
- Use a 60 Hz worker thread with `std::condition_variable::wait_for` (not touched by audio thread) for 0% idle CPU and instant shutdown.
- Use sequence counter (seqlock) + acquire/release atomics for wait-free atomic snapshotting in `currentResult()`.

## Artifact Index
- DISPATCH.md — Initial dispatch instructions
- BRIEFING.md — Working memory and context
- progress.md — Liveness heartbeat and milestone tracker
- handoff.md — Final 5-component handoff report
