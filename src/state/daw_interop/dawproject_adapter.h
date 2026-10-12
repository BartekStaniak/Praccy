#pragma once

#include "daw_adapter.h"

namespace praccy::state {

class DawprojectAdapter : public IDawChainAdapter {
public:
    DawprojectAdapter() = default;
    ~DawprojectAdapter() override = default;

    [[nodiscard]] std::string formatName() const override {
        return "DAWproject";
    }

    [[nodiscard]] std::vector<std::string> fileExtensions() const override {
        return {".dawproject"};
    }

    bool canHandle(const std::filesystem::path& path) const override;

    std::optional<ScenePreset> parseToScene(const std::filesystem::path& path) override;
    bool exportFromScene(const ScenePreset& scene, const std::filesystem::path& outputPath) override;
};

} // namespace praccy::state
