## 2026-10-07T17:14:29Z

You are the Remediation Worker for Milestone 4 of the Praccy v2.0 Architectural Blueprint project.
Your working directory is f:/Projects/Praccy/.agents/teamwork/teamwork_preview_worker_m4_fix/.
Project root is f:/Projects/Praccy.

MANDATORY INTEGRITY WARNING:
DO NOT CHEAT. All implementations must be genuine. DO NOT hardcode test results, create dummy/facade implementations, or circumvent the intended task. A teamwork_preview_auditor will independently verify your work. Integrity violations WILL be detected and your work WILL be rejected.

You MUST read these foundational and audit review documents before starting work:
1. f:/Projects/Praccy/.agents/teamwork/ORIGINAL_REQUEST.md
2. f:/Projects/Praccy/.agents/teamwork/orchestrator/PROJECT.md
3. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_challenger_m4_2/handoff.md (Details the 4 vulnerabilities: looper bad_alloc, prepare sampleRate sanitization, WM_DROPFILES MAX_PATH limit, toast NaN handling)
4. f:/Projects/Praccy/.agents/teamwork/teamwork_preview_challenger_m4_1/handoff.md (Details fuzzy search ranking inversion and partial subsequence matching)

Exclusive Write Ownership:
- src/tools/quick_looper.h
- src/tools/quick_looper.cpp
- src/main.cpp
- src/ui/ui_helpers.h
- src/ui/modals/plugin_browser_modal.cpp
- src/audio/graph_engine.cpp
- tests/test_praccy.cpp

Remediation Tasks:

1. Safeguard Memory Allocation & Sanitize RIFF in `src/tools/quick_looper.cpp`:
   - In `QuickLooper::loadWavFile()`:
     - Check `chunk.size`: ensure `chunk.size <= 256 * 1024 * 1024` (256 MB maximum for looper audio data) and check against remaining file size in stream (`file.seekg(0, std::ios::end); auto fileSize = file.tellg();`). If `chunk.size` exceeds remaining bytes or maximum allowed size, close file and return `false`.
     - Wrap buffer allocations in `try { ... } catch (const std::bad_alloc&) { return false; }` so corrupted WAV files never cause unhandled exception crashes.
   - In `QuickLooper::prepare()`:
     - Sanitize `sampleRate` and `maxBlockSize`:
       `if (!std::isfinite(sampleRate) || sampleRate <= 0.0) { sampleRate = 48000.0; }`
       `if (maxBlockSize == 0 || maxBlockSize > 65536) { maxBlockSize = 512; }`
     - Prevents negative/zero/NaN sampleRate from causing `size_t` underflow or `std::length_error`.

2. Dynamic Path Length for `WM_DROPFILES` in `src/main.cpp`:
   - In `WndProc` under `WM_DROPFILES`:
     - Replace fixed `wchar_t filePathW[MAX_PATH]` with dynamic length query:
       ```cpp
       UINT pathLen = DragQueryFileW(hDrop, i, nullptr, 0);
       if (pathLen > 0) {
           std::wstring filePathW(pathLen + 1, L'\0');
           DragQueryFileW(hDrop, i, filePathW.data(), pathLen + 1);
           filePathW.resize(pathLen);
           int utf8Len = WideCharToMultiByte(CP_UTF8, 0, filePathW.c_str(), -1, nullptr, 0, nullptr, nullptr);
           if (utf8Len > 0) {
               std::string filePathUtf8(utf8Len - 1, '\0');
               WideCharToMultiByte(CP_UTF8, 0, filePathW.c_str(), -1, filePathUtf8.data(), utf8Len, nullptr, nullptr);
               // validate extension case-insensitively and load
           }
       }
       ```
     - Fully supports paths longer than `MAX_PATH` (260 characters).

3. Sanitize `computeHudToastAlpha()` in `src/ui/ui_helpers.h`:
   - In `computeHudToastAlpha(float elapsedSeconds, float durationSeconds = 1.8f)`:
     ```cpp
     if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0f) {
         return (elapsedSeconds < 0.0f) ? 1.0f : 0.0f;
     }
     if (!std::isfinite(durationSeconds) || durationSeconds <= 0.0f) return 0.0f;
     if (elapsedSeconds >= durationSeconds) return 0.0f;
     return 1.0f - (elapsedSeconds / durationSeconds);
     ```
     Guarantees return value is always finite and bounded in `[0.0f, 1.0f]`.

4. Fix Fuzzy Ranking & Subsequence Match in `src/ui/modals/plugin_browser_modal.cpp`:
   - In `PluginBrowserModal::calculateFuzzyScore(const std::string& query, const std::string& target)`:
     - Make exact match strictly the highest score:
       `if (q == t) return 2000;` (so prefix matches on queries >= 10 characters never outrank exact matches).
     - In the subsequence matching loop, only award subsequence points if the entire query was found (`qIdx == q.length()`):
       ```cpp
       if (qIdx == q.length()) {
           score = std::max(score, subseqScore);
       }
       ```
       Prevents false-positive matches on disjoint partial character fragments.

5. Prepare Incoming Nodes in `src/audio/graph_engine.cpp`:
   - In `crossfadeToNodes`, ensure incoming nodes are prepared with current sample rate and block size before crossfading commences if not already prepared.

6. Build and Verify:
   - Run `python scripts/check_hardcoded_colors.py` (assert 0 violations).
   - Build all targets: `cmake --build build --target test_praccy Praccy test_challenger_m4_1 test_challenger_m4_2`.
   - Run `.\build\test_praccy.exe` (assert 28/28 pass).
   - Run `.\build\test_challenger_m4_1.exe` and `.\build\test_challenger_m4_2.exe` (assert all tests pass).
   - Run `ctest --test-dir build --output-on-failure` (assert 100% pass across all 9 test suites).
   - Ensure clean compilation with zero warnings under `-Wall -Wextra -Werror` / `/W4 /WX`.
