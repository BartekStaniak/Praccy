#pragma once

#include "../scene_manager.h"
#include <string>
#include <vector>
#include <filesystem>
#include <optional>

namespace praccy::state {

class IDawChainAdapter {
public:
    virtual ~IDawChainAdapter() = default;
    [[nodiscard]] virtual std::string formatName() const = 0;
    [[nodiscard]] virtual std::vector<std::string> fileExtensions() const = 0;
    virtual bool canHandle(const std::filesystem::path& path) const = 0;
    
    // Codecs consume and produce canonical ScenePreset directly
    virtual std::optional<ScenePreset> parseToScene(const std::filesystem::path& path) = 0;
    virtual bool exportFromScene(const ScenePreset& scene, const std::filesystem::path& outputPath) = 0;
};

} // namespace praccy::state
