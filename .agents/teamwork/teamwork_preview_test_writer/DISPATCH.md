## 2026-10-12T18:11:34Z
You are the E2E Test Suite Author for Praccy V2.4 Stage 1.
Your working directory is F:\Projects\Praccy\.agents\teamwork\teamwork_preview_test_writer.

MANDATORY: Read the authoritative user request at:
F:\Projects\Praccy\.agents\teamwork\ORIGINAL_REQUEST.md
Also read the project architecture and interface contracts at:
F:\Projects\Praccy\.agents\teamwork\PROJECT.md
And the test harness specification report at:
F:\Projects\Praccy\.agents\teamwork\spec_miner_survey_infra\survey_report.md

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

Your write ownership (exclusive):
- tests/test_praccy_v2_1.cpp
- TEST_INFRA.md (at project root)
- TEST_READY.md (at project root)
- files in your working directory

Objective:
Author the comprehensive Stage 1 test harness `tests/test_praccy_v2_1.cpp` using the 4-tier methodology (Tier 1 Feature Coverage, Tier 2 Boundary & Corner Cases, Tier 3 Cross-Feature Interactions, Tier 4 Real-World Scenarios) covering R1 through R6:
- Module 1: R1 Real-Time Audio DSP Safety, SPSC Automation & PDC (ParamAutomationRegister atomic bitmask & drain, 16,384 sample PDC phase coherence, 64-sample equal-power latency crossfade energy, SPSC block-boundary slot swaps).
- Module 2: R2 Configurable Retroactive Riff Capture & In-Process WAV Export (pre-allocated circular buffer 1/2/5/10 min, <0.15ms slice copy, header-only 24-bit WAV encoding, RIFF header validation).
- Module 3: R3 Transactional Undo/Redo & State Management (64-level history ring buffer, 400ms idle coalescing, mouse-release commits, A/B Tone Snapshot toggle with 10ms crossfade).
- Module 4: R4 Front-Panel Macros & Rack Ergonomics (4 mini-knobs fuzzy regex heuristics, card drag-and-drop physics, global map mode, hotkey text input guard `!WantTextInput`, focused card shortcuts).
- Module 5: R5 Visual Polish, List Virtualization & Meters (Spotlight ImGuiListClipper virtualization, footer DPI collision avoidance, Favorites scope boost, DIN IEC 60268-10 PPM ballistics).
- Module 6: R6 Shared Infrastructure (archive_utils decompression bomb guards: 30MB XML, 128MB archive, 100:1 ratio, 100 files, path sanitization; DragDropRouter audio/preset/chain classification; flat O(1) MIDI CC lookup table).

Follow the `TEST_INFRA.md` template from Project Pattern and author `TEST_INFRA.md` and `TEST_READY.md`.
Document the test commands and test coverage in your `handoff.md`.
When complete, notify your orchestrator via send_message.
