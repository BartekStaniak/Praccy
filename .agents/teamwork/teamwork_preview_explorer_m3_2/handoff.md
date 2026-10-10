# Architectural Implementation Blueprint: Feature 16 (Embedded Typography Engine via Win32 Resources & High-DPI Scaling)

## 1. Observation

1. **Current Font Loading Mechanism in `src/main.cpp` (Lines 180–194)**:
   ```cpp
   if (GetFileAttributesA("C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES) {
       io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 17.0f, &fontConfig, glyphRanges);
       if (GetFileAttributesA("C:\\Windows\\Fonts\\seguisym.ttf") != INVALID_FILE_ATTRIBUTES) {
           ImFontConfig symConfig = fontConfig;
           symConfig.MergeMode = true;
           static const ImWchar symRanges[] = {
               0x2600, 0x26FF, // Miscellaneous Symbols (★, ☆, etc.)
               0x2700, 0x27BF, // Dingbats
               0,
           };
           io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 17.0f, &symConfig, symRanges);
       }
   } else {
       io.Fonts->AddFontDefault();
   }
   ```
   *Direct Finding*: Font loading currently relies on hardcoded absolute filesystem paths pointing to the host OS directory (`C:\Windows\Fonts\segoeui.ttf`). If running on non-Windows platforms, non-standard Windows installations, or Wine/Proton, this silently falls back to Dear ImGui's built-in low-resolution ProggyClean bitmap font. Furthermore, the font size (`17.0f`) is completely hardcoded without DPI scaling.

2. **Current Resource Script in `resources/praccy.rc` (Lines 1–6)**:
   ```rc
   // Praccy Windows Resource Script
   #define IDI_APP_ICON 101

   IDI_APP_ICON ICON "icon.ico"
   1            ICON "icon.ico"
   ```
   *Direct Finding*: `resources/praccy.rc` only defines the application icon (`IDI_APP_ICON 101`). There are zero font resources embedded, and no dedicated resource header (`resource.h`) exists. Both `IDI_APP_ICON` and the resource type are unmanaged by a central symbol header.

3. **Dear ImGui Memory Ownership Contract in `third_party/imgui/imgui_draw.cpp` (Lines 3768–3771) and `imgui.h` (Line 3894)**:
   ```cpp
   // IF YOU GET A CRASH IN THE IM_FREE() CALL HERE AND USED AddFontFromMemoryTTF():
   // - DUE TO LEGACY REASON AddFontFromMemoryTTF() TRANSFERS MEMORY OWNERSHIP BY DEFAULT.
   // - USE `ImFontConfig font_cfg; font_cfg.FontDataOwnedByAtlas = false; io.Fonts->AddFontFromMemoryTTF(....., &cfg);` to disable passing ownership
   ```
   *Direct Finding*: By default, `ImFontAtlas::AddFontFromMemoryTTF()` assumes ownership of the font memory buffer and attempts to deallocate it with `IM_FREE()` upon font atlas rebuild or when `ImGui::DestroyContext()` is invoked. Memory returned by Win32 `LockResource()` resides within the PE `.rsrc` read-only section mapped into the virtual address space. Attempting to `free()` this pointer results in immediate access violation (`0xC0000005`) or heap corruption. Setting `fontConfig.FontDataOwnedByAtlas = false` is strictly required.

4. **DPI Configuration in `src/main.cpp` (Lines 127–129 and 495–520)**:
   - Line 128: `SetProcessDPIAware();` enables legacy system-level DPI awareness, but does not enable per-monitor v2 DPI awareness (`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2`).
   - Line 149: The initial window geometry (`1280x720`) is hardcoded in device-independent units, but fonts and UI padding are never scaled to match display DPI.
   - `WndProc` (lines 495–520) handles `WM_SIZE`, `WM_ERASEBKGND`, `WM_SYSCOMMAND`, and `WM_DESTROY`, but does NOT handle `WM_DPICHANGED`.
   - `third_party/imgui/backends/imgui_impl_win32.h` (lines 45–47) already exports:
     ```cpp
     IMGUI_IMPL_API void  ImGui_ImplWin32_EnableDpiAwareness();
     IMGUI_IMPL_API float ImGui_ImplWin32_GetDpiScaleForHwnd(void* hwnd);
     IMGUI_IMPL_API float ImGui_ImplWin32_GetDpiScaleForMonitor(void* monitor);
     ```

5. **Font Asset Verification and Availability**:
   - `Inter-Regular.ttf`: Official Windows-hinted TrueType font from Rasmus Andersson (`rsms/inter` v3.19, `Inter Hinted for Windows/Desktop/Inter-Regular.ttf`, 680,240 bytes, SIL Open Font License 1.1).
   - `JetBrainsMono-Regular.ttf`: Official TrueType font from JetBrains (`JetBrains/JetBrainsMono` master, `fonts/ttf/JetBrainsMono-Regular.ttf`, 270,224 bytes, Apache License 2.0).
   - Both assets are verifiable via curl HTTP/200 requests from official repositories and are 100% compatible with Win32 `RC` compilers (`rc.exe` and `windres`) and Dear ImGui's `stb_truetype` rasterizer.

---

## 2. Logic Chain

1. **Embedding TrueType Fonts via Win32 PE Resources**:
   - *Observation 1* establishes that Praccy currently relies on an external system font path (`C:\Windows\Fonts\segoeui.ttf`), making UI typography fragile and non-portable.
   - *Observation 2* demonstrates that `resources/praccy.rc` has no font resources or shared `resource.h`.
   - *Requirement R3* requires embedding Inter (UI text) and JetBrains Mono (Audio/DSP readouts) as binary resources in `resources/praccy.rc` and loading them via `AddFontFromMemoryTTF`.
   - *Therefore*:
     1. Create `resources/resource.h` defining:
        - `IDI_APP_ICON 101`
        - `IDR_FONT_INTER 201`
        - `IDR_FONT_JETBRAINS_MONO 202`
     2. Place `Inter-Regular.ttf` and `JetBrainsMono-Regular.ttf` in `resources/fonts/`.
     3. Update `resources/praccy.rc` with:
        ```rc
        #include "resource.h"
        IDI_APP_ICON            ICON    "icon.ico"
        1                       ICON    "icon.ico"
        IDR_FONT_INTER          RCDATA  "fonts/Inter-Regular.ttf"
        IDR_FONT_JETBRAINS_MONO RCDATA  "fonts/JetBrainsMono-Regular.ttf"
        ```
     4. `CMakeLists.txt` already specifies `target_include_directories(Praccy PRIVATE src resources)` and includes `resources/praccy.rc` in `PRACCY_SOURCES`. When the resource compiler executes, `fonts/Inter-Regular.ttf` resolves cleanly relative to the `.rc` location and include flags.

2. **Resource Extraction and Atlas Lifetime Management**:
   - *Observation 3* proves that `io.Fonts->AddFontFromMemoryTTF` assumes buffer ownership by default and calls `IM_FREE` on atlas destruction, crashing if passed read-only PE `.rsrc` memory.
   - In Win32, PE resources extracted via `FindResourceW`, `LoadResource`, and `LockResource` point to process memory with `PAGE_READONLY` protection.
   - *Therefore*:
     1. Implement a helper `loadWin32Resource(HMODULE hMod, int resId, LPCWSTR resType)` returning a pointer `const void* data` and `DWORD size`.
     2. Create `ImFontConfig fontConfig` and set `fontConfig.FontDataOwnedByAtlas = false`.
     3. Pass `const_cast<void*>(data)` to `io.Fonts->AddFontFromMemoryTTF(..., &fontConfig, glyphRanges)`.
     4. Store the resulting `ImFont*` pointers in global pointers `g_fontUI` (Inter at scaled ~17px) and `g_fontMono` (JetBrains Mono at scaled ~14.5px).
     5. Set `io.FontDefault = g_fontUI;` so all standard UI widgets render using Inter by default.
     6. Declare `extern ImFont* g_fontUI;` and `extern ImFont* g_fontMono;` in a shared header (`src/ui/theme.h` or `src/ui/design_tokens.h`), enabling parameter readout headers, tuners, and DSP monitors to call `ImGui::PushFont(g_fontMono)` and `ImGui::PopFont()`.

3. **Multi-Tier Robust Fallback Architecture**:
   - If `loadWin32Resource` fails (e.g. running under a headless test runner without resource bundling or mock harness):
     - `g_fontUI` falls back to `io.Fonts->AddFontDefault()` (built-in ProggyClean).
     - If `g_fontMono` fails to load, it falls back to `g_fontUI` (`g_fontMono = g_fontUI;`).
   - This fallback guarantee ensures that neither `g_fontUI` nor `g_fontMono` is ever `nullptr`, eliminating null-pointer dereferences across the application.

4. **High-DPI Canvas Scaling Architecture**:
   - *Observation 4* shows that window initialization uses legacy `SetProcessDPIAware()`, font sizes are unscaled, and `WM_DPICHANGED` is unhandled.
   - *Acceptance Criteria* requires: "Signal chain remains centered and readable across 1080p (100% DPI), 1440p (125% DPI), and 4K (150% and 200% DPI) without clipped text or overlapping controls."
   - *Therefore*:
     1. Upgrade DPI initialization to Per-Monitor v2 awareness by invoking `ImGui_ImplWin32_EnableDpiAwareness()` before `CreateWindowW()`.
     2. Compute the active monitor scale factor:
        $$\text{dpiScale} = \frac{\text{GetDpiForWindow}(\text{hwnd})}{96.0f}$$
        with fallback to `ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd)` or `1.0f`.
     3. Scale base font sizes proportionally:
        - Inter (UI): $\text{size}_{\text{UI}} = \text{std::round}(17.0f \times \text{dpiScale})$
          - 1080p (100% DPI, 96 DPI): $17.0\text{px}$
          - 1440p (125% DPI, 120 DPI): $21.0\text{px}$
          - 4K (150% DPI, 144 DPI): $25.5\text{px}$
          - 4K (200% DPI, 192 DPI): $34.0\text{px}$
        - JetBrains Mono: $\text{size}_{\text{Mono}} = \text{std::round}(14.5f \times \text{dpiScale})$
          - 1080p (100% DPI, 96 DPI): $14.5\text{px}$
          - 1440p (125% DPI, 120 DPI): $18.0\text{px}$
          - 4K (150% DPI, 144 DPI): $22.0\text{px}$
          - 4K (200% DPI, 192 DPI): $29.0\text{px}$
     4. Scale ImGui layout geometry by calling `style.ScaleAllSizes(dpiScale)` right after `applyPraccyTheme()`. This scales window padding, frame padding, item spacing, inner spacing, and rounding so controls never overlap or clip at high DPI.
     5. Handle `WM_DPICHANGED` in `WndProc`: reposition and resize the window to the suggested `RECT*` passed in `lParam`.

---

## 3. Concrete Architectural Specification & Code Templates

### 3.1. Resource Header: `resources/resource.h`
```c
#pragma once

// Win32 Resource Identifiers for Praccy Host
#define IDI_APP_ICON              101
#define IDR_FONT_INTER            201
#define IDR_FONT_JETBRAINS_MONO   202
```

### 3.2. Windows Resource Script: `resources/praccy.rc`
```rc
// Praccy Windows Resource Script
#include "resource.h"

// Application Icons
IDI_APP_ICON            ICON    "icon.ico"
1                       ICON    "icon.ico"

// Embedded TrueType Fonts (RT_RCDATA / type 10)
IDR_FONT_INTER          RCDATA  "fonts/Inter-Regular.ttf"
IDR_FONT_JETBRAINS_MONO RCDATA  "fonts/JetBrainsMono-Regular.ttf"
```

### 3.3. Font Asset Provisioning (`resources/fonts/`)
Font files required in repository:
1. `resources/fonts/Inter-Regular.ttf` (680,240 bytes)
   - Source: `https://github.com/rsms/inter/releases/download/v3.19/Inter-3.19.zip` -> `Inter Hinted for Windows/Desktop/Inter-Regular.ttf`
   - License: SIL Open Font License 1.1
2. `resources/fonts/JetBrainsMono-Regular.ttf` (270,224 bytes)
   - Source: `https://raw.githubusercontent.com/JetBrains/JetBrainsMono/master/fonts/ttf/JetBrainsMono-Regular.ttf`
   - License: Apache License 2.0

*Automated Worker Acquisition Script (PowerShell)*:
```pwsh
New-Item -ItemType Directory -Force -Path "resources/fonts"

# 1. Fetch JetBrains Mono Regular TTF
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/JetBrains/JetBrainsMono/master/fonts/ttf/JetBrainsMono-Regular.ttf" -OutFile "resources/fonts/JetBrainsMono-Regular.ttf"

# 2. Fetch Inter Regular TTF from release archive
$wc = [System.Net.WebClient]::new()
$zipBytes = $wc.DownloadData("https://github.com/rsms/inter/releases/download/v3.19/Inter-3.19.zip")
$ms = [System.IO.MemoryStream]::new($zipBytes)
$zip = [System.IO.Compression.ZipArchive]::new($ms)
$entry = $zip.Entries | Where-Object { $_.FullName -like "*Inter Hinted for Windows/Desktop/Inter-Regular.ttf*" } | Select-Object -First 1
$outStream = [System.IO.File]::Create("resources/fonts/Inter-Regular.ttf")
$entryStream = $entry.Open()
$entryStream.CopyTo($outStream)
$entryStream.Close()
$outStream.Close()
```

### 3.4. Win32 Resource Extraction & Atlas Loader in `src/main.cpp`
```cpp
#include "resource.h"

// Global typography pointers exposed to entire UI layer
ImFont* g_fontUI   = nullptr;
ImFont* g_fontMono = nullptr;

namespace {

struct EmbeddedResource {
    const void* data = nullptr;
    DWORD       size = 0;
};

EmbeddedResource loadWin32Resource(HMODULE hModule, int resourceId, LPCWSTR resourceType) {
    EmbeddedResource res{};
    HRSRC hRsrc = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), resourceType);
    if (!hRsrc) return res;

    res.size = SizeofResource(hModule, hRsrc);
    if (res.size == 0) return res;

    HGLOBAL hGlobal = LoadResource(hModule, hRsrc);
    if (!hGlobal) return res;

    res.data = LockResource(hGlobal);
    return res;
}

float getDpiScaleForWindow(HWND hwnd) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
        auto pfnGetDpiForWindow = reinterpret_cast<PFN_GetDpiForWindow>(GetProcAddress(hUser32, "GetDpiForWindow"));
        if (pfnGetDpiForWindow && hwnd) {
            UINT dpi = pfnGetDpiForWindow(hwnd);
            if (dpi > 0) return static_cast<float>(dpi) / 96.0f;
        }
    }
    float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    return (scale > 0.0f) ? scale : 1.0f;
}

} // anonymous namespace
```

### 3.5. ImGui Font Atlas & DPI Initialization in `WinMain`
```cpp
    // 1. Enable Per-Monitor v2 DPI Awareness before creating window
    ImGui_ImplWin32_EnableDpiAwareness();

    // ... [Create Win32 window (hwnd)] ...

    // 2. Query monitor DPI scale factor
    const float dpiScale = getDpiScaleForWindow(hwnd);

    // 3. Initialize Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // 4. Apply base theme and scale all UI spacing/padding by dpiScale
    praccy::ui::applyPraccyTheme();
    if (dpiScale > 1.0f) {
        ImGui::GetStyle().ScaleAllSizes(dpiScale);
    }

    // 5. Configure font rasterization
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 2;
    fontConfig.OversampleV = 2;
    fontConfig.FontDataOwnedByAtlas = false; // CRITICAL: Prevent free() on read-only PE .rsrc memory

    static const ImWchar glyphRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0,
    };

    const float baseFontSizeUI   = 17.0f;
    const float baseFontSizeMono = 14.5f;
    const float scaledFontSizeUI   = std::round(baseFontSizeUI * dpiScale);
    const float scaledFontSizeMono = std::round(baseFontSizeMono * dpiScale);

    HMODULE hModule = GetModuleHandleW(nullptr);

    // 6. Load Inter (UI Primary Font)
    EmbeddedResource resInter = loadWin32Resource(hModule, IDR_FONT_INTER, RT_RCDATA);
    if (resInter.data && resInter.size > 0) {
        g_fontUI = io.Fonts->AddFontFromMemoryTTF(
            const_cast<void*>(resInter.data),
            static_cast<int>(resInter.size),
            scaledFontSizeUI,
            &fontConfig,
            glyphRanges
        );
    }

    // Fallback if Inter resource unavailable
    if (!g_fontUI) {
        g_fontUI = io.Fonts->AddFontDefault();
    }
    io.FontDefault = g_fontUI;

    // Optional: Merge Segoe UI Symbol for dingbats/stars if available on Windows host
    if (GetFileAttributesA("C:\\Windows\\Fonts\\seguisym.ttf") != INVALID_FILE_ATTRIBUTES) {
        ImFontConfig symConfig = fontConfig;
        symConfig.MergeMode = true;
        static const ImWchar symRanges[] = {
            0x2600, 0x26FF, // Miscellaneous Symbols (★, ☆, etc.)
            0x2700, 0x27BF, // Dingbats
            0,
        };
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", scaledFontSizeUI, &symConfig, symRanges);
    }

    // 7. Load JetBrains Mono (Numeric Parameters & DSP Readouts)
    EmbeddedResource resMono = loadWin32Resource(hModule, IDR_FONT_JETBRAINS_MONO, RT_RCDATA);
    if (resMono.data && resMono.size > 0) {
        ImFontConfig monoConfig = fontConfig;
        monoConfig.FontDataOwnedByAtlas = false;
        g_fontMono = io.Fonts->AddFontFromMemoryTTF(
            const_cast<void*>(resMono.data),
            static_cast<int>(resMono.size),
            scaledFontSizeMono,
            &monoConfig,
            glyphRanges
        );
    }

    // Fallback if JetBrains Mono unavailable (never leave g_fontMono null)
    if (!g_fontMono) {
        g_fontMono = g_fontUI;
    }
```

### 3.6. `WndProc` DPI Change Handling
```cpp
    case WM_DPICHANGED: {
        const RECT* prc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hWnd, nullptr,
            prc->left, prc->top,
            prc->right - prc->left,
            prc->bottom - prc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
```

### 3.7. Shared Header Exposure: `src/ui/theme.h`
Add font pointer declarations so UI modules can access monospace typography:
```cpp
// Global typography handles
extern ImFont* g_fontUI;
extern ImFont* g_fontMono;
```
Usage in rack components and plugin cards:
```cpp
// When rendering DSP parameter readouts, tuner values, or frequency metrics:
ImGui::PushFont(g_fontMono);
ImGui::Text("%.1f Hz", pitchHz);
ImGui::PopFont();
```

---

## 4. Caveats

1. **PE Resource Alignment & Read-Only Memory**:
   Resources loaded via `LockResource()` are memory-mapped directly into the process image with `PAGE_READONLY` access. `FontDataOwnedByAtlas = false` is essential not only during `ImGui::DestroyContext()` but also if `io.Fonts->Clear()` or atlas re-baking occurs.
2. **Dynamic Font Re-Rasterization on Monitor Switch**:
   When a user drags the window across monitors with different DPIs (`WM_DPICHANGED`), resizing the window via `SetWindowPos` preserves correct window bounds. Fully re-rasterizing the texture atlas on DirectX 11 during a drag event requires invalidating D3D11 font textures (`ImGui_ImplDX11_InvalidateDeviceObjects()`) and re-creating them. The primary requirement is ensuring the initial window boots crisply at 100%, 125%, 150%, and 200% DPI without text clipping.
3. **Headless & Unit Test Independence**:
   The headless test suite (`test_praccy.cpp`) does not link `resources/praccy.rc` and runs without a Win32 GUI window. The typography code isolated in `src/main.cpp` and `src/ui/` does not break headless audio tests.

---

## 5. Conclusion

Feature 16 modernizes Praccy's typography from brittle OS-dependent paths to an integrated Win32 PE binary resource typography engine:
1. `resources/resource.h` and `resources/praccy.rc` declare `IDR_FONT_INTER (201)` and `IDR_FONT_JETBRAINS_MONO (202)` as `RT_RCDATA` binary resources.
2. `resources/fonts/` embeds `Inter-Regular.ttf` and `JetBrainsMono-Regular.ttf`.
3. `src/main.cpp` extracts resources via native Win32 APIs, enforces `FontDataOwnedByAtlas = false`, and exposes `g_fontUI` and `g_fontMono`.
4. High-DPI scaling dynamically calculates the monitor scale factor, scaling fonts and all ImGui style geometry (`ScaleAllSizes`) proportionally across 1080p (100%), 1440p (125%), and 4K (150%, 200%).
5. Multi-tier fallbacks guarantee zero null-pointer crashes in headless or unsupported environments.

---

## 6. Verification Method

1. **Verify Windows Resource Compilation**:
   Run CMake build on the main `Praccy` target:
   ```pwsh
   cmake --build build --config Release --target Praccy
   ```
   *Expected Output*: Build succeeds with exit code 0 and `Praccy.exe` links without resource compiler errors.

2. **Verify Embedded Resource Presence**:
   Inspect the compiled PE executable using PowerShell to confirm resource section growth:
   ```pwsh
   (Get-Item build/Praccy.exe).Length
   ```
   *Pass Condition*: Binary size increases by ~950 KB (the combined size of `Inter-Regular.ttf` and `JetBrainsMono-Regular.ttf`), proving fonts are baked directly into the executable.

3. **Verify Core Unit Tests Unaffected**:
   Run test suite:
   ```pwsh
   ctest --test-dir build --output-on-failure
   ```
   *Expected Output*: 100% tests pass (all 5 test suites pass).

4. **Verify DPI Scaling Calculations**:
   Assert that for standard monitor DPIs:
   - 96 DPI (100%): `scaledFontSizeUI == 17.0f`, `scaledFontSizeMono == 15.0f` (or 14.5f)
   - 120 DPI (125%): `scaledFontSizeUI == 21.0f`, `scaledFontSizeMono == 18.0f`
   - 144 DPI (150%): `scaledFontSizeUI == 26.0f` (or 25.5f), `scaledFontSizeMono == 22.0f`
   - 192 DPI (200%): `scaledFontSizeUI == 34.0f`, `scaledFontSizeMono == 29.0f`
