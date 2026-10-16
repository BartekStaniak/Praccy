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
    ImGui::TextColored(tokens.text.accent.vec4, "ASIO DRIVER CONFIGURATION");
    ImGui::Separator();
    ImGui::Spacing();

    // Driver selector combo
    std::string currentDriverName = m_asio.driverInfo().name;
    if (currentDriverName.empty()) currentDriverName = "Select ASIO Driver...";

    ImGui::Text("Active ASIO Device:");
    ImGui::SetNextItemWidth(340);
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

    ImGui::SameLine(0, 10);
    if (CenteredButton("Rescan Drivers", ImVec2(120, Metrics::buttonHeight()))) {
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

    ImGui::SetNextItemWidth(340);
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

    ImGui::SetNextItemWidth(340);
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

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

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
    ImGui::BulletText("Live RAM Footprint: %.1f MB (%.0f Hz stereo 32-bit float)", ramMb, sRate);
    ImGui::TextDisabled("Continuously records audio in a lock-free circular buffer. Press Ctrl+Shift+R to export.");

    ImGui::Spacing();
    ImGui::Text("Save Destination Directory:");

    std::string displayDir = m_cachedConfig.riffDirectory;
    if (displayDir.empty()) {
        const char* up = std::getenv("USERPROFILE");
        displayDir = (up && *up) ? (std::filesystem::path(up) / "Documents" / "Praccy" / "Riffs").string()
                                 : "Documents/Praccy/Riffs";
    }

    const float widgetH = Metrics::buttonHeight();
    const float btnBrowseW = 85.0f;
    const float btnOpenW = 60.0f;
    const float btnResetW = (!m_cachedConfig.riffDirectory.empty()) ? 60.0f : 0.0f;
    const float spacingX = Metrics::kItemSpacingX;
    const float totalBtnsW = btnBrowseW + spacingX + btnOpenW + (btnResetW > 0.0f ? (spacingX + btnResetW) : 0.0f);
    const float availW = ImGui::GetContentRegionAvail().x;
    const float inputW = std::max(180.0f, availW - totalBtnsW - spacingX);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, tokens.surfaces.cardBg.vec4);
    char dirBuf[512];
    std::strncpy(dirBuf, displayDir.c_str(), sizeof(dirBuf));
    dirBuf[sizeof(dirBuf) - 1] = '\0';
    ImGui::SetNextItemWidth(inputW);
    ImGui::InputText("##RiffSaveDir", dirBuf, sizeof(dirBuf), ImGuiInputTextFlags_ReadOnly);
    ImGui::PopStyleColor();

    ImGui::SameLine(0, spacingX);
    if (ImGui::Button("Browse...##RiffDir", ImVec2(btnBrowseW, widgetH))) {
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
            ImGui::BulletText("%s", paths[i].c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

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

    for (int i = 0; i < 4; ++i) {
        if (ImGui::RadioButton(themes[i], themeIdx == i)) {
            applyTheme(static_cast<ThemeId>(i));
            state::AppConfig cfg;
            cfg.load();
            cfg.themeId = i;
            cfg.save();
        }
        ImGui::Spacing();
    }

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
