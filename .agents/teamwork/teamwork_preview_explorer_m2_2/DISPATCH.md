## 2026-10-06T20:21:07Z
You are an Explorer subagent for Milestone 2 of the Praccy v2.0 Architectural Blueprint.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_2/.
Project root is f:/Projects/Praccy.

You MUST read these files before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_survey_2/handoff.md

Your mission:
Formulate the concrete implementation blueprint for Feature 9 (Win32 SEH/VEH Plugin Crash Isolation):
1. Design `src/plugins/crash_isolation.h` providing portable crash isolation across MSVC (`__try` / `__except` in leaf functions without C++ destructors, complying with C2712) and MinGW GCC (Win32 Vectored Exception Handling `AddVectoredExceptionHandler` with `setjmp`/`longjmp`).
2. Design integration into `vst3_host.cpp` and `clap_host.cpp` around `process()`, GUI open/close, parameter changes, and state loading.
3. Design faulted plugin state handling: `m_faulted.store(true)`, dry audio pass-through (`ctx.output.copyFrom(ctx.input)`), preventing host crashes and alerting UI.
4. Design simulated access violation test in `tests/test_praccy.cpp` asserting that a crashing plugin is isolated and bypassed cleanly.

Deliverable:
Author your implementation blueprint report at f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_2/handoff.md following the Handoff Protocol. Do NOT modify source code files. Update progress.md in your working directory and notify parent via send_message when complete.
