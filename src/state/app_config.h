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
    std::vector<std::string> recentPlugins;

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
