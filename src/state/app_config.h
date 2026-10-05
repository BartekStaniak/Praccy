#pragma once

#include "../audio/input_config.h"
#include <string>
#include <vector>

namespace praccy::state {

struct AppConfig {
    std::string lastAsioDriver;
    audio::InputRoutingMode inputMode{audio::InputRoutingMode::MonoLeft};
    float inputGainDb{0.0f};
    float masterVolumeDb{0.0f};
    float metronomeBpm{120.0f};
    std::vector<std::string> customPluginPaths;

    static std::string getConfigDir();
    static std::string getConfigFilePath();
    bool load(const std::string& customPath = "");
    bool save(const std::string& customPath = "") const;
};

} // namespace praccy::state
