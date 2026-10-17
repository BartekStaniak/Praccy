#include "settings_modal.h"
#include "../design_tokens.h"
#include "../theme.h"
#include "../ui_helpers.h"
#include "../update_checker.h"
#include "../../state/app_config.h"
#include "../../plugins/plugin_scanner.h"
#include "../../version.h"
#include <imgui.h>
#include <filesystem>
#include <optional>
#include <algorithm>

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
#include <shellapi.h>
#endif

namespace praccy::ui {

namespace {

#if defined(_WIN32)
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

std::optional<std::string> openNativeFolderPicker(HWND parentHwnd, const wchar_t* title = L"Select Directory") {
    ScopedComInitializer com;
    if (!com.succeeded()) return std::nullopt;

    // Apply dark mode preference for Win32 common dialogs (Windows 10 1809+ / Windows 11)
    const auto& tokens = themeTokens();
    if (HMODULE hUxTheme = LoadLibraryW(L"uxtheme.dll")) {
        using PFN_SetPreferredAppMode = int (WINAPI*)(int);
        FARPROC proc = GetProcAddress(hUxTheme, MAKEINTRESOURCEA(135));
        if (proc) {
            PFN_SetPreferredAppMode pfnSetPreferredAppMode = nullptr;
            std::memcpy(&pfnSetPreferredAppMode, &proc, sizeof(proc));
            if (pfnSetPreferredAppMode) {
                pfnSetPreferredAppMode(tokens.isDark ? 2 /* ForceDark */ : 3 /* ForceLight */);
            }
        }
        FreeLibrary(hUxTheme);
    }

    Microsoft::WRL::ComPtr<IFileOpenDialog> fileDialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fileDialog));
    if (FAILED(hr)) return std::nullopt;

    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(fileDialog->GetOptions(&options))) {
        fileDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    fileDialog->SetTitle(title ? title : L"Select Directory");

    if (!parentHwnd) {
        parentHwnd = GetActiveWindow();
    }

    hr = fileDialog->Show(parentHwnd);
    if (FAILED(hr)) {
        // User cancelled or closed dialog gracefully
        return std::nullopt;
    }

    Microsoft::WRL::ComPtr<IShellItem> item;
    hr = fileDialog->GetResult(&item);
    if (FAILED(hr) || !item) return std::nullopt;

    PWSTR pszPath = nullptr;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath);
    if (FAILED(hr) || !pszPath) return std::nullopt;

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, nullptr, 0, nullptr, nullptr);
    std::string resultPath;
    if (utf8Len > 0) {
        resultPath.resize(utf8Len - 1);
        WideCharToMultiByte(CP_UTF8, 0, pszPath, -1, resultPath.data(), utf8Len, nullptr, nullptr);
    }

    CoTaskMemFree(pszPath);
    return resultPath.empty() ? std::nullopt : std::make_optional(resultPath);
}
#endif

} // namespace

SettingsModal::SettingsModal(audio::AsioManager& asio,
                             plugins::PluginScanner& scanner,
                             std::function<void()> onOpenPluginBrowser)
    : m_asio(asio)
    , m_scanner(scanner)
    , m_onOpenPluginBrowser(std::move(onOpenPluginBrowser))
{
    m_cachedDrivers = audio::AsioManager::enumerateDrivers();
}

SettingsModal::~SettingsModal() = default;

void SettingsModal::open() {
    m_isOpen = true;
    m_cachedConfig.load();
    m_cachedDrivers = audio::AsioManager::enumerateDrivers();
    std::string currentDriver = m_asio.driverInfo().name;
    for (int i = 0; i < static_cast<int>(m_cachedDrivers.size()); ++i) {
        if (m_cachedDrivers[i].name == currentDriver) {
            m_selectedDriverIdx = i;
            break;
        }
    }
}

void SettingsModal::close() {
    m_isOpen = false;
}

void SettingsModal::renderAudioTab() {
    const auto& tokens = themeTokens();
    ImGui::Spacing();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(14.0f, 4.0f));
    if (ImGui::BeginTable("##AudioSettingsTwoCol", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("AsioCol", ImGuiTableColumnFlags_WidthStretch, 0.48f);
        ImGui::TableSetupColumn("RiffCol", ImGuiTableColumnFlags_WidthStretch, 0.52f);
        ImGui::TableNextColumn();
    }
    ImGui::TextColored(tokens.text.accent.vec4, "ASIO DRIVER CONFIGURATION");
    ImGui::Separator();
    ImGui::Spacing();

    // Driver selector combo (responsive width so "Rescan Drivers" never collides with column divider)
    std::string currentDriverName = m_asio.driverInfo().name;
    if (currentDriverName.empty()) currentDriverName = "Select ASIO Driver...";

    ImGui::Text("Active ASIO Device:");
    const float rescanBtnW = 116.0f;
    const float colAvailW = ImGui::GetContentRegionAvail().x;
    const float comboW = std::max(140.0f, colAvailW - rescanBtnW - 10.0f);
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::BeginCombo("##SettingsAsioDriversCombo", currentDriverName.c_str())) {
        for (int i = 0; i < static_cast<int>(m_cachedDrivers.size()); ++i) {
            const bool isSelected = (m_selectedDriverIdx == i);
            if (ImGui::Selectable(m_cachedDrivers[i].name.c_str(), isSelected)) {
                m_selectedDriverIdx = i;
                if (m_asio.loadDriver(m_cachedDrivers[i])) {
                    m_cachedConfig.lastAsioDriver = m_cachedDrivers[i].name;
                    m_cachedConfig.save();
                    double sRate = m_asio.currentSampleRate();
                    if (sRate <= 0.0) sRate = 48000.0;
                    const auto& dInfo = m_asio.driverInfo();
                    uint32_t bufSize = (dInfo.preferredBufferSize > 0) ? static_cast<uint32_t>(dInfo.preferredBufferSize) : 256;
                    if (m_onDriverChanged) {
                        m_onDriverChanged(sRate, bufSize);
                    } else {
                        m_asio.start(bufSize);
                    }
                }
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine(0, 8);
    if (CenteredButton("Rescan Drivers", ImVec2(rescanBtnW, Metrics::buttonHeight()))) {
        m_cachedDrivers = audio::AsioManager::enumerateDrivers();
    }

    ImGui::Spacing();
    if (CenteredButton("Open ASIO Control Panel", ImVec2(220, Metrics::buttonHeight()))) {
        m_asio.openControlPanel();
        if (m_asio.checkHardwareChanges()) {
            if (m_onDriverChanged) {
                m_onDriverChanged(m_asio.currentSampleRate(), static_cast<uint32_t>(m_asio.currentBufferSize()));
            }
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open the native hardware manufacturer's ASIO control panel (buffer size, latency)");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(tokens.text.primary.vec4, "Hardware Configuration & Status:");
    ImGui::Spacing();

    const bool hasDriver = m_asio.isLoaded();
    const double curSr = m_asio.currentSampleRate();
    const int curBuf = m_asio.currentBufferSize();
    const double latMs = (curSr > 0.0) ? (static_cast<double>(curBuf) / curSr * 1000.0) : 0.0;

    // Sample Rate Selector
    ImGui::Text("Sample Rate:");
    char srPreview[64];
    if (hasDriver && curSr > 0.0) {
        const char* srFmt = (std::fmod(curSr, 1000.0) == 0.0) ? "%.0f Hz (%.0f kHz)" : "%.0f Hz (%.1f kHz)";
        std::snprintf(srPreview, sizeof(srPreview), srFmt, curSr, curSr / 1000.0);
    } else {
        std::snprintf(srPreview, sizeof(srPreview), "48000 Hz (48 kHz)");
    }

    ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 4.0f));
    if (ImGui::BeginCombo("##SettingsSampleRateCombo", srPreview, hasDriver ? 0 : ImGuiComboFlags_NoArrowButton)) {
        if (hasDriver) {
            constexpr double standardRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
            for (double rate : standardRates) {
                const bool isSupported = m_asio.canSampleRate(rate);
                const bool isSelected = (std::abs(rate - curSr) < 1.0);
                const char* rFmt = (std::fmod(rate, 1000.0) == 0.0) ? "%.0f Hz (%.0f kHz)" : "%.0f Hz (%.1f kHz)";
                char rateLabel[96];
                char tmp[64];
                std::snprintf(tmp, sizeof(tmp), rFmt, rate, rate / 1000.0);
                if (isSupported) {
                    std::snprintf(rateLabel, sizeof(rateLabel), "%s##sr_%.0f", tmp, rate);
                } else {
                    std::snprintf(rateLabel, sizeof(rateLabel), "%s [Unsupported]##sr_%.0f", tmp, rate);
                }

                ImGuiSelectableFlags flags = isSupported ? 0 : ImGuiSelectableFlags_Disabled;
                if (ImGui::Selectable(rateLabel, isSelected, flags)) {
                    if (isSupported && !isSelected) {
                        if (m_onDriverChanged) {
                            m_onDriverChanged(rate, static_cast<uint32_t>(curBuf > 0 ? curBuf : 256));
                        } else {
                            m_asio.setSampleRate(rate);
                        }
                    }
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    // Buffer Size (Latency) Selector
    ImGui::Text("Buffer Size (Latency):");
    char bufPreview[64];
    if (hasDriver && curBuf > 0) {
        std::snprintf(bufPreview, sizeof(bufPreview), "%d samples (%.2f ms latency)", curBuf, latMs);
    } else {
        std::snprintf(bufPreview, sizeof(bufPreview), "256 samples (5.33 ms latency)");
    }

    ImGui::SetNextItemWidth(std::max(160.0f, ImGui::GetContentRegionAvail().x - 4.0f));
    if (ImGui::BeginCombo("##SettingsBufferSizeCombo", bufPreview, hasDriver ? 0 : ImGuiComboFlags_NoArrowButton)) {
        if (hasDriver) {
            constexpr int standardSizes[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096 };
            const auto& dInfo = m_asio.driverInfo();
            for (int sz : standardSizes) {
                bool inRange = true;
                if (dInfo.minBufferSize > 0 && sz < dInfo.minBufferSize) inRange = false;
                if (dInfo.maxBufferSize > 0 && sz > dInfo.maxBufferSize) inRange = false;
                const bool isSelected = (sz == curBuf);
                const double szLatMs = (curSr > 0.0) ? (static_cast<double>(sz) / curSr * 1000.0) : 0.0;
                char szLabel[96];
                if (inRange) {
                    std::snprintf(szLabel, sizeof(szLabel), "%d samples (%.2f ms)##buf_%d", sz, szLatMs, sz);
                } else {
                    std::snprintf(szLabel, sizeof(szLabel), "%d samples [Out of range]##buf_%d", sz, sz);
                }

                ImGuiSelectableFlags flags = inRange ? 0 : ImGuiSelectableFlags_Disabled;
                if (ImGui::Selectable(szLabel, isSelected, flags)) {
                    if (inRange && !isSelected) {
                        if (m_onDriverChanged) {
                            m_onDriverChanged(curSr > 0.0 ? curSr : 48000.0, static_cast<uint32_t>(sz));
                        } else {
                            m_asio.setBufferSize(sz);
                        }
                    }
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();
    if (m_asio.isRunning()) {
        ImGui::BulletText("Round-trip Latency: %.2f ms", latMs);
        ImGui::BulletText("Audio Engine State: ACTIVE (Real-time ASIO streaming)");
    } else {
        ImGui::BulletText("Audio Engine State: STOPPED (Driver loaded: %s)", m_asio.isLoaded() ? "Yes" : "No");
    }

    if (ImGui::GetCurrentTable() != nullptr) {
        ImGui::TableNextColumn();
    } else {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }

    ImGui::TextColored(tokens.text.accent.vec4, "RETROACTIVE RIFF RECORDER");
    ImGui::Separator();
    ImGui::Spacing();

    uint32_t currentMinutes = m_cachedConfig.retroactiveBufferMinutes;
    if (currentMinutes != 1 && currentMinutes != 2 && currentMinutes != 5 && currentMinutes != 10) {
        currentMinutes = 2;
    }

    ImGui::Text("Circular Audio Buffer Duration:");
    const uint32_t durations[] = {1, 2, 5, 10};
    for (int d = 0; d < 4; ++d) {
        char radioLabel[32];
        std::snprintf(radioLabel, sizeof(radioLabel), "%u min##RiffDur", durations[d]);
        if (d > 0) ImGui::SameLine(0, 16);
        if (ImGui::RadioButton(radioLabel, currentMinutes == durations[d])) {
            m_cachedConfig.retroactiveBufferMinutes = durations[d];
            m_cachedConfig.save();
            currentMinutes = durations[d];
        }
    }

    double sRate = m_asio.isRunning() ? m_asio.currentSampleRate() : 48000.0;
    if (sRate <= 0.0) sRate = 48000.0;
    double ramBytes = static_cast<double>(currentMinutes) * 60.0 * sRate * 2.0 * static_cast<double>(sizeof(float));
    double ramMb = ramBytes / (1024.0 * 1024.0);

    ImGui::Spacing();
    ImGui::BulletText("RAM Footprint: %.1f MB (%.0f kHz 32-bit stereo)", ramMb, sRate / 1000.0);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextColored(tokens.text.muted.vec4, "Continuously records audio in a lock-free circular buffer. Press Ctrl+Shift+R to export.");
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Text("Save Destination Directory:");

    std::string displayDir = m_cachedConfig.riffDirectory;
    if (displayDir.empty()) {
        const char* up = std::getenv("USERPROFILE");
        displayDir = (up && *up) ? (std::filesystem::path(up) / "Documents" / "Praccy" / "Riffs").string()
                                 : "Documents/Praccy/Riffs";
    }

    const float widgetH = Metrics::buttonHeight();
    const float btnBrowseW = 92.0f;
    const float btnOpenW = 82.0f;
    const float btnResetW = (!m_cachedConfig.riffDirectory.empty()) ? 70.0f : 0.0f;
    const float spacingX = Metrics::kItemSpacingX;

    // Place full-width path readout on its own row and action buttons directly beneath so the path never clips
    ImGui::PushStyleColor(ImGuiCol_FrameBg, tokens.surfaces.cardBg.vec4);
    char dirBuf[512];
    std::strncpy(dirBuf, displayDir.c_str(), sizeof(dirBuf));
    dirBuf[sizeof(dirBuf) - 1] = '\0';
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##RiffSaveDir", dirBuf, sizeof(dirBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", displayDir.c_str());
    }

    ImGui::Spacing();
    if (CenteredButton("Browse...##RiffDir", ImVec2(btnBrowseW, widgetH))) {
#if defined(_WIN32)
        HWND hwnd = nullptr;
        if (ImGui::GetCurrentContext()) {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            if (vp && vp->PlatformHandleRaw) {
                hwnd = reinterpret_cast<HWND>(vp->PlatformHandleRaw);
            }
        }
        auto chosen = openNativeFolderPicker(hwnd, L"Select Riff Destination Directory");
        if (chosen.has_value() && !chosen->empty()) {
            std::filesystem::path norm(*chosen);
            m_cachedConfig.riffDirectory = norm.lexically_normal().string();
            m_cachedConfig.save();
        }
#endif
    }

    ImGui::SameLine(0, spacingX);
    if (ImGui::Button("Open##RiffDirOpen", ImVec2(btnOpenW, widgetH))) {
#if defined(_WIN32)
        try {
            std::filesystem::create_directories(displayDir);
        } catch (...) {}
        ShellExecuteA(nullptr, "open", displayDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open folder in Windows Explorer");
    }

    if (!m_cachedConfig.riffDirectory.empty()) {
        ImGui::SameLine(0, spacingX);
        if (ImGui::Button("Reset##RiffDirReset", ImVec2(btnResetW, widgetH))) {
            m_cachedConfig.riffDirectory.clear();
            m_cachedConfig.save();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Reset to default Praccy Documents/Riffs folder");
        }
    }
    if (ImGui::GetCurrentTable() != nullptr) {
        ImGui::EndTable();
    }
    ImGui::PopStyleVar(); // CellPadding
}

void SettingsModal::renderPluginsTab() {
    const auto& tokens = themeTokens();
    ImGui::Spacing();
    ImGui::TextColored(tokens.text.accent.vec4, "PLUGIN MANAGER & STATUS");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Registered Plugins: %zu scanned", m_scanner.numPlugins());
    ImGui::Text("Search Directories: %zu configured", m_scanner.searchPaths().size());
    ImGui::Spacing();

    if (CenteredButton("Open Plugin Browser Overlay...", ImVec2(240, Metrics::buttonHeight()))) {
        if (m_onOpenPluginBrowser) {
            m_onOpenPluginBrowser();
        }
    }

    ImGui::SameLine(0, 10);
    if (CenteredButton("Rescan All Plugins", ImVec2(140, Metrics::buttonHeight()))) {
        m_scanner.scanAll();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(tokens.text.accent.vec4, "CUSTOM SEARCH PATHS");
    ImGui::Separator();
    ImGui::Spacing();

    // Native folder picker button
    if (CenteredButton("+ Add Directory (Browse)...", ImVec2(200, Metrics::buttonHeight()))) {
#if defined(_WIN32)
        HWND hwnd = nullptr;
        if (ImGui::GetCurrentContext()) {
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            if (vp && vp->PlatformHandleRaw) {
                hwnd = reinterpret_cast<HWND>(vp->PlatformHandleRaw);
            }
        }
        auto pickedPath = openNativeFolderPicker(hwnd);
        if (pickedPath && !pickedPath->empty()) {
            std::filesystem::path normPath(*pickedPath);
            std::string pathStr = normPath.lexically_normal().string();

            const auto& existingPaths = m_scanner.searchPaths();
            if (std::find(existingPaths.begin(), existingPaths.end(), pathStr) == existingPaths.end()) {
                m_scanner.addCustomSearchPath(pathStr);
                m_scanner.scanAll();

                state::AppConfig cfg;
                cfg.load();
                cfg.customPluginPaths = m_scanner.searchPaths();
                cfg.save();
            }
        }
#endif
    }

    ImGui::Spacing();
    const float footerH = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 20.0f;
    const float pathsHeight = std::max(120.0f, ImGui::GetContentRegionAvail().y - footerH);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.frameBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.subtle.vec4);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Metrics::kCardPaddingX, Metrics::kCardPaddingY));
    ImGui::BeginChild("##SearchPathsChild", ImVec2(0.0f, pathsHeight), true);
    const auto& paths = m_scanner.searchPaths();
    if (paths.empty()) {
        ImGui::TextColored(tokens.text.muted.vec4, "(No custom plugin search paths added yet)");
    } else {
        for (size_t i = 0; i < paths.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            const bool isSystemDefault = (i < 6);
            ImGui::AlignTextToFramePadding();
            ImGui::BulletText("%s", paths[i].c_str());
            ImGui::SameLine(std::max(280.0f, ImGui::GetContentRegionAvail().x - (isSystemDefault ? 90.0f : 150.0f)));
            ImGui::TextDisabled(isSystemDefault ? "[System]" : "[Custom]");
            if (!isSystemDefault) {
                ImGui::SameLine(0, 8.0f);
                if (CenteredButton("Remove", ImVec2(64.0f, Metrics::smallButtonHeight()))) {
                    m_scanner.removeCustomSearchPath(i);
                    state::AppConfig cfg;
                    cfg.load();
                    cfg.customPluginPaths = m_scanner.searchPaths();
                    cfg.save();
                    ImGui::PopID();
                    break;
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    auto& m_config = m_cachedConfig;
    if (ImGui::Checkbox("Enable Live Plugin Thumbnails (CPU intensive)", &m_config.enableLiveThumbnails)) {
        m_config.save();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Formats Supported: VST3 (64-bit), CLAP (64-bit), Native DSP Effects");
    ImGui::TextDisabled("Thumbnail Cache: Auto-captures live plugin GUI previews");
}

void SettingsModal::renderThemeTab() {
    const auto& tokens = themeTokens();
    ImGui::Spacing();
    ImGui::TextColored(tokens.text.accent.vec4, "VISUAL THEME & DESIGN TOKENS");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Select Design System Theme (WCAG AA Compliant):");
    ImGui::Spacing();

    const char* themes[] = {
        "Obsidian Studio (Default)",
        "Cyber / Midnight (High Contrast)",
        "Nordic Slate (Cool Neutral)",
        "Vintage Console (Warm Analog)"
    };

    ThemeId currentTheme = themeTokens().id;
    int themeIdx = static_cast<int>(currentTheme);

    const char* themeSubtitles[] = {
        "Deep obsidian surfaces with warm amber focus and emerald metering.",
        "High-contrast midnight navy with electric cyan and magenta accents.",
        "Cool Scandinavian slate greys with arctic blue and seafoam telemetry.",
        "Warm analog console paper with terracotta and forest green meters."
    };

    const float availW = ImGui::GetContentRegionAvail().x;
    const float cardGap = 12.0f;
    const float cardW = std::max(220.0f, (availW - cardGap) * 0.5f);
    const float padX = 12.0f;
    const float padBottom = 12.0f;
    const float wrapWidth = cardW - padX * 2.0f;

    const float headerH = std::max(28.0f, ImGui::GetTextLineHeight() + 8.0f);
    const float swatchY = headerH + 10.0f;
    const float swatchH = 16.0f;
    const float subtitleY = swatchY + swatchH + 10.0f;

    float maxSubH = 0.0f;
    for (int i = 0; i < 4; ++i) {
        ImVec2 sz = ImGui::CalcTextSize(themeSubtitles[i], nullptr, false, wrapWidth);
        if (sz.y > maxSubH) {
            maxSubH = sz.y;
        }
    }
    const float cardH = std::max(110.0f, subtitleY + maxSubH + padBottom);

    for (int i = 0; i < 4; ++i) {
        if (i % 2 == 1) ImGui::SameLine(0.0f, cardGap);

        const ThemeTokens& t = getThemeTokens(static_cast<ThemeId>(i));

        ImGui::PushID(i);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##ThemePreviewCard", ImVec2(cardW, cardH))) {
            applyTheme(static_cast<ThemeId>(i));
            themeIdx = i;
            state::AppConfig cfg;
            cfg.load();
            cfg.themeId = i;
            cfg.save();
        }
        const bool isSelected = (themeIdx == i);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 borderCol = isSelected ? tokens.borders.focus.u32
                              : (hovered ? tokens.borders.cardHovered.u32 : tokens.borders.subtle.u32);
        dl->AddRectFilled(pos, ImVec2(pos.x + cardW, pos.y + cardH), t.surfaces.panelBg.u32, 6.0f);

        dl->PushClipRect(pos, ImVec2(pos.x + cardW, pos.y + cardH), true);

        // Header bar inside preview card
        dl->AddRectFilled(pos, ImVec2(pos.x + cardW, pos.y + headerH), t.surfaces.headerBg.u32, 6.0f, ImDrawFlags_RoundCornersTop);
        const float headerTextY = pos.y + (headerH - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(pos.x + padX, headerTextY), t.text.primary.u32, themes[i]);

        if (isSelected) {
            const char* activeBadge = "ACTIVE";
            const ImVec2 badgeTextSz = ImGui::CalcTextSize(activeBadge);
            const float bH = 18.0f;
            const float bW = badgeTextSz.x + 12.0f;
            const float bX = pos.x + cardW - padX - bW;
            const float bY = pos.y + (headerH - bH) * 0.5f;
            dl->AddRectFilled(ImVec2(bX, bY), ImVec2(bX + bW, bY + bH), tokens.surfaces.cardBg.u32, bH * 0.5f);
            dl->AddRect(ImVec2(bX, bY), ImVec2(bX + bW, bY + bH), tokens.borders.focus.u32, bH * 0.5f, 0, 1.0f);
            dl->AddText(ImVec2(bX + 6.0f, bY + (bH - badgeTextSz.y) * 0.5f), tokens.borders.focus.u32, activeBadge);
        }

        // 6 live color swatches (WindowBg, CardBg, Accent, Active, BranchA, BranchB)
        const ImU32 swatches[6] = {
            t.surfaces.windowBg.u32, t.surfaces.cardBg.u32, t.text.accent.u32,
            t.signal.active.u32, t.signal.branchA.u32, t.signal.branchB.u32
        };
        for (int s = 0; s < 6; ++s) {
            const float sx = pos.x + padX + static_cast<float>(s) * 28.0f;
            const float sy = pos.y + swatchY;
            dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx + 22.0f, sy + swatchH), swatches[s], 3.0f);
            dl->AddRect(ImVec2(sx, sy), ImVec2(sx + 22.0f, sy + swatchH), t.borders.subtle.u32, 3.0f);
        }

        dl->AddText(nullptr, 0.0f, ImVec2(pos.x + padX, pos.y + subtitleY), t.text.secondary.u32, themeSubtitles[i], nullptr, wrapWidth);
        dl->PopClipRect();

        // Draw selection / hover border on top of card contents so it is never occluded
        dl->AddRect(pos, ImVec2(pos.x + cardW, pos.y + cardH), borderCol, 6.0f, 0, isSelected ? 2.0f : 1.0f);

        if (i == 1) ImGui::Spacing();
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("All themes enforce 4px/8px grid geometry and >= 4.5:1 text contrast.");
}

void SettingsModal::renderUpdatesTab() {
    const auto& tokens = themeTokens();
    ImGui::Spacing();
    ImGui::TextColored(tokens.text.accent.vec4, "SOFTWARE UPDATES");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Installed Version: %s", praccy::kVersionString);
    ImGui::Spacing();

    bool includeBeta = m_cachedConfig.checkBetaUpdates;
    if (ImGui::Checkbox("Enable beta builds from 'dev' branch on GitHub", &includeBeta)) {
        m_cachedConfig.checkBetaUpdates = includeBeta;
        m_cachedConfig.save();
    }

    ImGui::Spacing();
    if (CenteredButton("Check for Updates Now...", ImVec2(200, Metrics::buttonHeight()))) {
        UpdateChecker::instance().checkForUpdates(includeBeta);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Updates are verified via SHA-256 and extracted securely in-process.");
}

void SettingsModal::renderAboutTab() {
    const auto& tokens = themeTokens();
    ImGui::Spacing();
    ImGui::TextColored(tokens.text.accent.vec4, "PRACCY %s", praccy::kVersionString);
    ImGui::TextDisabled("Professional Ultra-Low-Latency Guitar & Audio Practice Suite");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BulletText("Audio Engine: ASIO Real-time Engine with Lock-Free SPSC Queues");
    ImGui::BulletText("Plugin Isolation: SEH/VEH Hardware Access Violation Sandboxing");
    ImGui::BulletText("Design Tokens: Unified WCAG AA Design System with 4 Production Themes");
    ImGui::BulletText("Practice Suite: Decoupled 60Hz Pitch Tuner, Quick Looper, Audio Player");
    ImGui::BulletText("Preset Management: 10ms Click-Free EqualPower Crossfading");

    ImGui::Spacing();
    ImGui::Spacing();
    if (CenteredButton("GitHub Repository", ImVec2(180, Metrics::buttonHeight()))) {
#if defined(_WIN32)
        ShellExecuteA(nullptr, "open", "https://github.com/BartekStaniak/Praccy/", nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
}

void SettingsModal::render() {
    if (!m_isOpen) return;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float initW = vp ? std::clamp(vp->Size.x * 0.65f, 680.0f, 920.0f) : 700.0f;
    const float initH = vp ? std::clamp(vp->Size.y * 0.72f, 540.0f, 760.0f) : 560.0f;

    ImGui::OpenPopup("Praccy Settings##Modal");
    ImGui::SetNextWindowSize(ImVec2(initW, initH), ImGuiCond_Appearing);
    if (vp) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(580.0f, 440.0f), ImVec2(vp->Size.x * 0.95f, vp->Size.y * 0.95f));
    }
    ImGui::SetNextWindowPos(vp ? vp->GetCenter() : ImVec2(100, 100), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    const auto& tokens = themeTokens();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tokens.surfaces.popupBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.subtle.vec4);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Spec::kModalCornerRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Spec::kModalWindowPadding, Spec::kModalWindowPadding));

    if (ImGui::BeginPopupModal("Praccy Settings##Modal", &m_isOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const float footerHeight = Metrics::kFooterHeight;
        const float contentHeight = ImGui::GetContentRegionAvail().y - footerHeight;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ColorToken::transparent().vec4);
        if (ImGui::BeginChild("SettingsTabsContentRegion", ImVec2(0, contentHeight), false)) {
            if (ImGui::BeginTabBar("SettingsTabs")) {
                if (ImGui::BeginTabItem("Audio & ASIO")) {
                    renderAudioTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Plugins")) {
                    renderPluginsTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Appearance")) {
                    renderThemeTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Updates")) {
                    renderUpdatesTab();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("About")) {
                    renderAboutTab();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Separator();
        ImGui::Spacing();
        if (CenteredButton("Close", ImVec2(90, Metrics::buttonHeight()))) {
            close();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace praccy::ui
