#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace praccy::audio {

enum class InputRoutingMode {
    MonoLeft,    // In 1 -> Left & Right (Standard single guitar/instrument)
    MonoRight,   // In 2 -> Left & Right (Instrument on input 2)
    MonoChannel, // In N -> Left & Right
    Stereo,      // In 1 -> Left, In 2 -> Right
    StereoCustom // In L, In R -> Left, Right
};

struct InputRoutingConfig {
    InputRoutingMode mode{InputRoutingMode::MonoLeft};
    int channelLeft{0};
    int channelRight{1};

    [[nodiscard]] std::string modeDescription() const {
        switch (mode) {
            case InputRoutingMode::MonoLeft: return "Mono: In 1 (L+R)";
            case InputRoutingMode::MonoRight: return "Mono: In 2 (L+R)";
            case InputRoutingMode::MonoChannel: return "Mono: In " + std::to_string(channelLeft + 1);
            case InputRoutingMode::Stereo: return "Stereo: In 1+2";
            case InputRoutingMode::StereoCustom: return "Stereo: " + std::to_string(channelLeft + 1) + "+" + std::to_string(channelRight + 1);
        }
        return "Default";
    }
};

} // namespace praccy::audio
