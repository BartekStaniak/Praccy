#include "app_config.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>

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
            inputMode = static_cast<audio::InputRoutingMode>(std::atoi(val.c_str()));
        } else if (key == "input_gain_db") {
            inputGainDb = static_cast<float>(std::atof(val.c_str()));
        } else if (key == "master_volume_db") {
            masterVolumeDb = static_cast<float>(std::atof(val.c_str()));
        } else if (key == "metronome_bpm") {
            metronomeBpm = static_cast<float>(std::atof(val.c_str()));
        } else if (key == "check_beta_updates") {
            checkBetaUpdates = (val == "1" || val == "true");
        } else if (key == "custom_plugin_path") {
            customPluginPaths.push_back(val);
        } else if (key == "favorite_plugin") {
            favoritePlugins.push_back(val);
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

    for (const auto& cp : customPluginPaths) {
        file << "custom_plugin_path=" << cp << "\n";
    }

    for (const auto& fp : favoritePlugins) {
        file << "favorite_plugin=" << fp << "\n";
    }

    return true;
}

} // namespace praccy::state
