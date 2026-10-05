#include "scene_manager.h"

namespace praccy::state {

SceneManager::SceneManager() {
    initializeDefaultScenes();
}

void SceneManager::initializeDefaultScenes() {
    m_scenes.clear();
    m_scenes.push_back(Scene{.name = "1: Clean", .serialSlotStates = {}, .parallelBlocksStates = {}});
    m_scenes.push_back(Scene{.name = "2: Crunch", .serialSlotStates = {}, .parallelBlocksStates = {}});
    m_scenes.push_back(Scene{.name = "3: Lead", .serialSlotStates = {}, .parallelBlocksStates = {}});
    m_scenes.push_back(Scene{.name = "4: Ambient", .serialSlotStates = {}, .parallelBlocksStates = {}});
    m_activeSceneIndex = 0;
}

const Scene* SceneManager::getScene(size_t index) const noexcept {
    if (index < m_scenes.size()) return &m_scenes[index];
    return nullptr;
}

void SceneManager::setSceneName(size_t index, const std::string& name) {
    if (index < m_scenes.size()) {
        m_scenes[index].name = name;
    }
}

void SceneManager::captureCurrentScene(int sceneIndex, audio::GraphEngine& graph) {
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(m_scenes.size())) return;

    auto& scene = m_scenes[sceneIndex];
    scene.serialSlotStates.clear();
    scene.parallelBlocksStates.clear();

    const size_t numNodes = graph.numNodes();
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            if (slot) {
                scene.serialSlotStates.push_back(SlotState{
                    .bypassed = slot->isBypassed(),
                    .dryWet = slot->dryWet(),
                    .inputGainDb = slot->inputGainDb(),
                    .outputGainDb = slot->outputGainDb()
                });
            }
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            if (block) {
                std::vector<BranchState> branchStates;
                for (size_t b = 0; b < block->numBranches(); ++b) {
                    auto* branch = block->getBranch(b);
                    BranchState bs{
                        .gainDb = branch->gainDb(),
                        .pan = branch->pan(),
                        .muted = branch->isMuted(),
                        .solo = branch->isSolo(),
                        .phaseInvert = branch->isPhaseInvert()
                    };
                    for (size_t s = 0; s < branch->numSlots(); ++s) {
                        auto* bSlot = branch->getSlot(s);
                        bs.slotStates.push_back(SlotState{
                            .bypassed = bSlot->isBypassed(),
                            .dryWet = bSlot->dryWet(),
                            .inputGainDb = bSlot->inputGainDb(),
                            .outputGainDb = bSlot->outputGainDb()
                        });
                    }
                    branchStates.push_back(std::move(bs));
                }
                scene.parallelBlocksStates[static_cast<int>(i)] = std::move(branchStates);
            }
        }
    }
}

void SceneManager::applyScene(int sceneIndex, audio::GraphEngine& graph) {
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(m_scenes.size())) return;

    const auto& scene = m_scenes[sceneIndex];
    m_activeSceneIndex = sceneIndex;

    size_t serialSlotIdx = 0;
    const size_t numNodes = graph.numNodes();
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            if (slot && serialSlotIdx < scene.serialSlotStates.size()) {
                const auto& state = scene.serialSlotStates[serialSlotIdx++];
                slot->setBypassed(state.bypassed);
                slot->setDryWet(state.dryWet);
                slot->setInputGainDb(state.inputGainDb);
                slot->setOutputGainDb(state.outputGainDb);
            }
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            auto it = scene.parallelBlocksStates.find(static_cast<int>(i));
            if (block && it != scene.parallelBlocksStates.end()) {
                const auto& branchStates = it->second;
                for (size_t b = 0; b < block->numBranches() && b < branchStates.size(); ++b) {
                    auto* branch = block->getBranch(b);
                    const auto& bs = branchStates[b];
                    branch->setGainDb(bs.gainDb);
                    branch->setPan(bs.pan);
                    branch->setMuted(bs.muted);
                    branch->setSolo(bs.solo);
                    branch->setPhaseInvert(bs.phaseInvert);

                    for (size_t s = 0; s < branch->numSlots() && s < bs.slotStates.size(); ++s) {
                        auto* bSlot = branch->getSlot(s);
                        const auto& ss = bs.slotStates[s];
                        bSlot->setBypassed(ss.bypassed);
                        bSlot->setDryWet(ss.dryWet);
                        bSlot->setInputGainDb(ss.inputGainDb);
                        bSlot->setOutputGainDb(ss.outputGainDb);
                    }
                }
            }
        }
    }
}

} // namespace praccy::state
