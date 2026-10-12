#pragma once

#include "daw_adapter.h"

namespace praccy::state {

class AbletonAdapter : public IDawChainAdapter {
public:
    AbletonAdapter() = default;
    ~AbletonAdapter() override = default;

    [[nodiscard]] std::string formatName() const override {
        return "Ableton Live Rack";
    }

    [[nodiscard]] std::vector<std::string> fileExtensions() const override {
        return {".adg", ".ADG"};
    }

    bool canHandle(const std::filesystem::path& path) const override;

    std::optional<ScenePreset> parseToScene(const std::filesystem::path& path) override;
    bool exportFromScene(const ScenePreset& scene, const std::filesystem::path& outputPath) override;
};

} // namespace praccy::state
