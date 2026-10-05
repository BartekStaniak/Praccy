#pragma once

#include "../audio/graph_engine.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace praccy::state {

struct SlotState {
    bool bypassed{false};
    float dryWet{1.0f};
    float inputGainDb{0.0f};
    float outputGainDb{0.0f};
};

struct BranchState {
    float gainDb{0.0f};
    float pan{0.0f};
    bool muted{false};
    bool solo{false};
    bool phaseInvert{false};
    std::vector<SlotState> slotStates;
};

struct Scene {
    std::string name;
    std::vector<SlotState> serialSlotStates;
    std::unordered_map<int, std::vector<BranchState>> parallelBlocksStates;
};

class SceneManager {
public:
    SceneManager();

    void initializeDefaultScenes();

    void captureCurrentScene(int sceneIndex, audio::GraphEngine& graph);
    void applyScene(int sceneIndex, audio::GraphEngine& graph);

    [[nodiscard]] int activeSceneIndex() const noexcept { return m_activeSceneIndex; }
    [[nodiscard]] size_t numScenes() const noexcept { return m_scenes.size(); }
    [[nodiscard]] const Scene* getScene(size_t index) const noexcept;
    void setSceneName(size_t index, const std::string& name);

private:
    int m_activeSceneIndex{0};
    std::vector<Scene> m_scenes;
};

} // namespace praccy::state
