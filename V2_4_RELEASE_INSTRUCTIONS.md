# Directive: Finalize and Complete Praccy Version 2.4 Release

## Context & Baseline
Praccy V2.4 Stage 1 (Musician Velocity & Pro-Audio Workflow Engine) is implemented and independently verified in commit `bab8504`:
- Lock-free `ParamAutomationRegister` and dynamic Parallel Delay Compensation (PDC)
- Retroactive Riff Recording with lock-free atomic slice latching & 24-bit WAV export
- Transactional `UndoManager` (64 levels, 400ms coalescing) & A/B Tone Snapshot toggle
- Card mini-knob macros, drag-and-drop reordering, and map mode with `!WantTextInput` guards
- `ImGuiListClipper` virtualization and DIN IEC 60268-10 PPM ballistics
- 52/52 tests passing in `test_praccy_v2_1.exe` and 11/11 passing in CTest

Complete the Version 2.4 delivery by executing the following 4 phases:

---

### Phase 1: Version Metadata & Documentation Alignment
1. **CMakeLists.txt**:
   - Update `project(Praccy VERSION 2.4.0 LANGUAGES CXX C RC)`.
2. **Version String Standardization**:
   - Centralize current version reporting to `"v2.4.0"` across:
     - `src/ui/update_checker.h` (`currentVersion{"v2.4.0"}`)
     - `src/ui/update_checker.cpp` (`result.currentVersion = "v2.4.0"`)
     - `src/ui/modals/settings_modal.cpp` (replace hardcoded `"v2.0.0"` with `"v2.4.0"` or pull dynamically from `UpdateChecker::instance().getInfo().currentVersion`)
     - `src/ui/rack_view.cpp` (replace hardcoded `"v1.1.3"` with `"v2.4.0"`).
3. **Windows Binary Metadata (`resources/praccy.rc`)**:
   - Add a standard `VS_VERSION_INFO` resource block with:
     - `FILEVERSION 2,4,0,0`
     - `PRODUCTVERSION 2,4,0,0`
     - Product Name: `"Praccy"`
     - File Description: `"Praccy - ASIO VST3 & CLAP Practice Suite"`
     - Product Version: `"2.4.0"`
4. **README.md**:
   - Update the badge, features list, and documentation to reflect V2.4 additions: Parallel Delay Compensation (PDC), Retroactive Riff Capture (`Ctrl+Shift+R`), Transactional Undo/Redo (`Ctrl+Z`/`Ctrl+Y`), Card Mini-Knob Macros, ImGui Drag-and-Drop Reordering, and Spotlight Virtualization.

---

### Phase 2: Runtime Edge-Case & Packaging Hardening
1. **Directory Auto-Creation**:
   - Ensure the Retroactive Riff destination `%USERPROFILE%\Documents\Praccy\Riffs\` and AppConfig directory are created safely with `std::filesystem::create_directories()` if they do not yet exist prior to first WAV export.
2. **Real-Time Safety Audit**:
   - Confirm strictly 0 heap allocations, 0 blocking file I/O, and 0 mutex acquisitions on the ASIO audio callback thread during card dragging, slot swapping, undo/redo state restoration, and retroactive riff buffer pushing.
3. **Input Guarding**:
   - Confirm all global shortcuts (`Ctrl+Z`, `Ctrl+Y`, `Ctrl+Shift+R`, `Ctrl+M`, `Ctrl+K`, `[`, `]`, `Q`, `0`) remain strictly bypassed whenever `ImGui::GetIO().WantTextInput` is true.

---

### Phase 3: Release Build & Distribution Staging
1. Perform a clean Release build:
   ```powershell
   cmake -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build --config Release
   ```
2. Create a clean standalone ZIP distribution archive `dist/Praccy-v2.4.0-win64.zip` containing:
   - `build/Praccy.exe`
   - `resources/` (fonts, icons, thumbnails)
   - `README.md`
   - `LICENSE`

---

### Phase 4: Full Verification Gate (All Must Pass Cleanly)
Run the verification suite and report the exact results:
1. **CTest Suite**: `ctest --test-dir build --output-on-failure` (Must pass 11/11 targets).
2. **Stage 1 Harness**: `.\build\test_praccy_v2_1.exe` (Must pass 52/52 assertions).
3. **Headless ASIO Stress**: `.\build\test_asio_driver.exe` (20,000 blocks, 0 audio thread allocations).
4. **Token Purity**: `py scripts/check_hardcoded_colors.py` (0 raw color violations in `src/ui`).
5. **WCAG Compliance**: `py scripts/wcag_audit.py` (100% WCAG 2.1 AA text contrast across all 4 themes).
6. **Git Status**: Clean working tree and commit changes with message:
   `feat(release): finalize version 2.4.0 release build and metadata alignment`
