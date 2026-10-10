# E2E Test Infra: Praccy v2.0 Architectural Blueprint

## Test Philosophy
- Opaque-box, requirement-driven testing validating real-time DSP, SecOps crash isolation, UI theming, and practice suite ergonomics.
- Methodology: Category-Partition + Boundary Value Analysis + Concurrency Stress Testing + Headless Audio Stream Verification.

## Acceptance Criteria Mapping
1. `GraphEngineTest.ConcurrentParallelMutation`: Concurrency test under high-frequency audio pump with continuous slot additions and removals.
2. `AsioManagerTest.Format24BitUnpack`: Bit-exact unpacking and packing verification for `ASIOSTInt24LSB` and `ASIOSTInt32LSB24`.
3. `TunerIdleDspLoad`: Verification of tuner background execution, zero heap allocation in callback, and load < 5%.
4. `SecOpsStaticInspection`: String inspection asserting zero `std::system`, `cmd.exe`, `powershell.exe` in binary/updater code.
5. `CorruptedPresetsResilience`: Safe, non-throwing parsing of corrupted `presets.ini` with `std::from_chars`.
6. `PluginCrashIsolation`: Hardware access violation isolation inside hosted plugin, preventing host crash and bypassing gracefully.
7. `CompilerWarningsCleanliness`: Build with warnings-as-errors (`/W4 /WX` on MSVC, `-Wall -Wextra -Werror` on GCC).
8. `HardcodedColorsZero`: `scripts/check_hardcoded_colors.py` verifies zero undeclared `IM_COL32` in `src/ui/`.
9. `ThemeContrastWcagAA`: Contrast ratio >= 4.5:1 across all 4 themes.
10. `HeadlessAudioRegression`: Headless harness booting Praccy with mock ASIO driver (`test_asio_driver.cpp`), pumping 20,000 blocks with random bypass toggles, zero memory corruption, zero NaN/Inf, zero deadlocks.

## Test Targets
- `test_praccy.exe`: Comprehensive unit & integration tests (`tests/test_praccy.cpp`).
- `test_asio_driver.exe`: Headless mock ASIO driver regression harness (`tests/test_asio_driver.cpp`).
- `python scripts/check_hardcoded_colors.py`: Design token compliance.
- Static regex audit: SecOps string compliance.
