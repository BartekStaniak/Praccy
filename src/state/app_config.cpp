#include "app_config.h"
#include "../utils/parse_utils.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <algorithm>

namespace praccy::state {

std::string AppConfig::getConfigDir() {
    const char* localApp = std::getenv("LOCALAPPDATA");
    std::filesystem::path dir;
    if (localApp) {
        dir = std::filesystem::path(localApp) / "Praccy";
    } else {
        dir = std::filesystem::current_path();
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

std::string AppConfig::getConfigFilePath() {
    return (std::filesystem::path(getConfigDir()) / "config.ini").string();
}

bool AppConfig::load(const std::string& customPath) {
    std::string path = customPath.empty() ? getConfigFilePath() : customPath;
    std::ifstream file(path);
    if (!file.is_open()) return false;

    customPluginPaths.clear();
    favoritePlugins.clear();
    recentPlugins.clear();
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        auto eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;

        std::string key = line.substr(0, eqPos);
        std::string val = line.substr(eqPos + 1);

        // Trim whitespace
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());

        if (key == "last_asio_driver") {
            lastAsioDriver = val;
        } else if (key == "input_mode") {
            inputMode = static_cast<audio::InputRoutingMode>(
                utils::parseInteger<int>(val, static_cast<int>(audio::InputRoutingMode::MonoLeft)));
        } else if (key == "input_gain_db") {
            inputGainDb = utils::parseFloat(val, 0.0f);
        } else if (key == "master_volume_db") {
            masterVolumeDb = utils::parseFloat(val, 0.0f);
        } else if (key == "metronome_bpm") {
            metronomeBpm = utils::parseFloat(val, 120.0f);
        } else if (key == "check_beta_updates") {
            checkBetaUpdates = (val == "1" || val == "true");
        } else if (key == "custom_plugin_path") {
            customPluginPaths.push_back(val);
        } else if (key == "favorite_plugin") {
            favoritePlugins.push_back(val);
        } else if (key == "recent_plugin") {
            recentPlugins.push_back(val);
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
    }
    return true;
}

bool AppConfig::save(const std::string& customPath) const {
    std::string path = customPath.empty() ? getConfigFilePath() : customPath;
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "# Praccy Configuration\n";
    file << "last_asio_driver=" << lastAsioDriver << "\n";
    file << "input_mode=" << static_cast<int>(inputMode) << "\n";
    file << "input_gain_db=" << inputGainDb << "\n";
    file << "master_volume_db=" << masterVolumeDb << "\n";
    file << "metronome_bpm=" << metronomeBpm << "\n";
    file << "check_beta_updates=" << (checkBetaUpdates ? "1" : "0") << "\n";
    file << "window_x=" << windowX << "\n";
    file << "window_y=" << windowY << "\n";
    file << "window_w=" << windowW << "\n";
    file << "window_h=" << windowH << "\n";
    file << "window_maximized=" << (windowMaximized ? "1" : "0") << "\n";

    for (const auto& cp : customPluginPaths) {
        file << "custom_plugin_path=" << cp << "\n";
    }

    for (const auto& fp : favoritePlugins) {
        file << "favorite_plugin=" << fp << "\n";
    }

    for (const auto& rp : recentPlugins) {
        file << "recent_plugin=" << rp << "\n";
    }

    return true;
}

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

} // namespace praccy::state
