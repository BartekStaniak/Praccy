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
    favoritePresets.clear();
    customPluginCategories.clear();
    recentPlugins.clear();
    scannedPlugins.clear();
    keyBindings.clear();
    presetFavoritesInitialized = false;
    enableLiveThumbnails = false;

    std::string currentSection;
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

        size_t firstNonWs = line.find_first_not_of(" \t");
        if (firstNonWs == std::string::npos) continue;
        char firstChar = line[firstNonWs];
        if (firstChar == '#' || firstChar == ';') continue;

        if (firstChar == '[') {
            size_t closeBracket = line.find(']', firstNonWs);
            if (closeBracket != std::string::npos) {
                currentSection = line.substr(firstNonWs + 1, closeBracket - (firstNonWs + 1));
                while (!currentSection.empty() && (currentSection.back() == ' ' || currentSection.back() == '\t')) currentSection.pop_back();
                while (!currentSection.empty() && (currentSection.front() == ' ' || currentSection.front() == '\t')) currentSection.erase(currentSection.begin());
            }
            continue;
        }

        auto eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;

        std::string key = line.substr(0, eqPos);
        std::string val = line.substr(eqPos + 1);

        // Trim whitespace and line endings
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.erase(key.begin());
        while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r' || val.back() == '\n')) val.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());

        if (key == "last_asio_driver") {
            lastAsioDriver = utils::unescapeIniField(val);
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
        } else if (key == "enable_live_thumbnails" || key == "enableLiveThumbnails" ||
                   key == "live_thumbnails" || (currentSection == "UI" && key == "live_thumbnails")) {
            enableLiveThumbnails = (val == "1" || val == "true");
        } else if (key == "preset_favorites_initialized") {
            presetFavoritesInitialized = (val == "1" || val == "true");
        } else if (key == "custom_plugin_path" || key == "vst3_path" || key == "clap_path") {
            customPluginPaths.push_back(utils::unescapeIniField(val));
        } else if (key == "favorite_plugin") {
            favoritePlugins.push_back(utils::unescapeIniField(val));
        } else if (key == "favorite_preset") {
            if (!val.empty()) {
                favoritePresets.push_back(utils::unescapeIniField(val));
            }
        } else if (key == "plugin_category") {
            auto sep = val.find('|');
            if (sep != std::string::npos) {
                std::string catName = utils::unescapeIniField(val.substr(0, sep));
                std::string catVal = utils::unescapeIniField(val.substr(sep + 1));
                customPluginCategories[catName] = catVal;
            }
        } else if (key == "recent_plugin") {
            recentPlugins.push_back(utils::unescapeIniField(val));
        } else if (key == "scanned_plugin" || key == "cached_plugin") {
            // Format: name|path|type|vendor|category|bundleFileName|classGuid|clapId
            std::vector<std::string> parts;
            size_t start = 0;
            while (start <= val.size()) {
                size_t pos = val.find('|', start);
                if (pos == std::string::npos) {
                    parts.push_back(val.substr(start));
                    break;
                }
                parts.push_back(val.substr(start, pos - start));
                start = pos + 1;
            }
            if (parts.size() >= 3) {
                ScannedPluginEntry entry;
                entry.name = utils::unescapeIniField(parts[0]);
                entry.path = utils::unescapeIniField(parts[1]);
                entry.type = utils::unescapeIniField(parts[2]);
                if (parts.size() > 3) entry.vendor = utils::unescapeIniField(parts[3]);
                if (parts.size() > 4) entry.category = utils::unescapeIniField(parts[4]);
                if (parts.size() > 5) entry.bundleFileName = utils::unescapeIniField(parts[5]);
                if (parts.size() > 6) entry.classGuid = utils::unescapeIniField(parts[6]);
                if (parts.size() > 7) entry.clapId = utils::unescapeIniField(parts[7]);
                scannedPlugins.push_back(std::move(entry));
            }
        } else if (key == "key_binding" || key == "key_bindings" || key == "keyBinding" ||
                   (currentSection == "KeyBindings" && (key == "binding" || key == "entry"))) {
            // Format: key|slotIndex|branchIndex|branchSlotIndex|targetType|paramId
            char delim = (val.find('|') != std::string::npos) ? '|' : ',';
            std::vector<std::string> parts;
            size_t start = 0;
            while (start <= val.size()) {
                size_t pos = val.find(delim, start);
                if (pos == std::string::npos) {
                    parts.push_back(val.substr(start));
                    break;
                }
                parts.push_back(val.substr(start, pos - start));
                start = pos + 1;
            }
            if (parts.size() >= 5) {
                KeyBindingEntry kb{};
                kb.key = utils::parseInteger<int>(parts[0], 0);
                kb.slotIndex = utils::parseInteger<int>(parts[1], -1);
                kb.branchIndex = utils::parseInteger<int>(parts[2], -1);
                kb.branchSlotIndex = utils::parseInteger<int>(parts[3], -1);
                kb.targetType = utils::parseInteger<uint32_t>(parts[4], 0);
                if (parts.size() >= 6) {
                    kb.paramId = utils::parseInteger<uint32_t>(parts[5], 0);
                }
                if (parts.size() >= 7) {
                    kb.macroIndex = utils::parseInteger<int>(parts[6], -1);
                }
                keyBindings.push_back(kb);
            }
        } else if (key == "retroactive_buffer_minutes") {
            uint32_t valM = utils::parseInteger<uint32_t>(val, 2);
            if (valM == 1 || valM == 2 || valM == 5 || valM == 10) {
                retroactiveBufferMinutes = valM;
            } else {
                retroactiveBufferMinutes = 2;
            }
        } else if (key == "riff_directory") {
            riffDirectory = utils::unescapeIniField(val);
        } else if (key == "last_loaded_preset") {
            lastLoadedPreset = utils::unescapeIniField(val);
        } else if (key == "rack_view_mode") {
            int modeVal = utils::parseInteger<int>(val, 0);
            rackViewMode = (modeVal == 1) ? RackViewMode::Modular : RackViewMode::Simple;
        } else if (key == "theme_id") {
            themeId = std::clamp(utils::parseInteger<int>(val, 0), 0, 3);
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
        } else if (key == "tuner_enabled") {
            tunerEnabled = (val == "1" || val == "true");
        } else if (key == "tuner_a4_reference") {
            tunerA4Reference = utils::parseFloat(val, 440.0f);
        } else if (key == "tuner_instrument") {
            tunerInstrument = utils::parseInteger<int>(val, 0);
        } else if (key == "tuner_string_count") {
            tunerStringCount = utils::parseInteger<int>(val, 6);
        } else if (key == "tuner_tuning_mode") {
            tunerTuningMode = utils::parseInteger<int>(val, 0);
        } else if (key == "tuner_tuning_type") {
            tunerTuningType = utils::parseInteger<int>(val, 0);
        } else if (key == "tuner_root_note") {
            tunerRootNote = utils::parseInteger<int>(val, 4);
        } else if (key == "tuner_custom_strings") {
            tunerCustomStrings = utils::unescapeIniField(val);
        }
    }
    return true;
}

bool AppConfig::save(const std::string& customPath) const {
    std::string path = customPath.empty() ? getConfigFilePath() : customPath;
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "# Praccy Configuration\n";
    file << "last_asio_driver=" << utils::escapeIniField(lastAsioDriver) << "\n";
    file << "input_mode=" << static_cast<int>(inputMode) << "\n";
    file << "input_gain_db=" << inputGainDb << "\n";
    file << "master_volume_db=" << masterVolumeDb << "\n";
    file << "metronome_bpm=" << metronomeBpm << "\n";
    file << "check_beta_updates=" << (checkBetaUpdates ? "1" : "0") << "\n";
    file << "enable_live_thumbnails=" << (enableLiveThumbnails ? "1" : "0") << "\n";
    file << "retroactive_buffer_minutes=" << retroactiveBufferMinutes << "\n";
    file << "riff_directory=" << utils::escapeIniField(riffDirectory) << "\n";
    file << "last_loaded_preset=" << utils::escapeIniField(lastLoadedPreset) << "\n";
    file << "rack_view_mode=" << static_cast<int>(rackViewMode) << "\n";
    file << "theme_id=" << themeId << "\n";
    file << "window_x=" << windowX << "\n";
    file << "window_y=" << windowY << "\n";
    file << "window_w=" << windowW << "\n";
    file << "window_h=" << windowH << "\n";
    file << "window_maximized=" << (windowMaximized ? "1" : "0") << "\n";
    file << "tuner_enabled=" << (tunerEnabled ? "1" : "0") << "\n";
    file << "tuner_a4_reference=" << tunerA4Reference << "\n";
    file << "tuner_instrument=" << tunerInstrument << "\n";
    file << "tuner_string_count=" << tunerStringCount << "\n";
    file << "tuner_tuning_mode=" << tunerTuningMode << "\n";
    file << "tuner_tuning_type=" << tunerTuningType << "\n";
    file << "tuner_root_note=" << tunerRootNote << "\n";
    file << "tuner_custom_strings=" << utils::escapeIniField(tunerCustomStrings) << "\n";

    file << "preset_favorites_initialized=" << (presetFavoritesInitialized ? "1" : "0") << "\n";
    for (const auto& cp : customPluginPaths) {
        file << "custom_plugin_path=" << utils::escapeIniField(cp) << "\n";
    }

    for (const auto& fp : favoritePlugins) {
        file << "favorite_plugin=" << utils::escapeIniField(fp) << "\n";
    }

    for (const auto& fpre : favoritePresets) {
        file << "favorite_preset=" << utils::escapeIniField(fpre) << "\n";
    }

    for (const auto& [name, cat] : customPluginCategories) {
        file << "plugin_category=" << utils::escapeIniField(name) << "|" << utils::escapeIniField(cat) << "\n";
    }

    for (const auto& rp : recentPlugins) {
        file << "recent_plugin=" << utils::escapeIniField(rp) << "\n";
    }

    for (const auto& sp : scannedPlugins) {
        file << "scanned_plugin="
             << utils::escapeIniField(sp.name) << "|"
             << utils::escapeIniField(sp.path) << "|"
             << utils::escapeIniField(sp.type) << "|"
             << utils::escapeIniField(sp.vendor) << "|"
             << utils::escapeIniField(sp.category) << "|"
             << utils::escapeIniField(sp.bundleFileName) << "|"
             << utils::escapeIniField(sp.classGuid) << "|"
             << utils::escapeIniField(sp.clapId) << "\n";
    }

    for (const auto& kb : keyBindings) {
        file << "key_binding="
             << kb.key << "|"
             << kb.slotIndex << "|"
             << kb.branchIndex << "|"
             << kb.branchSlotIndex << "|"
             << kb.targetType << "|"
             << kb.paramId << "|"
             << kb.macroIndex << "\n";
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
