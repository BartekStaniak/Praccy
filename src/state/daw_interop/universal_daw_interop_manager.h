#pragma once

#include "daw_adapter.h"
#include "reaper_adapter.h"
#include "ableton_adapter.h"
#include "dawproject_adapter.h"
#include "vstpreset_adapter.h"
#include "../../plugins/plugin_scanner.h"
#include <memory>
#include <vector>
#include <string>
#include <filesystem>
#include <functional>

namespace praccy::audio {
class GraphEngine;
}

namespace praccy::state {
class SceneManager;

class UniversalDawInteropManager {
public:
    UniversalDawInteropManager();
    ~UniversalDawInteropManager() = default;

    static UniversalDawInteropManager& instance();

    [[nodiscard]] const std::vector<std::unique_ptr<IDawChainAdapter>>& adapters() const {
        return m_adapters;
    }

    [[nodiscard]] IDawChainAdapter* findAdapter(const std::filesystem::path& path) const;
    [[nodiscard]] IDawChainAdapter* findAdapterByExtension(std::string_view ext) const;

    // Imports preset/chain from file and hydrates GraphEngine & SceneManager
    bool importAndApply(
        const std::filesystem::path& path,
        audio::GraphEngine& graph,
        SceneManager& sceneMgr,
        const plugins::PluginScanner& scanner,
        std::string* outStatusMessage = nullptr
    );

    // Exports given ScenePreset to target format file
    bool exportScene(
        const ScenePreset& scene,
        const std::filesystem::path& outputPath,
        std::string* outStatusMessage = nullptr
    );

private:
    std::vector<std::unique_ptr<IDawChainAdapter>> m_adapters;
};

} // namespace praccy::state
