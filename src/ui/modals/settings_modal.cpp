#include "settings_modal.h"
#include "../design_tokens.h"
#include "../theme.h"
#include "../ui_helpers.h"
#include "../update_checker.h"
#include "../../state/app_config.h"
#include "../../plugins/plugin_scanner.h"
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

std::optional<std::string> openNativeFolderPicker(HWND parentHwnd) {
    ScopedComInitializer com;
    if (!com.succeeded()) return std::nullopt;

    Microsoft::WRL::ComPtr<IFileOpenDialog> fileDialog;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fileDialog));
    if (FAILED(hr)) return std::nullopt;

    FILEOPENDIALOGOPTIONS options = 0;
    if (SUCCEEDED(fileDialog->GetOptions(&options))) {
        fileDialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    }
    fileDialog->SetTitle(L"Select Audio Plugin Directory");

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
                    state::AppConfig cfg;
                    cfg.load();
                    cfg.lastAsioDriver = m_cachedDrivers[i].name;
                    cfg.save();
                }
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine(0, 10);
    if (CenteredButton("Rescan Drivers", ImVec2(120, 26))) {
        m_cachedDrivers = audio::AsioManager::enumerateDrivers();
    }

    ImGui::Spacing();
    if (CenteredButton("Open ASIO Control Panel", ImVec2(220, 26))) {
        m_asio.openControlPanel();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open the native hardware manufacturer's ASIO control panel (buffer size, latency)");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(tokens.text.primary.vec4, "Hardware Status:");
    if (m_asio.isRunning()) {
        double sRate = m_asio.currentSampleRate();
        int bSize = m_asio.currentBufferSize();
        double latMs = (sRate > 0.0) ? (static_cast<double>(bSize) / sRate * 1000.0) : 0.0;
        ImGui::BulletText("Sample Rate: %.0f Hz", sRate);
        ImGui::BulletText("Buffer Size: %d samples", bSize);
        ImGui::BulletText("Round-trip Latency: %.2f ms", latMs);
        ImGui::BulletText("Audio Engine State: ACTIVE (Real-time ASIO streaming)");
    } else {
        ImGui::BulletText("Audio Engine State: STOPPED (Driver loaded: %s)", m_asio.isLoaded() ? "Yes" : "No");
    }
}

void SettingsModal::renderPluginsTab() {
    const auto& tokens = themeTokens();
    ImGui::Spacing();
    ImGui::TextColored(tokens.text.accent.vec4, "PLUGIN MANAGER & DIRECTORIES");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Registered Plugins: %zu scanned", m_scanner.numPlugins());
    ImGui::Text("Search Directories: %zu configured", m_scanner.searchPaths().size());
    ImGui::Spacing();

    if (CenteredButton("Open Plugin Browser Overlay...", ImVec2(240, 28))) {
        if (m_onOpenPluginBrowser) {
            m_onOpenPluginBrowser();
        }
    }

    ImGui::SameLine(0, 10);
    if (CenteredButton("Rescan All Plugins", ImVec2(140, 28))) {
        m_scanner.scanAll();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(tokens.text.accent.vec4, "CUSTOM SEARCH PATHS:");
    ImGui::Spacing();

    // Native folder picker button
    if (CenteredButton("+ Add Directory (Browse)...", ImVec2(200, 26))) {
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
    ImGui::BeginChild("##SearchPathsChild", ImVec2(0.0f, 120.0f), true);
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

    ImGui::Text("Installed Version: %s", "v2.0.0");
    ImGui::Spacing();

    state::AppConfig cfg;
    cfg.load();
    bool includeBeta = cfg.checkBetaUpdates;
    if (ImGui::Checkbox("Enable beta builds from 'dev' branch on GitHub", &includeBeta)) {
        cfg.checkBetaUpdates = includeBeta;
        cfg.save();
    }

    ImGui::Spacing();
    if (CenteredButton("Check for Updates Now...", ImVec2(200, 28))) {
        UpdateChecker::instance().checkForUpdates(includeBeta);
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Updates are verified via SHA-256 and extracted securely in-process.");
}

void SettingsModal::renderAboutTab() {
    const auto& tokens = themeTokens();
    ImGui::Spacing();
    ImGui::TextColored(tokens.text.accent.vec4, "PRACCY v2.0.0");
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
    if (CenteredButton("GitHub Repository ->", ImVec2(180, 26))) {
#if defined(_WIN32)
        ShellExecuteA(nullptr, "open", "https://github.com/praccy/praccy", nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
}

void SettingsModal::render() {
    if (!m_isOpen) return;

    ImGui::OpenPopup("Praccy Settings##Modal");
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Praccy Settings##Modal", &m_isOpen, ImGuiWindowFlags_NoResize)) {
        const auto& tokens = themeTokens();
        ImGui::TextColored(tokens.text.accent.vec4, "PRACCY PREFERENCES");
        ImGui::Separator();
        ImGui::Spacing();

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

        float bottomY = std::max(ImGui::GetCursorPosY() + 12.0f, 435.0f);
        ImGui::SetCursorPosY(bottomY);
        ImGui::Separator();
        if (CenteredButton("Close", ImVec2(90, 26))) {
            close();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

} // namespace praccy::ui
