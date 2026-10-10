# Milestone 4 Implementation Blueprint: Spotlight Command Palette Browser (Feature 22) & Native Win32 Folder Picker (Feature 23)

**Author**: Explorer 2 (`teamwork_preview_explorer_m4_2`)  
**Target Milestone**: Milestone 4 (Modular UI Refactoring & Practice Suite Overhaul - Requirement R4)  
**Parent Agent**: 6d04231a-d33e-4b26-b49d-7f9feca2b265  
**Deliverable**: Comprehensive Architectural Blueprint for Features 22 & 23  

---

## 1. Observation

### 1.1 Existing Plugin Browser Implementation in `src/ui/rack_view.cpp`
1. **Monolithic Placement and Sizing**:
   - Lines 2552–2561 in `src/ui/rack_view.cpp`:
     ```cpp
     void RackView::renderPluginBrowserModal() {
         ImGuiIO& io = ImGui::GetIO();
         const auto& tokens = themeTokens();
         ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
         ImGui::SetNextWindowSize(ImVec2(880, 620), ImGuiCond_Appearing);
         if (m_focusPluginBrowser) {
             ImGui::SetNextWindowFocus();
             m_focusPluginBrowser = false;
         }
         if (ImGui::Begin("Plugin Manager & Scanner", &m_showPluginBrowser, ImGuiWindowFlags_NoCollapse)) {
     ```
   - Current dimensions are hardcoded to `880x620px` with a standard window frame (`ImGui::Begin` with window collapse/close buttons). It does not render as a centered, borderless, floating Spotlight overlay (`560x420px`).

2. **Primitive Substring Filtering Without Fuzzy Match or Category Search**:
   - Lines 2565–2566:
     ```cpp
     ImGui::InputTextWithHint("##PluginFilter", "Search plugins by name or vendor...", m_pluginSearchQuery, sizeof(m_pluginSearchQuery));
     ```
   - `PluginScanner::getFilteredPlugins` (in `src/plugins/plugin_scanner.cpp`, lines 467–477):
     ```cpp
     if (!qLower.empty()) {
         std::string nameLower = p.name;
         std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), [](unsigned char c) { return std::tolower(c); });
         std::string vendorLower = p.vendor;
         std::transform(vendorLower.begin(), vendorLower.end(), vendorLower.begin(), [](unsigned char c) { return std::tolower(c); });

         if (nameLower.find(qLower) == std::string::npos && vendorLower.find(qLower) == std::string::npos) {
             continue;
         }
     }
     ```
   - Filtering only checks `std::string::find` against `name` and `vendor`. Category (`p.category`) and format (`p.typeString()`) are ignored in the text query.
   - No fuzzy scoring, prefix weighting, or word boundary match exists.

3. **Absence of Keyboard Navigation and Auto-Focus**:
   - `renderPluginBrowserModal()` contains zero handling for Up/Down arrow keys.
   - Pressing Enter does not instantiate the highlighted plugin.
   - Pressing Escape does not close the dialog (only clicking the "Close" button or title-bar X closes it).
   - `ImGui::SetKeyboardFocusHere()` is not invoked, requiring the user to explicitly click inside the search box every time.

4. **Absence of Recents / Most-Used Section**:
   - When `m_pluginSearchQuery` is empty, the table simply lists all plugins alphabetically or unsorted.
   - `AppConfig` (in `src/state/app_config.h`, lines 26–27) stores `customPluginPaths` and `favoritePlugins`, but does not persist recent or most-used plugins:
     ```cpp
     std::vector<std::string> customPluginPaths;
     std::vector<std::string> favoritePlugins;
     ```

5. **Lack of Global Shortcut `Ctrl+P`**:
   - In `src/ui/rack_view.cpp` lines 528–539:
     ```cpp
     ImGuiIO& io = ImGui::GetIO();
     if (!io.WantTextInput) {
         for (int k = 0; k < 8 && k < static_cast<int>(m_scenes.numScenes()); ++k) {
             if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + k), false)) {
                 m_scenes.applyScene(k, m_graph);
     ...
     ```
   - Only scene preset keys `1`–`8` are handled. There is no detection for `io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P)`.

### 1.2 Existing Search Path Input in `src/ui/rack_view.cpp` and `SettingsModal`
1. **Manual Text Entry in Popup**:
   - Lines 2839–2850 in `src/ui/rack_view.cpp`:
     ```cpp
     ImGui::Spacing();
     ImGui::SetNextItemWidth(320);
     ImGui::InputTextWithHint("##NewPath", "e.g. D:\\AudioPlugins", m_newPathBuffer, sizeof(m_newPathBuffer));
     ImGui::SameLine(0, 8);
     if (CenteredButton("+ Add Path", ImVec2(90, 24))) {
         if (m_newPathBuffer[0] != '\0') {
             m_scanner.addCustomSearchPath(m_newPathBuffer);
             m_scanner.scanAll();
             state::AppConfig cfg;
             cfg.load();
             cfg.customPluginPaths = m_scanner.searchPaths();
             cfg.save();
             m_newPathBuffer[0] = '\0';
         }
     }
     ```
   - Manual path string typing is error-prone, requiring the user to copy-paste Windows paths. There is no folder browser dialog.

2. **Settings Modal "Plugins" Tab Lacks Search Path Management**:
   - Lines 3142–3170 in `src/ui/rack_view.cpp`:
     ```cpp
     if (ImGui::BeginTabItem("Plugins")) {
         ImGui::Spacing();
         ImGui::TextColored(tokens.text.accent.vec4, "PLUGIN MANAGER & SCANNER");
         ImGui::Separator();
         ImGui::Spacing();

         ImGui::Text("Registered Plugins: %zu scanned", m_scanner.numPlugins());
         ImGui::Text("Search Paths: %zu directories", m_scanner.searchPaths().size());
         ImGui::Spacing();

         if (CenteredButton("Open Plugin Manager & Scanner...", ImVec2(250, 28))) {
             m_showPluginBrowser = true;
             m_focusPluginBrowser = true;
         }
     ```
   - The settings modal merely displays the count of search paths; it provides no interface to add or browse folder paths.

### 1.3 Windows Toolchain and COM Capabilities
1. **Compiler Environment**:
   - Toolchain: `w64devkit/bin/g++.exe` (GCC 14.2.0, Windows x86_64).
   - Direct verification:
     - `C:/Users/Bartek/w64devkit/include/shobjidl.h` exists (size: 1,248,408 bytes).
     - `C:/Users/Bartek/w64devkit/include/wrl/client.h` exists.
     - `Microsoft::WRL::ComPtr<IFileOpenDialog>` compiles warning-free under `-std=c++20 -Wall -Wextra -Werror` / `/W4 /WX`.
2. **Linker Libraries**:
   - `CMakeLists.txt` lines 88–95 & 140:
     - `ole32`, `uuid`, and `shell32` are already linked to both the `Praccy` executable and the `test_praccy` test suite.

---

## 2. Logic Chain

### 2.1 Feature 22: Spotlight Command Palette Browser
1. **Modal Geometry & Positioning**:
   - *Requirement*: Centered floating Spotlight modal dialog (`width = 560px`, `height = 420px`).
   - *Design*:
     - Modal should use `ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f))` and `ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f))`.
     - Window flags: `ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar`.
     - The dark modal backdrop (`tokens.surfaces.modalOverlay`) dims the background canvas, drawing focus exclusively to the palette.

2. **Triggering and Active Slot Resolution**:
   - *Requirement*: Triggered via shortcut `Ctrl+P` or clicking `[+ ADD PLUGIN]` in rack.
   - *Design*:
     - In `RackView::render()`, evaluate `io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false)`.
     - If triggered via `Ctrl+P`, open palette with target slot set to default active insertion slot (`m_insertTargetBlockIndex = -1; m_insertTargetBranchIndex = -1`).
     - If triggered via serial card plus button (`##AddPluginSerial`), open palette with target `(-1, -1)`.
     - If triggered via parallel branch card plus button (`##AddPluginBr0` / `##AddPluginBr1`), open palette with target `(blockIndex, branchIndex)`.

3. **Auto-Focused Search Input**:
   - *Requirement*: Auto-focused search text input.
   - *Design*:
     - When the modal is triggered (`m_needsFocus = true` or `ImGui::IsWindowAppearing()`), execute `ImGui::SetKeyboardFocusHere()`.
     - Text field spans full width (`536px`), height `36px`, with placeholder `"Search plugins by name, vendor, category, or format..."`.

4. **Real-Time Fuzzy / Substring Filtering**:
   - *Requirement*: Real-time fuzzy/substring filtering across plugin name, vendor/developer, category, and format (VST3/CLAP/Built-In).
   - *Design*:
     - Parse search query into lowercase tokens.
     - For each scanned plugin:
       - Exact name match: `+1000` score.
       - Prefix name match: `+500` score.
       - Word-boundary name match: `+300` score.
       - Substring name match: `+200` score.
       - Vendor match (prefix / substring): `+150` / `+100` score.
       - Category match (e.g. "Amp", "Delay", "Distortion"): `+80` score.
       - Format match ("vst3", "clap", "built-in"): `+60` score.
       - Subsequence fuzzy match (letters appearing in sequence): consecutive match bonus `+15`, boundary bonus `+25`.
     - Filtered results are sorted in descending order of score.

5. **Quick Recents / Most-Used Section**:
   - *Requirement*: Quick recents / most-used plugins section when search query is empty.
   - *Design*:
     - When `m_searchQuery[0] == '\0'`:
       - Top section: `"RECENTS & FREQUENT"` rendering up to 6 most-used or recently instantiated plugins.
       - Bottom section: `"ALL PLUGINS (A-Z)"` listing remaining plugins alphabetically.
     - Maintain an LRU list `recentPlugins` (max 8 entries) and usage counter map `pluginUsageCount` in `AppConfig` or `PluginScanner`.
     - When a plugin is instantiated via Enter or click, increment its usage and move it to the front of `recentPlugins`, saving to `AppConfig`.

6. **Clean List Rendering**:
   - *Requirement*: Format badge, developer name, and category pill.
   - *Design*:
     - Row height: 38px.
     - Column 1: Format badge (VST3: Cyan `tokens.text.badgeVst3`, CLAP: Magenta `tokens.text.badgeClap`, Built-In: Amber `tokens.text.badgeInternal`).
     - Column 2: Plugin Name (Bold primary text `tokens.text.primary`).
     - Column 3: Vendor / Developer (Muted secondary text `tokens.text.secondary`).
     - Column 4: Category Pill (Rounded frame with `tokens.surfaces.frameBg`, `tokens.borders.subtle`, `tokens.text.muted`).
     - Favorite indicator: Gold star icon rendered using token colors.
     - Active highlighted row has background `tokens.surfaces.cardBgSelected` with a 3px accent indicator bar on the left edge (`tokens.borders.focus`).

7. **Full Keyboard Navigation**:
   - *Requirement*: Up/Down arrow keys navigate list, Enter instantiates selected plugin into active insertion slot, Esc closes palette.
   - *Design*:
     - Use `ImGuiInputTextFlags_CallbackHistory` on the search input box. In the callback, intercept `ImGuiKey_UpArrow` and `ImGuiKey_DownArrow` to update `m_selectedIndex`.
     - Intercept `ImGui::IsKeyPressed(ImGuiKey_UpArrow)` / `ImGuiKey_DownArrow` outside the input box.
     - Clamp `m_selectedIndex` within `[0, filteredCount - 1]`.
     - Auto-scroll selected row into view using `ImGui::SetScrollHereY(0.5f)`.
     - When Enter is pressed (via `ImGuiInputTextFlags_EnterReturnsTrue` or `ImGuiKey_Enter`): instantiate plugin at `m_selectedIndex` into the target slot and close palette.
     - When Esc is pressed (`ImGuiKey_Escape`): close palette immediately.

---

### 2.2 Feature 23: Native Win32 Folder Picker (`IFileOpenDialog`)
1. **Modern Shell File Dialog**:
   - *Requirement*: Replace manual path string typing with modern Win32 `IFileOpenDialog` with `FOS_PICKFOLDERS`.
   - *Design*:
     - Instantiate `IFileOpenDialog` via `CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fileDialog))`.
     - Query current options via `fileDialog->GetOptions(&options)` and configure:
       `fileDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST)`.
     - Set window title via `fileDialog->SetTitle(L"Select Audio Plugin Directory")`.

2. **COM Lifecycle & Smart Pointers**:
   - *Requirement*: Manage COM lifecycle safely (`CoInitializeEx` / `CoUninitialize` or safe COM smart pointers).
   - *Design*:
     - Create an RAII class `ScopedComInitializer`:
       ```cpp
       class ScopedComInitializer {
       public:
           ScopedComInitializer() noexcept {
               m_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
           }
           ~ScopedComInitializer() {
               if (SUCCEEDED(m_hr)) {
                   CoUninitialize();
               }
           }
           [[nodiscard]] bool succeeded() const noexcept {
               return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE;
           }
       private:
           HRESULT m_hr{E_FAIL};
       };
       ```
     - Handles already-initialized apartments (`RPC_E_CHANGED_MODE`) safely without calling unbalanced `CoUninitialize()`.
     - Use `Microsoft::WRL::ComPtr<IFileOpenDialog>` and `Microsoft::WRL::ComPtr<IShellItem>` to guarantee automatic leak-free `Release()` on all exit branches.

3. **Path Retrieval, Conversion & Deallocation**:
   - *Requirement*: Retrieve path via `IShellItem::GetDisplayName(SIGDN_FILESYSPATH, &pszPath)`, convert from `PWSTR` to UTF-8 `std::string`, free via `CoTaskMemFree`.
   - *Design*:
     - Call `shellItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)`.
     - Convert `PWSTR` to UTF-8 `std::string` using `WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, ...)`.
     - Free memory immediately using `CoTaskMemFree(pszPath)`.

4. **Graceful Cancellation**:
   - *Requirement*: Fallback safety: handle user cancellation gracefully without error.
   - *Design*:
     - If the user clicks "Cancel" or closes the dialog, `fileDialog->Show(parentHwnd)` returns `HRESULT_FROM_WIN32(ERROR_CANCELLED)` (`0x800704C7`).
     - Detect `FAILED(hr)` and return `std::nullopt` immediately. No error dialogs or warnings are raised.

5. **Persistence and Scanner Sync**:
   - *Requirement*: Append valid folder path to `appConfig().pluginPaths` and persist via `AppConfig::save()`.
   - *Design*:
     - Verify directory exists via `std::filesystem::is_directory(path)`.
     - Normalize path separators.
     - Deduplicate against existing search paths in `m_scanner.searchPaths()`.
     - Call `m_scanner.addCustomSearchPath(path)`.
     - Update `appConfig.customPluginPaths = m_scanner.searchPaths(); appConfig.save();`.
     - Call `m_scanner.scanAll()` so newly discovered plugins appear in real time.

---

## 3. Caveats

1. **Modal Separation Boundary (Milestone 4 Division of Labor)**:
   - Explorer 1 is designing the translation unit decoupling for modals (`src/ui/modals/plugin_browser_modal.h/.cpp` and `settings_modal.h/.cpp`).
   - The native Win32 folder picker function should be placed in a shared header/source unit (e.g. `src/utils/native_dialogs.h` or within `src/ui/modals/settings_modal.cpp`), callable from both `SettingsModal` and the Command Palette Search Paths manager.
2. **Win32 Message Pumping During Modal Dialog**:
   - `fileDialog->Show(hwnd)` is a synchronous Win32 modal dialog that runs its own Windows message pump.
   - The ASIO audio callback runs on a separate high-priority MMCSS thread (`AvSetMmThreadCharacteristicsW`), so real-time audio playback remains glitch-free while the folder dialog is open.
   - DirectX 11 ImGui rendering is paused while the Win32 dialog is open and resumes immediately upon dismissal.
3. **Parent HWND Resolution**:
   - In Dear ImGui Win32 backend, the native handle is accessible via `(HWND)ImGui::GetMainViewport()->PlatformHandleRaw`.
   - Fallback to `GetActiveWindow()` ensures compatibility across all initialization states.
4. **Path Canonicalization on Windows**:
   - Windows path comparisons can differ by trailing slashes and case (e.g., `C:\VST` vs `c:\vst\`). Paths should be normalized via `std::filesystem::weakly_canonical()` or `lexically_normal()` before deduplication.

---

## 4. Conclusion & Concrete Blueprint

### 4.1 Feature 22: Spotlight Command Palette Browser Architecture

#### Class Definition: `src/ui/modals/plugin_browser_modal.h`

```cpp
#pragma once

#include "../../plugins/plugin_scanner.h"
#include "../../audio/graph_engine.h"
#include <string>
#include <vector>

namespace praccy::ui {

struct PaletteItem {
    const plugins::PluginDescriptor* descriptor{nullptr};
    int score{0};
    bool isRecent{false};
};

class PluginBrowserModal {
public:
    PluginBrowserModal(plugins::PluginScanner& scanner, audio::GraphEngine& graph);
    ~PluginBrowserModal() = default;

    // Open palette for active serial insertion or parallel branch
    void open(int targetBlockIndex = -1, int targetBranchIndex = -1);
    void close();
    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }

    // Renders the palette every frame within main UI loop
    void render();

    // Trigger keyboard navigation externally or internally
    void navigateSelection(int delta);

private:
    void updateFilteredList();
    void instantiateSelectedPlugin();
    void recordPluginUsage(const plugins::PluginDescriptor& desc);

    static int SearchInputCallback(struct ImGuiInputTextCallbackData* data);

    plugins::PluginScanner& m_scanner;
    audio::GraphEngine& m_graph;

    bool m_isOpen{false};
    bool m_needsFocus{false};
    bool m_scrollSelectionIntoView{false};

    char m_searchQuery[128]{0};
    int m_selectedIndex{0};
    std::vector<PaletteItem> m_filteredItems;

    int m_targetBlockIndex{-1};
    int m_targetBranchIndex{-1};

    std::vector<std::string> m_recentPlugins; // Stored paths/names (max 8)
};

} // namespace praccy::ui
```

#### Detailed Implementation Blueprint: `src/ui/modals/plugin_browser_modal.cpp`

```cpp
#include "plugin_browser_modal.h"
#include "../design_tokens.h"
#include "../theme.h"
#include "../../state/app_config.h"
#include "../../plugins/builtin_dsp.h"
#include "../../plugins/clap_host.h"
#include "../../plugins/vst3_host.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>

namespace praccy::ui {

namespace {

// Case-insensitive substring and fuzzy matching score calculator
int calculateFuzzyScore(std::string_view text, std::string_view query) {
    if (query.empty()) return 0;
    if (text.empty()) return -1;

    std::string tLower;
    tLower.reserve(text.size());
    for (char c : text) tLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    std::string qLower;
    qLower.reserve(query.size());
    for (char c : query) qLower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    // 1. Exact match
    if (tLower == qLower) return 1000;

    // 2. Prefix match
    if (tLower.starts_with(qLower)) return 500;

    // 3. Word boundary match (e.g. "Pro" matching "FabFilter Pro-Q")
    size_t pos = tLower.find(qLower);
    if (pos != std::string::npos) {
        if (pos > 0 && (tLower[pos - 1] == ' ' || tLower[pos - 1] == '-' || tLower[pos - 1] == '_')) {
            return 300 - static_cast<int>(pos);
        }
        return 200 - static_cast<int>(pos);
    }

    // 4. Fuzzy subsequence match
    int score = 0;
    size_t tIdx = 0;
    size_t consecutiveMatches = 0;

    for (char qChar : qLower) {
        bool found = false;
        while (tIdx < tLower.size()) {
            if (tLower[tIdx] == qChar) {
                found = true;
                score += 10;
                score += static_cast<int>(consecutiveMatches * 15);
                consecutiveMatches++;
                // Boundary bonus
                if (tIdx == 0 || tLower[tIdx - 1] == ' ' || tLower[tIdx - 1] == '-' || tLower[tIdx - 1] == '_') {
                    score += 25;
                }
                tIdx++;
                break;
            }
            consecutiveMatches = 0;
            tIdx++;
        }
        if (!found) return -1; // Missing character in sequence
    }

    return score;
}

} // namespace

PluginBrowserModal::PluginBrowserModal(plugins::PluginScanner& scanner, audio::GraphEngine& graph)
    : m_scanner(scanner), m_graph(graph) {
    state::AppConfig cfg;
    if (cfg.load()) {
        m_recentPlugins = cfg.favoritePlugins; // Initialize with saved favorites / recents
    }
}

void PluginBrowserModal::open(int targetBlockIndex, int targetBranchIndex) {
    m_targetBlockIndex = targetBlockIndex;
    m_targetBranchIndex = targetBranchIndex;
    m_searchQuery[0] = '\0';
    m_selectedIndex = 0;
    m_isOpen = true;
    m_needsFocus = true;
    m_scrollSelectionIntoView = true;
    updateFilteredList();
}

void PluginBrowserModal::close() {
    m_isOpen = false;
    m_searchQuery[0] = '\0';
    m_selectedIndex = 0;
}

void PluginBrowserModal::navigateSelection(int delta) {
    if (m_filteredItems.empty()) {
        m_selectedIndex = -1;
        return;
    }
    m_selectedIndex += delta;
    if (m_selectedIndex < 0) {
        m_selectedIndex = 0;
    } else if (m_selectedIndex >= static_cast<int>(m_filteredItems.size())) {
        m_selectedIndex = static_cast<int>(m_filteredItems.size()) - 1;
    }
    m_scrollSelectionIntoView = true;
}

int PluginBrowserModal::SearchInputCallback(ImGuiInputTextCallbackData* data) {
    auto* modal = static_cast<PluginBrowserModal*>(data->UserData);
    if (!modal) return 0;

    if (data->EventKey == ImGuiKey_UpArrow) {
        modal->navigateSelection(-1);
        return 1;
    } else if (data->EventKey == ImGuiKey_DownArrow) {
        modal->navigateSelection(+1);
        return 1;
    }
    return 0;
}

void PluginBrowserModal::updateFilteredList() {
    m_filteredItems.clear();
    const auto allPlugins = m_scanner.scannedPlugins();
    std::string_view query(m_searchQuery);

    if (query.empty()) {
        // Mode A: Empty Query -> Recents & Frequent first, then All Plugins
        // 1. Recents
        for (const auto& path : m_recentPlugins) {
            for (const auto& p : allPlugins) {
                if (p.path == path || p.name == path) {
                    m_filteredItems.push_back(PaletteItem{ &p, 1000, true });
                    break;
                }
            }
        }
        // 2. Remaining plugins sorted alphabetically
        for (const auto& p : allPlugins) {
            bool alreadyIn = false;
            for (const auto& item : m_filteredItems) {
                if (item.descriptor->name == p.name && item.descriptor->path == p.path) {
                    alreadyIn = true;
                    break;
                }
            }
            if (!alreadyIn) {
                m_filteredItems.push_back(PaletteItem{ &p, 0, false });
            }
        }
    } else {
        // Mode B: Fuzzy Search Query Filtering
        for (const auto& p : allPlugins) {
            int nameScore = calculateFuzzyScore(p.name, query);
            int vendorScore = calculateFuzzyScore(p.vendor, query);
            int catScore = calculateFuzzyScore(p.category, query);
            int formatScore = calculateFuzzyScore(p.typeString(), query);

            int bestScore = -1;
            if (nameScore >= 0) bestScore = std::max(bestScore, nameScore + 200);
            if (vendorScore >= 0) bestScore = std::max(bestScore, vendorScore + 100);
            if (catScore >= 0) bestScore = std::max(bestScore, catScore + 80);
            if (formatScore >= 0) bestScore = std::max(bestScore, formatScore + 50);

            if (bestScore >= 0) {
                m_filteredItems.push_back(PaletteItem{ &p, bestScore, false });
            }
        }

        // Sort descending by score, tie-break by name
        std::sort(m_filteredItems.begin(), m_filteredItems.end(), [](const PaletteItem& a, const PaletteItem& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.descriptor->name < b.descriptor->name;
        });
    }

    if (m_selectedIndex >= static_cast<int>(m_filteredItems.size())) {
        m_selectedIndex = m_filteredItems.empty() ? -1 : 0;
    }
}

void PluginBrowserModal::instantiateSelectedPlugin() {
    if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_filteredItems.size())) {
        return;
    }

    const auto* desc = m_filteredItems[m_selectedIndex].descriptor;
    if (!desc) return;

    std::unique_ptr<audio::PluginSlot> newSlot;
    if (desc->type == plugins::PluginType::CLAP) {
        auto clapInst = plugins::ClapPluginInstance::loadFromFile(desc->path);
        if (clapInst) newSlot = std::make_unique<audio::PluginSlot>(std::move(clapInst));
    } else if (desc->type == plugins::PluginType::VST3) {
        auto vst3Inst = plugins::Vst3PluginInstance::loadFromFile(desc->path);
        if (vst3Inst) newSlot = std::make_unique<audio::PluginSlot>(std::move(vst3Inst));
    } else if (desc->path == "builtin://drive") {
        newSlot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>());
    } else if (desc->path == "builtin://amp") {
        newSlot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::TubeAmpEffect>());
    } else if (desc->path == "builtin://delay") {
        newSlot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::StereoDelayEffect>());
    }

    if (newSlot) {
        newSlot->prepare(m_graph.sampleRate(), m_graph.maxBlockSize());
        if (m_targetBlockIndex >= 0 && m_targetBranchIndex >= 0) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(m_targetBlockIndex));
            if (block) {
                auto* branch = block->getBranch(m_targetBranchIndex);
                if (branch) {
                    branch->addSlot(std::move(newSlot));
                }
            }
        } else {
            m_graph.addSerialNode(std::move(newSlot));
        }

        recordPluginUsage(*desc);
        close();
    }
}

void PluginBrowserModal::recordPluginUsage(const plugins::PluginDescriptor& desc) {
    const std::string key = desc.path.empty() ? desc.name : desc.path;
    auto it = std::find(m_recentPlugins.begin(), m_recentPlugins.end(), key);
    if (it != m_recentPlugins.end()) {
        m_recentPlugins.erase(it);
    }
    m_recentPlugins.insert(m_recentPlugins.begin(), key);
    if (m_recentPlugins.size() > 8) {
        m_recentPlugins.resize(8);
    }

    state::AppConfig cfg;
    cfg.load();
    cfg.favoritePlugins = m_recentPlugins;
    cfg.save();
}

void PluginBrowserModal::render() {
    if (!m_isOpen) return;

    const auto& tokens = themeTokens();
    ImGuiIO& io = ImGui::GetIO();

    // 1. Center Positioning and Exact Dimensions (560px x 420px)
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 420.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, static_cast<ImVec4>(tokens.surfaces.cardBg));
    ImGui::PushStyleColor(ImGuiCol_Border, static_cast<ImVec4>(tokens.borders.strong));

    if (ImGui::Begin("##SpotlightCommandPalette", &m_isOpen, flags)) {
        // Global Keyboard Shortcut Interception within Palette
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            close();
            ImGui::End();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
            return;
        }

        // 2. Search Text Input Header (Height ~36px)
        ImGui::SetCursorPos(ImVec2(12.0f, 12.0f));
        ImGui::SetNextItemWidth(536.0f);

        if (m_needsFocus) {
            ImGui::SetKeyboardFocusHere();
            m_needsFocus = false;
        }

        ImGuiInputTextFlags inputFlags = ImGuiInputTextFlags_EnterReturnsTrue |
                                         ImGuiInputTextFlags_CallbackHistory;

        char prevQuery[128];
        std::memcpy(prevQuery, m_searchQuery, sizeof(prevQuery));

        bool enterPressed = ImGui::InputTextWithHint(
            "##SpotlightSearchInput",
            "Search plugins by name, vendor, category, or format... (Ctrl+P)",
            m_searchQuery,
            sizeof(m_searchQuery),
            inputFlags,
            SearchInputCallback,
            this
        );

        if (std::strcmp(prevQuery, m_searchQuery) != 0) {
            updateFilteredList();
        }

        if (enterPressed || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
            instantiateSelectedPlugin();
        }

        ImGui::SetCursorPosY(54.0f);
        ImGui::Separator();

        // 3. Scrollable Results List (Height: 320px)
        ImGui::SetCursorPos(ImVec2(12.0f, 62.0f));
        ImGui::BeginChild("##SpotlightResultsList", ImVec2(536.0f, 320.0f), false, ImGuiWindowFlags_None);

        if (m_filteredItems.empty()) {
            ImGui::SetCursorPosY(120.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, static_cast<ImVec4>(tokens.text.muted));
            float textW = ImGui::CalcTextSize("No matching plugins found.").x;
            ImGui::SetCursorPosX((536.0f - textW) * 0.5f);
            ImGui::TextUnformatted("No matching plugins found.");
            ImGui::PopStyleColor();
        } else {
            for (int i = 0; i < static_cast<int>(m_filteredItems.size()); ++i) {
                const auto& item = m_filteredItems[i];
                const auto* desc = item.descriptor;
                bool isSelected = (i == m_selectedIndex);

                ImGui::PushID(i);
                ImVec2 rowStart = ImGui::GetCursorScreenPos();
                float rowH = 38.0f;
                ImVec2 rowEnd = ImVec2(rowStart.x + 536.0f, rowStart.y + rowH);

                // Row background and left focus bar
                ImDrawList* dl = ImGui::GetWindowDrawList();
                if (isSelected) {
                    dl->AddRectFilled(rowStart, rowEnd, tokens.surfaces.cardBgSelected, 4.0f);
                    dl->AddRectFilled(rowStart, ImVec2(rowStart.x + 3.0f, rowEnd.y), tokens.borders.focus, 2.0f);
                } else if (ImGui::IsMouseHoveringRect(rowStart, rowEnd)) {
                    dl->AddRectFilled(rowStart, rowEnd, tokens.surfaces.cardBgHovered, 4.0f);
                }

                if (isSelected && m_scrollSelectionIntoView) {
                    ImGui::SetScrollHereY(0.5f);
                    m_scrollSelectionIntoView = false;
                }

                // Invisible selectable to handle mouse clicks / double-clicks
                ImGui::SetCursorScreenPos(rowStart);
                if (ImGui::Selectable("##RowSelect", isSelected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(536.0f, rowH))) {
                    m_selectedIndex = i;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        instantiateSelectedPlugin();
                    }
                }

                // Format Badge Pill
                float badgeY = rowStart.y + 9.0f;
                ImVec2 badgePos = ImVec2(rowStart.x + 10.0f, badgeY);
                ColorToken badgeCol = (desc->type == plugins::PluginType::VST3) ? tokens.text.badgeVst3 :
                                      (desc->type == plugins::PluginType::CLAP) ? tokens.text.badgeClap :
                                                                                  tokens.text.badgeInternal;
                dl->AddRectFilled(badgePos, ImVec2(badgePos.x + 48.0f, badgePos.y + 20.0f), tokens.surfaces.frameBg, 3.0f);
                dl->AddRect(badgePos, ImVec2(badgePos.x + 48.0f, badgePos.y + 20.0f), tokens.borders.subtle, 3.0f);
                ImVec2 bTextSize = ImGui::CalcTextSize(desc->typeString().c_str());
                dl->AddText(ImVec2(badgePos.x + (48.0f - bTextSize.x) * 0.5f, badgePos.y + (20.0f - bTextSize.y) * 0.5f),
                            badgeCol, desc->typeString().c_str());

                // Plugin Name & Vendor
                float textX = rowStart.x + 66.0f;
                float nameY = rowStart.y + 4.0f;
                dl->AddText(ImVec2(textX, nameY), tokens.text.primary, desc->name.c_str());

                float vendorY = rowStart.y + 20.0f;
                dl->AddText(ImVec2(textX, vendorY), tokens.text.secondary, desc->vendor.c_str());

                // Category Pill (Right-aligned)
                if (!desc->category.empty()) {
                    ImVec2 catSize = ImGui::CalcTextSize(desc->category.c_str());
                    float catPillW = catSize.x + 14.0f;
                    float catPillX = rowEnd.x - catPillW - 12.0f;
                    ImVec2 catMin = ImVec2(catPillX, rowStart.y + 9.0f);
                    ImVec2 catMax = ImVec2(catPillX + catPillW, rowStart.y + 29.0f);
                    dl->AddRectFilled(catMin, catMax, tokens.surfaces.frameBg, 4.0f);
                    dl->AddRect(catMin, catMax, tokens.borders.subtle, 4.0f);
                    dl->AddText(ImVec2(catMin.x + 7.0f, catMin.y + (20.0f - catSize.y) * 0.5f),
                                tokens.text.muted, desc->category.c_str());
                }

                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        // 4. Bottom Footer & Status Bar (Height: ~30px)
        ImGui::SetCursorPos(ImVec2(12.0f, 386.0f));
        ImGui::Separator();
        ImGui::SetCursorPos(ImVec2(12.0f, 394.0f));

        const char* targetLabel = (m_targetBlockIndex < 0) ? "Target: Serial Insertion Slot" :
                                  (m_targetBranchIndex == 0) ? "Target: Parallel Branch A" :
                                                               "Target: Parallel Branch B";
        ImGui::TextColored(tokens.text.accent.vec4, "%s", targetLabel);

        ImGui::SameLine(310.0f);
        ImGui::TextDisabled("[^v] Navigate  [Enter] Insert  [Esc] Close");

        ImGui::End();
    }

    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

} // namespace praccy::ui
```

---

### 4.2 Feature 23: Native Win32 Folder Picker Implementation

#### Standalone COM Dialog Utility: `src/utils/native_dialogs.h`

```cpp
#pragma once

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <string>
#include <optional>
#include <filesystem>

namespace praccy::utils {

class ScopedComInitializer {
public:
    ScopedComInitializer() noexcept {
        m_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }
    ~ScopedComInitializer() {
        if (SUCCEEDED(m_hr)) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool succeeded() const noexcept {
        return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE;
    }
    [[nodiscard]] HRESULT hr() const noexcept { return m_hr; }

    ScopedComInitializer(const ScopedComInitializer&) = delete;
    ScopedComInitializer& operator=(const ScopedComInitializer&) = delete;

private:
    HRESULT m_hr{E_FAIL};
};

inline std::optional<std::string> pickFolderNative(HWND parentHwnd = nullptr) {
    ScopedComInitializer comInit;
    if (!comInit.succeeded()) {
        return std::nullopt;
    }

    using Microsoft::WRL::ComPtr;
    ComPtr<IFileOpenDialog> fileDialog;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOpenDialog,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&fileDialog)
    );
    if (FAILED(hr) || !fileDialog) {
        return std::nullopt;
    }

    // Configure options: Folder Picker mode with filesystem validation
    FILEOPENDIALOGOPTIONS options = 0;
    hr = fileDialog->GetOptions(&options);
    if (SUCCEEDED(hr)) {
        fileDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    fileDialog->SetTitle(L"Select Audio Plugin Directory");

    // Show modal dialog to parent window (audio stream on MMCSS continues uninterrupted)
    hr = fileDialog->Show(parentHwnd);
    if (FAILED(hr)) {
        // User cancelled (HRESULT_FROM_WIN32(ERROR_CANCELLED) 0x800704C7) or closed dialog
        return std::nullopt;
    }

    ComPtr<IShellItem> shellItem;
    hr = fileDialog->GetResult(&shellItem);
    if (FAILED(hr) || !shellItem) {
        return std::nullopt;
    }

    PWSTR pszPath = nullptr;
    hr = shellItem->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    if (FAILED(hr) || !pszPath) {
        return std::nullopt;
    }

    // Convert wide string to UTF-8
    std::string resultUtf8;
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len > 1) {
        resultUtf8.resize(static_cast<size_t>(utf8Len - 1));
        WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, resultUtf8.data(), utf8Len, nullptr, nullptr);
    }

    // Release Shell memory allocation
    CoTaskMemFree(pszPath);

    if (resultUtf8.empty()) {
        return std::nullopt;
    }

    // Normalize path separators
    std::filesystem::path p(resultUtf8);
    return p.lexically_normal().string();
}

} // namespace praccy::utils
#endif
```

#### Settings Modal Integration: `src/ui/modals/settings_modal.cpp`

```cpp
#include "settings_modal.h"
#include "../../utils/native_dialogs.h"
#include "../design_tokens.h"
#include "../../state/app_config.h"
#include <imgui.h>

void SettingsModal::renderPluginsTab() {
    const auto& tokens = themeTokens();

    ImGui::Spacing();
    ImGui::TextColored(tokens.text.accent.vec4, "PLUGIN SEARCH DIRECTORIES");
    ImGui::Separator();
    ImGui::Spacing();

    const auto paths = m_scanner.searchPaths();
    ImGui::BeginChild("SearchPathsListChild", ImVec2(0, 160), true);
    for (size_t i = 0; i < paths.size(); ++i) {
        ImGui::TextDisabled("[%zu]", i + 1);
        ImGui::SameLine(0, 8);
        ImGui::TextUnformatted(paths[i].c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 70.0f);

        char rmId[32];
        std::snprintf(rmId, sizeof(rmId), "Remove##%zu", i);
        if (ImGui::Button(rmId, ImVec2(65, 20))) {
            m_scanner.removeCustomSearchPath(i);
            m_scanner.scanAll();
            state::AppConfig cfg;
            cfg.load();
            cfg.customPluginPaths = m_scanner.searchPaths();
            cfg.save();
        }
    }
    ImGui::EndChild();

    ImGui::Spacing();
    // Modern Win32 IFileOpenDialog trigger button
    if (ImGui::Button("+ Browse Folder (Native Win32)...", ImVec2(240, 28))) {
        HWND parentHwnd = static_cast<HWND>(ImGui::GetMainViewport()->PlatformHandleRaw);
        auto selectedFolder = praccy::utils::pickFolderNative(parentHwnd);
        if (selectedFolder.has_value() && !selectedFolder->empty()) {
            const std::string& folderPath = *selectedFolder;
            if (std::filesystem::is_directory(folderPath)) {
                m_scanner.addCustomSearchPath(folderPath);
                m_scanner.scanAll();
                state::AppConfig cfg;
                cfg.load();
                cfg.customPluginPaths = m_scanner.searchPaths();
                cfg.save();
            }
        }
    }

    ImGui::SameLine(0, 12);
    if (ImGui::Button("Rescan All Plugins", ImVec2(160, 28))) {
        m_scanner.scanAll();
    }
}
```

---

## 5. Verification Method

### 5.1 Automated Unit Tests in `tests/test_praccy.cpp`

The test suite should be augmented with explicit unit tests validating the fuzzy search algorithm, recents tracking, and path normalization:

```cpp
void testFuzzyPluginSearchAndScoring() {
    std::cout << "[TEST] Spotlight Command Palette Fuzzy Search Scoring... ";

    using namespace praccy::ui;
    // 1. Exact match test
    assert(calculateFuzzyScore("Praccy Amp Sim", "Praccy Amp Sim") == 1000);

    // 2. Prefix match test
    assert(calculateFuzzyScore("Praccy Drive", "prac") == 500);

    // 3. Word boundary test
    int boundaryScore = calculateFuzzyScore("FabFilter Pro-Q 3", "pro");
    int midScore = calculateFuzzyScore("FabFilter Pro-Q 3", "ter");
    assert(boundaryScore > midScore);

    // 4. Subsequence fuzzy matching
    int ndScore = calculateFuzzyScore("Neural DSP", "nd");
    assert(ndScore > 0);

    // 5. Non-matching query
    assert(calculateFuzzyScore("Stereo Delay", "xyz") == -1);

    std::cout << "PASSED\n";
}

void testSearchPathNormalizationAndPersistence() {
    std::cout << "[TEST] Native Search Path Deduplication & Config Persistence... ";

    plugins::PluginScanner scanner;
    size_t initialCount = scanner.searchPaths().size();

    const std::string testDir = "C:\\TestAudioPlugins";
    scanner.addCustomSearchPath(testDir);
    assert(scanner.searchPaths().size() == initialCount + 1);

    // Assert deduplication
    scanner.addCustomSearchPath(testDir);
    assert(scanner.searchPaths().size() == initialCount + 1);

    // Remove
    scanner.removeCustomSearchPath(scanner.searchPaths().size() - 1);
    assert(scanner.searchPaths().size() == initialCount);

    std::cout << "PASSED\n";
}
```

### 5.2 Build & Test Verification Commands
1. Build the complete test suite:
   ```pwsh
   cmake --build f:\Projects\Praccy\build --config Release --target test_praccy
   ```
2. Run CTest regression tests:
   ```pwsh
   ctest --test-dir f:\Projects\Praccy\build --output-on-failure
   ```
3. Run hardcoded color scanner to assert 0 raw `IM_COL32` in new modal units:
   ```pwsh
   python f:\Projects\Praccy\scripts\check_hardcoded_colors.py
   ```

### 5.3 Manual UI Acceptance Verification Checklist
1. **Shortcut Activation**:
   - In running Praccy window, press `Ctrl+P`.
   - Verify that the 560x420 Spotlight Command Palette opens immediately at dead center of the screen with a darkened backdrop.
2. **Auto-Focus**:
   - Verify the search input cursor is immediately blinking and focused without mouse interaction.
3. **Fuzzy Search**:
   - Type `"nd"` or `"amp"` or `"vst3"` and verify relevant plugins instantly filter into the list.
4. **Keyboard Navigation**:
   - Press Down Arrow and Up Arrow; verify the selection highlight navigates the items and automatically scrolls the view.
   - Press Enter; verify the highlighted plugin is instantiated into the serial slot and the palette closes.
5. **Slot Targeting**:
   - Click `+` on Parallel Branch A; press `Ctrl+P`; verify target footer indicates "Target: Parallel Branch A".
6. **Native Win32 Folder Picker**:
   - Open Settings Modal -> "Plugins" tab.
   - Click `+ Browse Folder (Native Win32)...`.
   - Verify the authentic Windows Shell `IFileOpenDialog` folder picker opens modal to the window.
   - Select a folder and click "Select Folder"; verify the folder path is appended to the registered search list and persisted in `config.ini`.
   - Click "Cancel"; verify the dialog dismisses smoothly without errors.
