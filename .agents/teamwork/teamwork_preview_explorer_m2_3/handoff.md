# Architectural Implementation Blueprint: Features 10, 11, 12 & 13
## Non-Throwing Parsing, Window Placement Persistence, Warning Hardening & Resilience Unit Testing

**Document Identifier**: `PRAC-2026-M2-BP-F10-13`  
**Agent**: `teamwork_preview_explorer_m2_3` (Milestone 2 Explorer Subagent)  
**Parent Orchestrator**: `6d04231a-d33e-4b26-b49d-7f9feca2b265`  
**Working Directory**: `f:/Projects/Praccy/.agents/teamwork/teamwork_preview_explorer_m2_3/`  
**Target Subsystems**:
- Feature 10: Non-Throwing Parsing (`src/utils/parse_utils.h`, `src/state/scene_manager.cpp`, `src/state/app_config.cpp`)
- Feature 11: Window Placement Persistence (`src/state/app_config.h`, `src/state/app_config.cpp`, `src/main.cpp`)
- Feature 12: Compiler Warning Hardening (`CMakeLists.txt`, `src/audio/asio_manager.cpp`, `src/plugins/vst3_host.cpp`, `src/plugins/plugin_scanner.cpp`)
- Feature 13: Corrupted Preset & Config Unit Test Harness (`tests/test_praccy.cpp`)

---

## 1. Observation

### 1.1 Direct Codebase Audit: Throwing & Unchecked String-to-Numeric Conversions (Feature 10)
A forensic search across the codebase revealed multiple unguarded calls to throwing C++ standard library functions (`std::stoul`, `std::stof`) and legacy C library functions with undefined overflow behavior (`std::atoi`, `std::atof`):

1. **`src/state/scene_manager.cpp`**:
   - **Line 28 (`hexToBytes`)**:
     ```cpp
     uint8_t b = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
     ```
     *Observed Vulnerability*: If `hex` contains non-hex characters (e.g. `"ZZ"`, `"?!"`), `std::stoul` throws `std::invalid_argument`. If the number exceeds `unsigned long`, it throws `std::out_of_range`. Neither exception is caught.
   - **Line 540 (`loadFromFile`)**:
     ```cpp
     size_t count = (itNodes != kv.end()) ? std::stoul(itNodes->second) : 0;
     ```
     *Observed Vulnerability*: If `numNodes=` contains invalid text (e.g. `numNodes=abc`), `std::stoul` throws unhandled `std::invalid_argument`, terminating Praccy immediately upon boot.
   - **Lines 552–554 (`loadFromFile`)**:
     ```cpp
     np.slot.dryWet = kv.count(pfx + "dryWet") ? std::stof(kv.at(pfx + "dryWet")) : 1.0f;
     np.slot.inputGainDb = kv.count(pfx + "inGain") ? std::stof(kv.at(pfx + "inGain")) : 0.0f;
     np.slot.outputGainDb = kv.count(pfx + "outGain") ? std::stof(kv.at(pfx + "outGain")) : 0.0f;
     ```
     *Observed Vulnerability*: Malformed floating-point values in `presets.ini` trigger unhandled `std::invalid_argument` or `std::out_of_range`.
   - **Line 560**: `std::stoul(kv.at(pfx + "numBranches"))`
   - **Lines 565–566**: `std::stof(kv.at(bpfx + "gain"))`, `std::stof(kv.at(bpfx + "pan"))`
   - **Line 571**: `std::stoul(kv.at(bpfx + "numSlots"))`
   - **Lines 579–581**: `std::stof(kv.at(spfx + "dryWet"))`, `std::stof(kv.at(spfx + "inGain"))`, `std::stof(kv.at(spfx + "outGain"))`

2. **`src/state/app_config.cpp`**:
   - **Line 49 (`load`)**:
     ```cpp
     inputMode = static_cast<audio::InputRoutingMode>(std::atoi(val.c_str()));
     ```
   - **Lines 51, 53, 55 (`load`)**:
     ```cpp
     inputGainDb = static_cast<float>(std::atof(val.c_str()));
     masterVolumeDb = static_cast<float>(std::atof(val.c_str()));
     metronomeBpm = static_cast<float>(std::atof(val.c_str()));
     ```
     *Observed Vulnerability*: `std::atoi` and `std::atof` return `0` indistinguishably on errors, exhibit undefined behavior on overflow, and `std::atof` depends on C runtime locale (`.` vs `,`).

### 1.2 Direct Codebase Audit: Window Placement & Geometry Persistence (Feature 11)
Inspection of `src/main.cpp`, `src/state/app_config.h`, and `src/state/app_config.cpp` revealed:
1. **Hardcoded Window Creation** (`src/main.cpp:60–66`):
   ```cpp
   HWND hwnd = CreateWindowW(
       wc.lpszClassName,
       L"Praccy - ASIO VST3/CLAP Practice Host",
       WS_OVERLAPPEDWINDOW,
       100, 100, 1280, 720,
       nullptr, nullptr, wc.hInstance, nullptr
   );
   ```
   Window coordinates `(100, 100)` and dimensions `(1280, 720)` are fixed constants.
2. **Missing Configuration Fields** (`src/state/app_config.h:9–23`):
   No data members exist for window rectangle coordinates, size, or maximized state.
3. **Hardcoded Show Command** (`src/main.cpp:286`):
   Always calls `ShowWindow(hwnd, SW_SHOWDEFAULT);`, disregarding previously maximized window state.
4. **Multi-Monitor Edge Case**:
   If an external monitor is disconnected, restoring raw coordinates without Win32 `MonitorFromRect` validation leaves the window completely off-screen and invisible to the user.

### 1.3 Direct Codebase Audit: Compiler Warning Hardening (Feature 12)
1. **Build Configuration** (`CMakeLists.txt:85–89`):
   ```cmake
   if(MSVC)
       target_compile_options(Praccy PRIVATE /O2 /W4 /permissive-)
   else()
       target_compile_options(Praccy PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter)
   endif()
   ```
   - MSVC is missing `/WX` (warnings as errors).
   - GCC/Clang is missing `-Werror`.
   - Test targets (`test_praccy`, `test_asio_driver`, `test_challenger_m1`, `test_challenger_m1_2`) do NOT specify compile options.
   - Third-party include directories in `target_include_directories` use `PRIVATE` instead of `SYSTEM PRIVATE`.
2. **Latent First-Party Warnings Discovered Under Strict Compilation (`-Wall -Wextra -Werror`)**:
   - `src/audio/asio_manager.cpp:48`:
     ```
     error: 'char* strncpy(char*, const char*, size_t)' output may be truncated copying 255 bytes from a string of length 255 [-Werror=stringop-truncation]
     ```
   - `src/plugins/vst3_host.cpp:176, 181, 317`:
     ```
     error: cast between incompatible function types from 'FARPROC' to 'praccy::plugins::InitDllProc' / 'GetFactoryProc' / 'ExitDllProc' [-Werror=cast-function-type]
     ```
   - `src/plugins/plugin_scanner.cpp:265`:
     ```
     error: cast between incompatible function types from 'FARPROC' to 'praccy::plugins::GetFactoryProc' [-Werror=cast-function-type]
     ```
3. **Latent Third-Party Warnings Discovered**:
   - `third_party/vst3_pluginterfaces/base/funknown.cpp:301, 321, 354, 390`:
     ```
     error: format '%X' expects argument of type 'unsigned int', but argument 3 has type 'long unsigned int' [-Werror=format=]
     ```
4. **Empirical Verification of Clean First-Party Translation Units**:
   Executing test compilation across all other first-party files (`main.cpp`, `graph_engine.cpp`, `plugin_window.cpp`, `builtin_dsp.cpp`, `midi_manager.cpp`, `tuner.cpp`, `metronome.cpp`, `audio_player.cpp`, `quick_looper.cpp`, `scene_manager.cpp`, `app_config.cpp`, `rack_view.cpp`, `thumbnail_manager.cpp`, `update_checker.cpp`, `test_praccy.cpp`) confirmed **100% clean compilation** with zero warnings under `-O3 -Wall -Wextra -Werror -Wno-unused-parameter`.

### 1.4 Direct Codebase Audit: Test Suite Coverage (Feature 13)
- `tests/test_praccy.cpp:184–220`: `testSceneManager()` tests only in-memory capture and recall; it never invokes `loadFromFile()` or tests corrupted ini inputs.
- No unit test currently validates recovery from malformed preset or config files.

---

## 2. Logic Chain

### 2.1 Feature 10: Designing `src/utils/parse_utils.h` and Call-Site Refactoring
1. *From Observation 1.1*: `std::stoul` and `std::stof` throw unhandled exceptions upon malformed or truncated text.
2. C++20 `<charconv>` provides `std::from_chars`, which is guaranteed non-allocating, non-throwing, locale-independent, and returns error codes via `std::from_chars_result { const char* ptr, std::errc ec }`.
3. *Critical C++20 Nuance*: By ISO C++ specification `[charconv.from.chars]`, `std::from_chars` does **NOT** accept leading whitespace, nor does it recognize a leading `'+'` sign. If a user or preset writes `" 1.5 "` or `"+6.0"`, raw `std::from_chars` returns `std::errc::invalid_argument`.
4. Therefore, `parse_utils.h` must:
   - Trim leading and trailing whitespace (`' '`, `'\t'`, `'\r'`, `'\n'`).
   - Strip an optional leading `'+'` before passing the buffer slice to `std::from_chars`.
   - Validate that parsing consumed the entire valid slice (`ptr == sv.data() + sv.size()`).
   - Return a caller-specified `defaultValue` if `ec != std::errc()` or if the string is empty.
5. In `scene_manager.cpp`:
   - Replace `hexToBytes` with `praccy::utils::hexToBytes`. If an odd length or invalid hex character occurs, safely return an empty vector `{}` instead of crashing.
   - Replace all 12 throwing `std::stoul` / `std::stof` call sites with `parseInteger<size_t>` and `parseFloat`.
6. In `app_config.cpp`:
   - Replace `std::atoi` and `std::atof` with `parseInteger<int>` and `parseFloat`.

### 2.2 Feature 11: Multi-Monitor Window Placement Persistence
1. *From Observation 1.2*: Praccy currently boots at a static `(100, 100, 1280, 720)`. Closing the application while maximized or resizing loses user preferences.
2. In Win32, calling `GetWindowRect` while maximized captures the maximized display bounds rather than the restored geometry. Win32 `GetWindowPlacement` and `SetWindowPlacement` decouple the restored rectangle (`rcNormalPosition`) from the display command (`showCmd == SW_SHOWMAXIMIZED`).
3. Furthermore, when closing a minimized application, `wp.showCmd == SW_SHOWMINIMIZED`. To avoid launching in a minimized state, the saved state must map minimized to `SW_SHOWNORMAL`.
4. *Multi-Monitor Safety*: If a multi-monitor display is disconnected, the saved `rcNormalPosition` may reside in virtual space with no physical monitor. Passing this to `SetWindowPlacement` renders the window completely invisible.
5. Checking `MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)` detects when the target rectangle has zero monitor overlap. When `null`, the geometry must fall back to the primary monitor workspace (`GetMonitorInfoW` on `MONITOR_DEFAULTTOPRIMARY`).
6. In `src/main.cpp`:
   - To prevent white window flash, Praccy pre-renders the first frame to the DX11 swapchain before displaying the window.
   - Calling `appConfig.restoreWindowPlacement(hwnd)` right after pre-rendering and before the main message loop displays the window in its correct restored or maximized state with zero visual glitch.
   - On shutdown (or `WM_CLOSE`), `appConfig.saveWindowPlacement(hwnd)` captures the latest state into `config.ini`.

### 2.3 Feature 12: Warning Hardening (`/W4 /WX`, `-Wall -Wextra -Werror`)
1. *From Observation 1.3*: Missing `/WX` on MSVC and `-Werror` on GCC allows warnings to accumulate.
2. Third-party headers in `third_party/` trigger warnings if compiled under strict flags without `SYSTEM` classification. In CMake, `target_include_directories(... SYSTEM PRIVATE ...)` generates `-isystem` on GCC/Clang and `/external:I` on MSVC, suppressing third-party header warnings.
3. Third-party translation units (`funknown.cpp`) trigger `-Wformat` warnings. In CMake, attaching `COMPILE_OPTIONS "-w"` (GCC) and `"/W0"` (MSVC) via `set_source_files_properties` isolates third-party source files completely.
4. *Latent First-Party Warning Resolution*:
   - `asio_manager.cpp:48`: `strncpy(descStr, subKeyName, sizeof(descStr) - 1)` triggers `-Wstringop-truncation`. Replacing with `std::snprintf(descStr, sizeof(descStr), "%s", subKeyName)` is standards-compliant, guarantees null-termination, and produces zero warnings.
   - `vst3_host.cpp:176, 181, 317` & `plugin_scanner.cpp:265`: `reinterpret_cast<TargetFunc>(GetProcAddress(...))` triggers `-Wcast-function-type`. Casting through `void*` (`reinterpret_cast<TargetFunc>(reinterpret_cast<void*>(GetProcAddress(...)))`) is the canonical Win32 pattern recognized by GCC and MSVC to cleanly suppress this warning without pragmas.

### 2.4 Feature 13: Corrupted Preset & Config Unit Test Harness
1. *From Observation 1.4*: The test suite lacks verification against malformed INI files.
2. Designing `testCorruptedPresetsIni()` in `tests/test_praccy.cpp` creates temporary test files exercising:
   - Malformed numbers (`numNodes=broken`, `dryWet=NaN`, out-of-range integer strings).
   - Malformed hex bytes (`state=012` odd length, `state=A5ZZ` non-hex).
   - Mangled INI syntax (lines without `=`, unclosed section brackets, truncated keys).
   - Completely empty / 0-byte file (asserting fallback to default clean/crunch/lead/ambient scenes).
   - Explicit `+` signs and whitespace padding.
3. Asserting that `SceneManager::loadFromFile` and `AppConfig::load` complete with return code `true`, zero unhandled exceptions, and valid default fallback data verifies Acceptance Criteria 50.

---

## 3. Concrete Architectural Blueprint

### 3.1 Feature 10: `src/utils/parse_utils.h`
Create new file `src/utils/parse_utils.h`:

```cpp
#pragma once

#include <charconv>
#include <string_view>
#include <vector>
#include <cstdint>
#include <type_traits>

namespace praccy::utils {

/// Strips leading and trailing whitespace characters (' ', '\t', '\r', '\n')
inline std::string_view trim(std::string_view sv) noexcept {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

/// Non-throwing integer parser using C++20 std::from_chars with safe fallback default
template <typename T>
inline T parseInteger(std::string_view sv, T defaultValue = T{}, int base = 10) noexcept {
    static_assert(std::is_integral_v<T>, "parseInteger requires an integral type");
    sv = trim(sv);
    if (sv.empty()) return defaultValue;

    // std::from_chars does not accept leading '+' sign by ISO standard
    if (sv.front() == '+') {
        sv.remove_prefix(1);
        if (sv.empty()) return defaultValue;
    }

    T result = defaultValue;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result, base);
    if (ec == std::errc() && ptr == (sv.data() + sv.size())) {
        return result;
    }
    return defaultValue;
}

/// Non-throwing float parser using C++20 std::from_chars with safe fallback default
inline float parseFloat(std::string_view sv, float defaultValue = 0.0f) noexcept {
    sv = trim(sv);
    if (sv.empty()) return defaultValue;

    if (sv.front() == '+') {
        sv.remove_prefix(1);
        if (sv.empty()) return defaultValue;
    }

    float result = defaultValue;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (ec == std::errc() && ptr == (sv.data() + sv.size())) {
        return result;
    }
    return defaultValue;
}

/// Non-throwing double parser using C++20 std::from_chars with safe fallback default
inline double parseDouble(std::string_view sv, double defaultValue = 0.0) noexcept {
    sv = trim(sv);
    if (sv.empty()) return defaultValue;

    if (sv.front() == '+') {
        sv.remove_prefix(1);
        if (sv.empty()) return defaultValue;
    }

    double result = defaultValue;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (ec == std::errc() && ptr == (sv.data() + sv.size())) {
        return result;
    }
    return defaultValue;
}

/// Non-throwing 2-character hex byte parser (e.g. "A5" -> 0xA5)
inline bool parseHexByte(std::string_view sv, uint8_t& outByte) noexcept {
    if (sv.size() != 2) return false;
    uint8_t val = 0;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + 2, val, 16);
    if (ec == std::errc() && ptr == (sv.data() + 2)) {
        outByte = val;
        return true;
    }
    return false;
}

/// Non-throwing hex string to byte vector decoder
/// Returns empty vector on odd length or corrupted non-hex characters
inline std::vector<uint8_t> hexToBytes(std::string_view hex) {
    hex = trim(hex);
    std::vector<uint8_t> bytes;
    if (hex.length() % 2 != 0) return bytes;
    bytes.reserve(hex.length() / 2);
    for (size_t i = 0; i < hex.length(); i += 2) {
        uint8_t b = 0;
        if (!parseHexByte(hex.substr(i, 2), b)) {
            bytes.clear(); // Corrupted hex stream, fail safe
            return bytes;
        }
        bytes.push_back(b);
    }
    return bytes;
}

} // namespace praccy::utils
```

### 3.2 Feature 10 Call-Site Integrations

#### Refactoring `src/state/scene_manager.cpp`:
1. Include the new header:
   ```cpp
   #include "../utils/parse_utils.h"
   ```
2. Replace lines 23–32:
   ```cpp
   // BEFORE:
   static std::vector<uint8_t> hexToBytes(const std::string& hex) {
       std::vector<uint8_t> bytes;
       if (hex.length() % 2 != 0) return bytes;
       bytes.reserve(hex.length() / 2);
       for (size_t i = 0; i < hex.length(); i += 2) {
           uint8_t b = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
           bytes.push_back(b);
       }
       return bytes;
   }

   // AFTER:
   static std::vector<uint8_t> hexToBytes(const std::string& hex) {
       return praccy::utils::hexToBytes(hex);
   }
   ```
3. Replace line 540:
   ```cpp
   // BEFORE:
   size_t count = (itNodes != kv.end()) ? std::stoul(itNodes->second) : 0;
   // AFTER:
   size_t count = (itNodes != kv.end()) ? praccy::utils::parseInteger<size_t>(itNodes->second, 0) : 0;
   ```
4. Replace lines 552–554:
   ```cpp
   // BEFORE:
   np.slot.dryWet = kv.count(pfx + "dryWet") ? std::stof(kv.at(pfx + "dryWet")) : 1.0f;
   np.slot.inputGainDb = kv.count(pfx + "inGain") ? std::stof(kv.at(pfx + "inGain")) : 0.0f;
   np.slot.outputGainDb = kv.count(pfx + "outGain") ? std::stof(kv.at(pfx + "outGain")) : 0.0f;
   // AFTER:
   np.slot.dryWet = kv.count(pfx + "dryWet") ? praccy::utils::parseFloat(kv.at(pfx + "dryWet"), 1.0f) : 1.0f;
   np.slot.inputGainDb = kv.count(pfx + "inGain") ? praccy::utils::parseFloat(kv.at(pfx + "inGain"), 0.0f) : 0.0f;
   np.slot.outputGainDb = kv.count(pfx + "outGain") ? praccy::utils::parseFloat(kv.at(pfx + "outGain"), 0.0f) : 0.0f;
   ```
5. Replace line 560:
   ```cpp
   // BEFORE:
   size_t numBr = kv.count(pfx + "numBranches") ? std::stoul(kv.at(pfx + "numBranches")) : 0;
   // AFTER:
   size_t numBr = kv.count(pfx + "numBranches") ? praccy::utils::parseInteger<size_t>(kv.at(pfx + "numBranches"), 0) : 0;
   ```
6. Replace lines 565–566:
   ```cpp
   // BEFORE:
   bp.gainDb = kv.count(bpfx + "gain") ? std::stof(kv.at(bpfx + "gain")) : 0.0f;
   bp.pan = kv.count(bpfx + "pan") ? std::stof(kv.at(bpfx + "pan")) : 0.0f;
   // AFTER:
   bp.gainDb = kv.count(bpfx + "gain") ? praccy::utils::parseFloat(kv.at(bpfx + "gain"), 0.0f) : 0.0f;
   bp.pan = kv.count(bpfx + "pan") ? praccy::utils::parseFloat(kv.at(bpfx + "pan"), 0.0f) : 0.0f;
   ```
7. Replace line 571:
   ```cpp
   // BEFORE:
   size_t numSl = kv.count(bpfx + "numSlots") ? std::stoul(kv.at(bpfx + "numSlots")) : 0;
   // AFTER:
   size_t numSl = kv.count(bpfx + "numSlots") ? praccy::utils::parseInteger<size_t>(kv.at(bpfx + "numSlots"), 0) : 0;
   ```
8. Replace lines 579–581:
   ```cpp
   // BEFORE:
   sl.dryWet = kv.count(spfx + "dryWet") ? std::stof(kv.at(spfx + "dryWet")) : 1.0f;
   sl.inputGainDb = kv.count(spfx + "inGain") ? std::stof(kv.at(spfx + "inGain")) : 0.0f;
   sl.outputGainDb = kv.count(spfx + "outGain") ? std::stof(kv.at(spfx + "outGain")) : 0.0f;
   // AFTER:
   sl.dryWet = kv.count(spfx + "dryWet") ? praccy::utils::parseFloat(kv.at(spfx + "dryWet"), 1.0f) : 1.0f;
   sl.inputGainDb = kv.count(spfx + "inGain") ? praccy::utils::parseFloat(kv.at(spfx + "inGain"), 0.0f) : 0.0f;
   sl.outputGainDb = kv.count(spfx + "outGain") ? praccy::utils::parseFloat(kv.at(spfx + "outGain"), 0.0f) : 0.0f;
   ```

#### Refactoring `src/state/app_config.cpp`:
1. Include header:
   ```cpp
   #include "../utils/parse_utils.h"
   ```
2. Replace lines 48–56:
   ```cpp
   // BEFORE:
   } else if (key == "input_mode") {
       inputMode = static_cast<audio::InputRoutingMode>(std::atoi(val.c_str()));
   } else if (key == "input_gain_db") {
       inputGainDb = static_cast<float>(std::atof(val.c_str()));
   } else if (key == "master_volume_db") {
       masterVolumeDb = static_cast<float>(std::atof(val.c_str()));
   } else if (key == "metronome_bpm") {
       metronomeBpm = static_cast<float>(std::atof(val.c_str()));
   }

   // AFTER:
   } else if (key == "input_mode") {
       inputMode = static_cast<audio::InputRoutingMode>(
           utils::parseInteger<int>(val, static_cast<int>(audio::InputRoutingMode::MonoLeft)));
   } else if (key == "input_gain_db") {
       inputGainDb = utils::parseFloat(val, 0.0f);
   } else if (key == "master_volume_db") {
       masterVolumeDb = utils::parseFloat(val, 0.0f);
   } else if (key == "metronome_bpm") {
       metronomeBpm = utils::parseFloat(val, 120.0f);
   }
   ```

---

### 3.3 Feature 11: Window Placement Persistence

#### Additions to `src/state/app_config.h`:
```cpp
#pragma once

#include "../audio/input_config.h"
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace praccy::state {

struct AppConfig {
    std::string lastAsioDriver;
    audio::InputRoutingMode inputMode{audio::InputRoutingMode::MonoLeft};
    float inputGainDb{0.0f};
    float masterVolumeDb{0.0f};
    float metronomeBpm{120.0f};
    bool checkBetaUpdates{false};
    std::vector<std::string> customPluginPaths;
    std::vector<std::string> favoritePlugins;

    // Window geometry persistence
    int windowX{100};
    int windowY{100};
    int windowW{1280};
    int windowH{720};
    bool windowMaximized{false};

    static std::string getConfigDir();
    static std::string getConfigFilePath();
    bool load(const std::string& customPath = "");
    bool save(const std::string& customPath = "") const;

#if defined(_WIN32)
    void saveWindowPlacement(HWND hwnd);
    bool restoreWindowPlacement(HWND hwnd) const;
#endif
};

} // namespace praccy::state
```

#### Additions to `src/state/app_config.cpp`:
1. In `AppConfig::load`:
   ```cpp
   } else if (key == "window_x") {
       windowX = utils::parseInteger<int>(val, 100);
   } else if (key == "window_y") {
       windowY = utils::parseInteger<int>(val, 100);
   } else if (key == "window_w") {
       windowW = utils::parseInteger<int>(val, 1280);
   } else if (key == "window_h") {
       windowH = utils::parseInteger<int>(val, 720);
   } else if (key == "window_maximized") {
       windowMaximized = (val == "1" || val == "true");
   }
   ```
2. In `AppConfig::save`:
   ```cpp
   file << "window_x=" << windowX << "\n";
   file << "window_y=" << windowY << "\n";
   file << "window_w=" << windowW << "\n";
   file << "window_h=" << windowH << "\n";
   file << "window_maximized=" << (windowMaximized ? "1" : "0") << "\n";
   ```
3. Implement `saveWindowPlacement` and `restoreWindowPlacement`:
   ```cpp
   #if defined(_WIN32)
   void AppConfig::saveWindowPlacement(HWND hwnd) {
       if (!hwnd || !IsWindow(hwnd)) return;
       WINDOWPLACEMENT wp{};
       wp.length = sizeof(WINDOWPLACEMENT);
       if (GetWindowPlacement(hwnd, &wp)) {
           windowX = wp.rcNormalPosition.left;
           windowY = wp.rcNormalPosition.top;
           windowW = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
           windowH = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
           // If the application was minimized when closed, do not save as maximized
           windowMaximized = (wp.showCmd == SW_SHOWMAXIMIZED);
       }
   }

   bool AppConfig::restoreWindowPlacement(HWND hwnd) const {
       if (!hwnd || !IsWindow(hwnd)) return false;

       // 1. Sanitize minimum bounds
       int w = (windowW >= 640) ? windowW : 1280;
       int h = (windowH >= 480) ? windowH : 720;
       int x = windowX;
       int y = windowY;

       // 2. Validate multi-monitor bounds
       RECT rc{ x, y, x + w, y + h };
       HMONITOR hMon = MonitorFromRect(&rc, MONITOR_DEFAULTTONULL);
       if (!hMon) {
           // Window is off-screen (e.g. secondary monitor disconnected)
           // Fall back to primary monitor work area
           HMONITOR hPrimary = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
           MONITORINFO mi{};
           mi.cbSize = sizeof(MONITORINFO);
           if (hPrimary && GetMonitorInfoW(hPrimary, &mi)) {
               x = mi.rcWork.left + 50;
               y = mi.rcWork.top + 50;
               w = std::min(w, static_cast<int>(mi.rcWork.right - mi.rcWork.left - 100));
               h = std::min(h, static_cast<int>(mi.rcWork.bottom - mi.rcWork.top - 100));
           } else {
               x = 100;
               y = 100;
               w = 1280;
               h = 720;
           }
       }

       // 3. Restore via SetWindowPlacement
       WINDOWPLACEMENT wp{};
       wp.length = sizeof(WINDOWPLACEMENT);
       wp.flags = 0;
       wp.showCmd = windowMaximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
       wp.rcNormalPosition = RECT{ x, y, x + w, y + h };

       return SetWindowPlacement(hwnd, &wp) != 0;
   }
   #endif
   ```

#### Integration in `src/main.cpp`:
1. **At Startup (around line 286)**:
   ```cpp
   // BEFORE:
   // Display window now that the first frame is already drawn and ready
   ShowWindow(hwnd, SW_SHOWDEFAULT);
   UpdateWindow(hwnd);

   // AFTER:
   // Display window now that the first frame is already drawn and ready
   if (configLoaded) {
       appConfig.restoreWindowPlacement(hwnd);
   } else {
       ShowWindow(hwnd, SW_SHOWDEFAULT);
   }
   UpdateWindow(hwnd);
   ```
2. **At Shutdown (around line 328)**:
   ```cpp
   // BEFORE:
   appConfig.customPluginPaths = scanner.searchPaths();
   appConfig.save();

   // AFTER:
   appConfig.customPluginPaths = scanner.searchPaths();
   appConfig.saveWindowPlacement(hwnd);
   appConfig.save();
   ```

---

### 3.4 Feature 12: Compiler Warning Hardening & Latent Fixes

#### 1. `CMakeLists.txt` Changes:
Replace lines 58–66 and lines 84–115 with hardened flags and system include separation:

```cmake
# Third-party source file definitions
set(THIRD_PARTY_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
)

# Isolate third-party source files from strict first-party warnings
if(MSVC)
    set_source_files_properties(${THIRD_PARTY_SOURCES} PROPERTIES COMPILE_OPTIONS "/W0")
else()
    set_source_files_properties(${THIRD_PARTY_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

# First-party include directories
target_include_directories(Praccy PRIVATE
    src
    resources
)

# Isolate third-party headers as SYSTEM to suppress header warnings
target_include_directories(Praccy SYSTEM PRIVATE
    third_party
    third_party/imgui
    third_party/imgui/backends
    third_party/clap/include
    third_party/readerwriterqueue
)

# Hardened warning compilation options
if(MSVC)
    target_compile_options(Praccy PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(Praccy PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()

# Apply to test_praccy and test harnesses
target_include_directories(test_praccy PRIVATE src tests)
target_include_directories(test_praccy SYSTEM PRIVATE
    third_party
    third_party/clap/include
    third_party/readerwriterqueue
)
if(MSVC)
    target_compile_options(test_praccy PRIVATE /O2 /W4 /WX /permissive-)
    target_compile_options(test_asio_driver PRIVATE /O2 /W4 /WX /permissive-)
    target_compile_options(test_challenger_m1 PRIVATE /O2 /W4 /WX /permissive-)
    target_compile_options(test_challenger_m1_2 PRIVATE /O2 /W4 /WX /permissive-)
else()
    target_compile_options(test_praccy PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
    target_compile_options(test_asio_driver PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
    target_compile_options(test_challenger_m1 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
    target_compile_options(test_challenger_m1_2 PRIVATE -O3 -Wall -Wextra -Werror -Wno-unused-parameter)
endif()
```

#### 2. First-Party Code Warning Fixes:
1. **`src/audio/asio_manager.cpp:48`** (Fix `-Wstringop-truncation`):
   ```cpp
   // BEFORE:
   if (RegQueryValueExA(hDriverKey, "Description", nullptr, nullptr, reinterpret_cast<LPBYTE>(descStr), &descSize) != ERROR_SUCCESS) {
       std::strncpy(descStr, subKeyName, sizeof(descStr) - 1);
   }

   // AFTER:
   if (RegQueryValueExA(hDriverKey, "Description", nullptr, nullptr, reinterpret_cast<LPBYTE>(descStr), &descSize) != ERROR_SUCCESS) {
       std::snprintf(descStr, sizeof(descStr), "%s", subKeyName);
   }
   ```
2. **`src/plugins/vst3_host.cpp:176, 181, 317`** (Fix `-Wcast-function-type`):
   ```cpp
   // Line 176 BEFORE:
   auto* initDll = reinterpret_cast<InitDllProc>(GetProcAddress(hLib, "InitDll"));
   // AFTER:
   auto* initDll = reinterpret_cast<InitDllProc>(reinterpret_cast<void*>(GetProcAddress(hLib, "InitDll")));

   // Line 181 BEFORE:
   auto* getFactory = reinterpret_cast<GetFactoryProc>(GetProcAddress(hLib, "GetPluginFactory"));
   // AFTER:
   auto* getFactory = reinterpret_cast<GetFactoryProc>(reinterpret_cast<void*>(GetProcAddress(hLib, "GetPluginFactory")));

   // Line 317 BEFORE:
   auto* exitDll = reinterpret_cast<ExitDllProc>(GetProcAddress(m_module, "ExitDll"));
   // AFTER:
   auto* exitDll = reinterpret_cast<ExitDllProc>(reinterpret_cast<void*>(GetProcAddress(m_module, "ExitDll")));
   ```
3. **`src/plugins/plugin_scanner.cpp:265`** (Fix `-Wcast-function-type`):
   ```cpp
   // Line 265 BEFORE:
   auto* getFactory = reinterpret_cast<GetFactoryProc>(GetProcAddress(hLib, "GetPluginFactory"));
   // AFTER:
   auto* getFactory = reinterpret_cast<GetFactoryProc>(reinterpret_cast<void*>(GetProcAddress(hLib, "GetPluginFactory")));
   ```

---

### 3.5 Feature 13: Corrupted `presets.ini` Unit Test in `tests/test_praccy.cpp`

Add `testCorruptedPresetsIni()` to `tests/test_praccy.cpp`:

```cpp
#include <fstream>
#include <cstdio>

void testCorruptedPresetsIni() {
    std::cout << "[TEST] Corrupted presets.ini & config.ini Parsing Resilience... ";

    const std::string corruptPresetsPath = "test_corrupt_presets_temp.ini";

    // ------------------------------------------------------------------------
    // Scenario 1: Malformed numeric fields, invalid hex state, garbage tokens
    // ------------------------------------------------------------------------
    {
        std::ofstream out(corruptPresetsPath);
        out << "[Scene_0]\n"
            << "name=Corrupted Test Scene\n"
            << "numNodes=not_a_valid_integer\n" // Malformed integer count -> must safely default to 0
            << "node_0_kind=plugin\n"
            << "node_0_name=Corrupted Tube\n"
            << "node_0_path=builtin://amp\n"
            << "node_0_type=BuiltIn\n"
            << "node_0_bypassed=not_a_bool\n"
            << "node_0_dryWet=corrupted_float_nan\n" // Malformed float -> must safely default to 1.0f
            << "node_0_inGain=--++broken\n"          // Malformed float -> must safely default to 0.0f
            << "node_0_outGain=+99999999999999999999999999999999999999999999999\n" // Overflow
            << "node_0_state=INVALID_HEX_STREAM_ZZ\n" // Corrupted hex -> must safely default to empty {}
            << "[Scene_1]\n"
            << "name=Parallel Corrupted\n"
            << "numNodes=1\n"
            << "node_0_kind=parallel\n"
            << "node_0_numBranches=broken_branch_num\n"
            << "node_0_b_0_name=Branch Broken\n"
            << "node_0_b_0_gain=bad_gain\n"
            << "node_0_b_0_pan=bad_pan\n"
            << "node_0_b_0_numSlots=invalid_slots\n"
            << "node_0_b_0_s_0_dryWet=NaN\n"
            << "node_0_b_0_s_0_state=012\n"; // Odd-length hex -> must safely fail and return empty {}
        out.close();

        state::SceneManager mgr;
        // Must return true and parse without throwing std::invalid_argument or std::out_of_range
        bool loaded = mgr.loadFromFile(corruptPresetsPath);
        assert(loaded == true);
        assert(mgr.scenes().size() >= 2);

        // Verify Scene 0: numNodes defaulted to 0 safely without crash
        const auto& s0 = mgr.scenes()[0];
        assert(s0.name == "Corrupted Test Scene");
        assert(s0.nodes.empty());

        // Verify Scene 1: numBranches defaulted to 0 safely
        const auto& s1 = mgr.scenes()[1];
        assert(s1.name == "Parallel Corrupted");
        assert(s1.nodes.size() == 1);
        assert(s1.nodes[0].branches.empty());

        std::remove(corruptPresetsPath.c_str());
    }

    // ------------------------------------------------------------------------
    // Scenario 2: Truncated syntax, missing '=', broken brackets
    // ------------------------------------------------------------------------
    {
        std::ofstream out(corruptPresetsPath);
        out << "[Scene_0\n"               // Missing closing bracket
            << "name Scene Without Equals\n" // Missing '='
            << "numNodes=\n"              // Key with empty value
            << "==\n"                     // Double equals only
            << "random_unformatted_line\n"
            << "[Scene_1]\n"
            << "name=Valid Scene\n"
            << "numNodes=1\n"
            << "node_0_kind=plugin\n"
            << "node_0_dryWet=+0.85\n"    // Explicit plus sign supported
            << "node_0_inGain=-3.50\n"
            << "node_0_outGain=+6.00\n"
            << "node_0_state=0A0B0C\n";   // Valid hex stream
        out.close();

        state::SceneManager mgr;
        bool loaded = mgr.loadFromFile(corruptPresetsPath);
        assert(loaded == true);
        assert(!mgr.scenes().empty());

        // Find Scene 1
        bool foundValid = false;
        for (const auto& sc : mgr.scenes()) {
            if (sc.name == "Valid Scene") {
                foundValid = true;
                assert(sc.nodes.size() == 1);
                assert(std::abs(sc.nodes[0].slot.dryWet - 0.85f) < 1e-4f);
                assert(std::abs(sc.nodes[0].slot.inputGainDb - (-3.50f)) < 1e-4f);
                assert(std::abs(sc.nodes[0].slot.outputGainDb - 6.00f) < 1e-4f);
                assert(sc.nodes[0].slot.state.size() == 3);
                assert(sc.nodes[0].slot.state[0] == 0x0A);
                assert(sc.nodes[0].slot.state[1] == 0x0B);
                assert(sc.nodes[0].slot.state[2] == 0x0C);
            }
        }
        assert(foundValid);
        std::remove(corruptPresetsPath.c_str());
    }

    // ------------------------------------------------------------------------
    // Scenario 3: Zero-byte file recovery -> Reinitializes default factory scenes
    // ------------------------------------------------------------------------
    {
        std::ofstream out(corruptPresetsPath);
        out.close(); // Empty file

        state::SceneManager mgr;
        bool loaded = mgr.loadFromFile(corruptPresetsPath);
        assert(loaded == true);
        // Automatic fallback to 4 default scenes (Clean, Crunch, Lead, Ambient)
        assert(mgr.scenes().size() == 4);
        assert(mgr.scenes()[0].name == "1: Clean");
        assert(mgr.scenes()[1].name == "2: Crunch");
        assert(mgr.scenes()[2].name == "3: Lead");
        assert(mgr.scenes()[3].name == "4: Ambient");

        std::remove(corruptPresetsPath.c_str());
    }

    // ------------------------------------------------------------------------
    // Scenario 4: Corrupted AppConfig resilience
    // ------------------------------------------------------------------------
    {
        const std::string corruptConfigPath = "test_corrupt_config_temp.ini";
        std::ofstream out(corruptConfigPath);
        out << "input_mode=broken_routing\n"
            << "input_gain_db=not_a_float\n"
            << "master_volume_db=nan_volume\n"
            << "metronome_bpm=-9999999999999999999999999999999999999999999\n"
            << "window_x=invalid_x\n"
            << "window_y=invalid_y\n"
            << "window_w=broken_w\n"
            << "window_h=broken_h\n"
            << "window_maximized=invalid_bool\n";
        out.close();

        state::AppConfig cfg;
        bool loaded = cfg.load(corruptConfigPath);
        assert(loaded == true);
        assert(cfg.inputMode == audio::InputRoutingMode::MonoLeft);
        assert(cfg.inputGainDb == 0.0f);
        assert(cfg.masterVolumeDb == 0.0f);
        assert(cfg.metronomeBpm == 120.0f);
        assert(cfg.windowX == 100);
        assert(cfg.windowY == 100);
        assert(cfg.windowW == 1280);
        assert(cfg.windowH == 720);
        assert(cfg.windowMaximized == false);

        std::remove(corruptConfigPath.c_str());
    }

    std::cout << "PASSED\n";
}
```

And in `main()` of `tests/test_praccy.cpp`:
```cpp
    testCorruptedPresetsIni();
```

---

## 4. Caveats

1. **GCC/MinGW Floating-Point `std::from_chars` Support**:
   - `std::from_chars` for floating-point types (`float`, `double`) was introduced in C++17 and implemented in GCC 11+. Our build toolchain uses GCC 16.2.0 (and MSVC 2019/2022 supports it out of the box), verified empirically to work flawlessly. Older compilers (< GCC 11) lack floating-point `std::from_chars`.
2. **Leading `+` Sign Handling**:
   - Standard ISO C++ `std::from_chars` explicitly rejects `+` signs. The designed `parse_utils.h` intentionally detects and strips leading `'+'` before calling `std::from_chars`. If downstream developers bypass `parse_utils.h` and invoke `std::from_chars` directly, strings with `+` will fail to parse.
3. **Multi-Monitor DPI Scaling**:
   - `GetWindowPlacement` and `SetWindowPlacement` operate in virtual desktop coordinates. When monitors have differing DPI scales, Windows Per-Monitor DPI awareness handles scaling automatically when `SetProcessDPIAware()` is active.
4. **Window Placement Restoration Timing**:
   - Win32 `SetWindowPlacement` applies the normal geometry and show state. Calling it before the first DX11 frame is rendered could cause a momentary flicker; invoking it at `src/main.cpp:286` (where `ShowWindow(hwnd, SW_SHOWDEFAULT)` was previously called) maintains zero white flash.

---

## 5. Conclusion

Features 10, 11, 12, and 13 comprehensively eliminate crash-prone string parsing, solve window state loss on restart, establish strict compiler warning immunity, and harden the test harness:
1. **Feature 10**: Fully replaces throwing `std::stoul` and `std::stof` with zero-allocation, non-throwing C++20 `std::from_chars` wrapped in `src/utils/parse_utils.h`.
2. **Feature 11**: Fully persists window rectangle and maximized state across sessions via `GetWindowPlacement` / `SetWindowPlacement`, with multi-monitor bounds validation (`MonitorFromRect`) to guarantee windows never appear off-screen.
3. **Feature 12**: Enables `/W4 /WX` on MSVC and `-Wall -Wextra -Werror` on GCC, cleanly isolating third-party code as `SYSTEM PRIVATE` and resolving latent first-party warnings (`snprintf` in `asio_manager.cpp`, `FARPROC` double cast through `void*` in `vst3_host.cpp` and `plugin_scanner.cpp`).
4. **Feature 13**: Introduces `testCorruptedPresetsIni()` in `tests/test_praccy.cpp`, rigorously verifying non-throwing parsing and graceful degradation across corrupted numbers, malformed syntax, invalid hex bytes, and zero-byte files.

---

## 6. Verification Method

### 6.1 Strict Compiler Warning Verification Commands
Execute clean build with warnings treated as errors:
```powershell
& "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build clean
& "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build test_praccy
& "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build Praccy
```
*Expected Result*: Clean build with exit code 0, zero warnings, zero errors.

### 6.2 Test Suite Execution & Acceptance Assertion
Execute the core test suite including the corrupted presets test:
```powershell
f:\Projects\Praccy\build\test_praccy.exe
```
*Expected Result*:
```
[TEST] Corrupted presets.ini & config.ini Parsing Resilience... PASSED
ALL TESTS PASSED SUCCESSFULLY! (15/15)
```

### 6.3 Static Code Audit for Deprecated Conversions
Run static search to assert zero occurrences of throwing conversions in state management:
```powershell
Get-ChildItem -Path f:\Projects\Praccy\src\state -Recurse -Include *.cpp | Select-String -Pattern "std::stoul", "std::stof", "std::stoi", "std::atoi", "std::atof"
```
*Expected Result*: Zero matches returned.

### 6.4 Blueprint Invalidation Conditions
The blueprint is invalidated if:
1. An input string such as `numNodes=abc` or `dryWet=bad` causes an uncaught C++ exception or crash in `test_praccy.exe`.
2. Restoring window placement on a single-monitor system after unplugging a second monitor leaves the window outside visible screen bounds.
3. Building with `-Werror` or `/WX` fails on any first-party or third-party translation unit.
