#pragma once

#include "../audio/graph_engine.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace praccy::state {

struct PluginSlotPreset {
    std::string name;
    std::string path;          // "builtin://drive", "builtin://amp", "builtin://delay", or VST3/CLAP file path
    std::string type;          // "BuiltIn", "VST3", "CLAP"
    bool bypassed{false};
    float dryWet{1.0f};
    float inputGainDb{0.0f};
    float outputGainDb{0.0f};
    std::vector<uint8_t> state;
};

struct BranchPreset {
    std::string name{"Branch"};
    float gainDb{0.0f};
    float pan{0.0f};
    bool muted{false};
    bool solo{false};
    bool phaseInvert{false};
    std::vector<PluginSlotPreset> slots;
};

struct NodePreset {
    enum class Kind { Plugin, ParallelBlock };
    Kind kind{Kind::Plugin};
    PluginSlotPreset slot;
    std::vector<BranchPreset> branches;
};

struct ScenePreset {
    std::string name;
    std::vector<NodePreset> nodes;
};

class SceneManager {
public:
    SceneManager();

    void initializeDefaultScenes();

    bool loadFromFile(const std::string& filePath = "");
    bool saveToFile(const std::string& filePath = "") const;

    void captureCurrentScene(int sceneIndex, audio::GraphEngine& graph);
    bool applyScene(int sceneIndex, audio::GraphEngine& graph);

    void savePresetChain(const std::string& name, audio::GraphEngine& graph);
    bool loadPresetChain(const std::string& name, audio::GraphEngine& graph);
    [[nodiscard]] std::vector<std::string> savedPresetNames() const;

    [[nodiscard]] int activeSceneIndex() const noexcept { return m_activeSceneIndex; }
    void setActiveSceneIndex(int idx) noexcept { m_activeSceneIndex = idx; }
    [[nodiscard]] size_t numScenes() const noexcept { return m_scenes.size(); }
    [[nodiscard]] const std::vector<ScenePreset>& scenes() const noexcept { return m_scenes; }
    [[nodiscard]] const ScenePreset* getScene(size_t index) const noexcept;
    void setSceneName(size_t index, const std::string& name);

    [[nodiscard]] const std::string& lastStatusMessage() const noexcept { return m_statusMessage; }

    static std::unique_ptr<audio::PluginSlot> createSlotFromPreset(const PluginSlotPreset& p);

private:
    int m_activeSceneIndex{0};
    std::vector<ScenePreset> m_scenes;
    std::vector<ScenePreset> m_userPresets;
    std::string m_statusMessage;
};

} // namespace praccy::state
