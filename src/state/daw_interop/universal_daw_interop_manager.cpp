#include "universal_daw_interop_manager.h"
#include "../scene_manager.h"
#include "../../audio/graph_engine.h"
#include <algorithm>

namespace praccy::state {

UniversalDawInteropManager::UniversalDawInteropManager() {
    m_adapters.push_back(std::make_unique<ReaperAdapter>());
    m_adapters.push_back(std::make_unique<AbletonAdapter>());
    m_adapters.push_back(std::make_unique<DawprojectAdapter>());
    m_adapters.push_back(std::make_unique<VstPresetAdapter>());
}

UniversalDawInteropManager& UniversalDawInteropManager::instance() {
    static UniversalDawInteropManager s_instance;
    return s_instance;
}

IDawChainAdapter* UniversalDawInteropManager::findAdapter(const std::filesystem::path& path) const {
    for (const auto& adapter : m_adapters) {
        if (adapter && adapter->canHandle(path)) {
            return adapter.get();
        }
    }
    return nullptr;
}

IDawChainAdapter* UniversalDawInteropManager::findAdapterByExtension(std::string_view ext) const {
    std::string lowerExt(ext);
    std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), [](unsigned char c){ return std::tolower(c); });
    if (!lowerExt.empty() && lowerExt.front() != '.') {
        lowerExt.insert(lowerExt.begin(), '.');
    }

    for (const auto& adapter : m_adapters) {
        if (!adapter) continue;
        for (const auto& aExt : adapter->fileExtensions()) {
            std::string lowerAExt = aExt;
            std::transform(lowerAExt.begin(), lowerAExt.end(), lowerAExt.begin(), [](unsigned char c){ return std::tolower(c); });
            if (lowerAExt == lowerExt) {
                return adapter.get();
            }
        }
    }
    return nullptr;
}

bool UniversalDawInteropManager::importAndApply(
    const std::filesystem::path& path,
    audio::GraphEngine& graph,
    SceneManager& sceneMgr,
    const plugins::PluginScanner& scanner,
    std::string* outStatusMessage
) {
    auto* adapter = findAdapter(path);
    if (!adapter) {
        if (outStatusMessage) *outStatusMessage = "No compatible DAW adapter found for: " + path.filename().string();
        return false;
    }

    auto optScene = adapter->parseToScene(path);
    if (!optScene.has_value()) {
        if (outStatusMessage) *outStatusMessage = "Failed to parse " + adapter->formatName() + ": " + path.filename().string();
        return false;
    }

    ScenePreset scene = std::move(*optScene);

    // Multi-tier Plugin Resolution for each slot
    auto resolveSlot = [&](PluginSlotPreset& slot) {
        if (slot.path.starts_with("builtin://")) {
            return; // Already resolved
        }

        std::string id = slot.classGuid.empty() ? slot.clapId : slot.classGuid;
        auto* desc = scanner.resolvePlugin(slot.name, slot.bundleFileName, id);
        if (desc) {
            slot.path = desc->path;
            slot.type = (desc->type == plugins::PluginType::CLAP ? "CLAP" :
                        (desc->type == plugins::PluginType::VST3 ? "VST3" : "BuiltIn"));
            if (slot.name.empty()) slot.name = desc->name;
        } else {
            // Not found in local library -> slot.path remains empty to trigger MissingPluginPlaceholder
            slot.path = "";
        }
    };

    for (auto& node : scene.nodes) {
        if (node.kind == NodePreset::Kind::Plugin) {
            resolveSlot(node.slot);
        } else if (node.kind == NodePreset::Kind::ParallelBlock) {
            for (auto& branch : node.branches) {
                for (auto& bSlot : branch.slots) {
                    resolveSlot(bSlot);
                }
            }
        }
    }

    // Apply the scene to GraphEngine with 10ms EqualPowerRamp crossfade
    bool ok = sceneMgr.applyGraphSnapshot(scene, graph);
    if (ok) {
        sceneMgr.captureCurrentScene(sceneMgr.activeSceneIndex(), graph);
        if (outStatusMessage) {
            *outStatusMessage = "Imported " + adapter->formatName() + ": " + path.filename().string();
        }
    } else {
        if (outStatusMessage) {
            *outStatusMessage = "Failed to apply scene from: " + path.filename().string();
        }
    }

    return ok;
}

bool UniversalDawInteropManager::exportScene(
    const ScenePreset& scene,
    const std::filesystem::path& outputPath,
    std::string* outStatusMessage
) {
    auto* adapter = findAdapter(outputPath);
    if (!adapter) {
        if (outStatusMessage) *outStatusMessage = "No compatible DAW adapter for extension: " + outputPath.extension().string();
        return false;
    }

    bool ok = adapter->exportFromScene(scene, outputPath);
    if (ok) {
        if (outStatusMessage) {
            *outStatusMessage = "Exported " + adapter->formatName() + " to: " + outputPath.filename().string();
        }
    } else {
        if (outStatusMessage) {
            *outStatusMessage = "Failed to export " + adapter->formatName() + " to: " + outputPath.filename().string();
        }
    }
    return ok;
}

} // namespace praccy::state
