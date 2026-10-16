#include "scene_manager.h"
#include "app_config.h"
#include "../plugins/builtin_dsp.h"
#include "../plugins/vst3_host.h"
#include "../plugins/clap_host.h"
#include "../plugins/plugin_window.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <iomanip>

#include "../utils/parse_utils.h"

namespace praccy::state {

static std::string bytesToHex(const std::vector<uint8_t>& data) {
    std::ostringstream oss;
    for (uint8_t b : data) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

static std::vector<uint8_t> hexToBytes(const std::string& hex) {
    return praccy::utils::hexToBytes(hex);
}

static void captureSlotToPreset(audio::PluginSlot* slot, PluginSlotPreset& sp) {
    if (!slot) return;
    sp.name = slot->name();
    sp.bypassed = slot->isBypassed();
    sp.dryWet = slot->dryWet();
    sp.inputGainDb = slot->inputGainDb();
    sp.outputGainDb = slot->outputGainDb();

    auto* inner = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    if (inner) {
        sp.path = inner->path();
        sp.bundleFileName = inner->bundleFileName();
        sp.classGuid = inner->classGuid();
        sp.clapId = inner->clapId();
        if (dynamic_cast<plugins::Vst3PluginInstance*>(inner)) {
            sp.type = "VST3";
        } else if (dynamic_cast<plugins::ClapPluginInstance*>(inner)) {
            sp.type = "CLAP";
        } else {
            sp.type = "BuiltIn";
        }
        sp.state = inner->saveState();

        const size_t nParams = inner->numParameters();
        sp.paramValues.reserve(nParams);
        for (size_t p = 0; p < nParams; ++p) {
            auto desc = inner->getParameterDesc(p);
            sp.paramValues.push_back(PluginSlotPreset::ParamValue{desc.id, desc.currentValue});
        }
    }

    if (slot->hasMacrosInitialized()) {
        for (auto& sm : slot->macros()) {
            if (sm.bound && inner) {
                sm.currentValue = inner->getParameterValue(sm.paramId);
            } else if (sm.isSlotTrim) {
                sm.currentValue = slot->outputGainDb();
            } else if (sm.isSlotMix) {
                sm.currentValue = slot->dryWet() * 100.0f;
            }

            PluginSlotPreset::MacroBinding mb;
            mb.paramId = sm.paramId;
            mb.label = sm.label;
            mb.value = sm.currentValue;
            mb.defaultValue = sm.defaultValue;
            mb.minValue = sm.minValue;
            mb.maxValue = sm.maxValue;
            mb.bound = sm.bound;
            mb.isSlotMix = sm.isSlotMix;
            mb.isSlotTrim = sm.isSlotTrim;
            sp.macros.push_back(std::move(mb));
        }
    }
}

static void applyPresetToSlot(const PluginSlotPreset& sp, audio::PluginSlot* slot, plugins::IPluginInstance* inner) {
    if (!slot) return;

    slot->automationRegister().reset();
    slot->setBypassed(sp.bypassed);
    slot->setDryWet(sp.dryWet);
    slot->setInputGainDb(sp.inputGainDb);
    slot->setOutputGainDb(sp.outputGainDb);

    if (!sp.state.empty() && inner) {
        inner->loadState(sp.state);
    } else if (!sp.paramValues.empty() && inner) {
        for (const auto& pv : sp.paramValues) {
            inner->setParameterValue(pv.paramId, pv.value);
        }
    }

    if (!sp.paramValues.empty()) {
        for (const auto& pv : sp.paramValues) {
            slot->setParameter(pv.paramId, pv.value);
        }
    }

    if (!sp.macros.empty()) {
        for (size_t m = 0; m < sp.macros.size() && m < 4; ++m) {
            const auto& mb = sp.macros[m];
            auto& sm = slot->macros()[m];
            sm.paramId = mb.paramId;
            sm.label = mb.label;
            sm.defaultValue = mb.defaultValue;
            sm.currentValue = mb.value;

            if (mb.bound || mb.isSlotMix || mb.isSlotTrim) {
                sm.bound = mb.bound;
                sm.isSlotMix = mb.isSlotMix;
                sm.isSlotTrim = mb.isSlotTrim;
                sm.minValue = mb.minValue;
                sm.maxValue = mb.maxValue;
            } else {
                // Backward compatibility for presets serialized without explicit bound/trim/mix flags
                sm.isSlotTrim = (m == 2 && mb.label == "VOL") || (mb.label == "TRIM");
                sm.isSlotMix = (m == 3 && mb.label == "MIX") || (mb.label == "MIX");
                sm.bound = !sm.isSlotTrim && !sm.isSlotMix && (mb.paramId != 0 || !mb.label.empty());
                if (sm.isSlotTrim) {
                    sm.minValue = -24.0f;
                    sm.maxValue = 12.0f;
                } else if (sm.isSlotMix) {
                    sm.minValue = 0.0f;
                    sm.maxValue = 100.0f;
                } else if (sm.bound && inner) {
                    const size_t nParams = inner->numParameters();
                    for (size_t p = 0; p < nParams; ++p) {
                        auto desc = inner->getParameterDesc(p);
                        if (desc.id == mb.paramId) {
                            sm.minValue = desc.minValue;
                            sm.maxValue = desc.maxValue;
                            break;
                        }
                    }
                }
            }

            if (sm.bound) {
                slot->setParameter(mb.paramId, mb.value);
                if (inner) {
                    inner->setParameterValue(mb.paramId, mb.value);
                }
            } else if (sm.isSlotTrim) {
                sm.currentValue = slot->outputGainDb();
            } else if (sm.isSlotMix) {
                sm.currentValue = slot->dryWet() * 100.0f;
            }
        }
        slot->setMacrosInitialized(true);
    }

    if (inner) {
        inner->clearGuiParamEdit();
    }
}

SceneManager::SceneManager() {
    initializeDefaultScenes();
    if (!loadFromFile()) {
        saveToFile();
    }
}

void SceneManager::initializeDefaultScenes() {
    m_scenes.clear();

    // 1: Clean
    ScenePreset s1;
    s1.name = "1: Clean";
    s1.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .bundleFileName = "", .classGuid = "", .clapId = "", .latencySamples = 0, .bypassed = false, .dryWet = 1.0f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}, .macros = {}},
        .branches = {},
        .branchBlend = 0.5f
    });
    s1.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .bundleFileName = "", .classGuid = "", .clapId = "", .latencySamples = 0, .bypassed = false, .dryWet = 0.25f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}, .macros = {}},
        .branches = {},
        .branchBlend = 0.5f
    });
    m_scenes.push_back(std::move(s1));

    // 2: Crunch
    ScenePreset s2;
    s2.name = "2: Crunch";
    s2.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Drive", .path = "builtin://drive", .type = "BuiltIn", .bundleFileName = "", .classGuid = "", .clapId = "", .latencySamples = 0, .bypassed = false, .dryWet = 0.8f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}, .macros = {}},
        .branches = {},
        .branchBlend = 0.5f
    });
    s2.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .bundleFileName = "", .classGuid = "", .clapId = "", .latencySamples = 0, .bypassed = false, .dryWet = 1.0f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}, .macros = {}},
        .branches = {},
        .branchBlend = 0.5f
    });
    m_scenes.push_back(std::move(s2));

    // 3: Lead
    ScenePreset s3;
    s3.name = "3: Lead";
    s3.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Drive", .path = "builtin://drive", .type = "BuiltIn", .bundleFileName = "", .classGuid = "", .clapId = "", .latencySamples = 0, .bypassed = false, .dryWet = 1.0f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}, .macros = {}},
        .branches = {},
        .branchBlend = 0.5f
    });
    s3.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .bundleFileName = "", .classGuid = "", .clapId = "", .latencySamples = 0, .bypassed = false, .dryWet = 1.0f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}, .macros = {}},
        .branches = {},
        .branchBlend = 0.5f
    });
    s3.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .bundleFileName = "", .classGuid = "", .clapId = "", .latencySamples = 0, .bypassed = false, .dryWet = 0.45f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}, .macros = {}},
        .branches = {},
        .branchBlend = 0.5f
    });
    m_scenes.push_back(std::move(s3));

    // 4: Ambient (Parallel Split)
    ScenePreset s4;
    s4.name = "4: Ambient";
    NodePreset splitNode;
    splitNode.kind = NodePreset::Kind::ParallelBlock;

    BranchPreset bA;
    bA.name = "Branch A (Dry Amp)";
    bA.gainDb = 0.0f;
    bA.pan = -0.3f;
    bA.slots.push_back(PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .bundleFileName = "", .classGuid = "", .clapId = "", .latencySamples = 0, .bypassed = false, .dryWet = 1.0f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}, .macros = {}});

    BranchPreset bB;
    bB.name = "Branch B (Ambient Delay)";
    bB.gainDb = -2.0f;
    bB.pan = 0.3f;
    bB.slots.push_back(PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .bundleFileName = "", .classGuid = "", .clapId = "", .latencySamples = 0, .bypassed = false, .dryWet = 0.9f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}, .macros = {}});

    splitNode.branches.push_back(std::move(bA));
    splitNode.branches.push_back(std::move(bB));
    s4.nodes.push_back(std::move(splitNode));
    m_scenes.push_back(std::move(s4));

    m_activeSceneIndex = 0;
}

const ScenePreset* SceneManager::getScene(size_t index) const noexcept {
    if (index < m_scenes.size()) return &m_scenes[index];
    return nullptr;
}

void SceneManager::setSceneName(size_t index, const std::string& name) {
    if (index < m_scenes.size()) {
        m_scenes[index].name = name;
        saveToFile();
    }
}

std::unique_ptr<audio::PluginSlot> SceneManager::createSlotFromPreset(const PluginSlotPreset& p) {
    std::unique_ptr<plugins::IPluginInstance> inst;

    if (p.path == "builtin://drive" || p.name == "Praccy Drive") {
        inst = std::make_unique<plugins::OverdriveEffect>();
    } else if (p.path == "builtin://amp" || p.name == "Praccy Amp Sim") {
        inst = std::make_unique<plugins::TubeAmpEffect>();
    } else if (p.path == "builtin://delay" || p.name == "Praccy Stereo Delay") {
        inst = std::make_unique<plugins::StereoDelayEffect>();
    } else if (p.type == "VST3" || p.path.find(".vst3") != std::string::npos) {
        inst = plugins::Vst3PluginInstance::loadFromFile(p.path);
    } else if (p.type == "CLAP" || p.path.find(".clap") != std::string::npos) {
        inst = plugins::ClapPluginInstance::loadFromFile(p.path);
    }

    if (!inst) {
        std::cerr << "SceneManager: Could not instantiate plugin for preset: " << p.name << " (" << p.path << ")\n";
        // Invariant 4: Graceful Degradation for Missing Plugins
        auto placeholder = std::make_unique<audio::MissingPluginPlaceholder>(
            p.name.empty() ? "Missing Plugin" : p.name,
            p.type.empty() ? "VST3" : p.type,
            p.classGuid,
            p.clapId
        );
        auto slot = std::make_unique<audio::PluginSlot>(std::move(placeholder));
        slot->setBypassed(p.bypassed);
        slot->setDryWet(p.dryWet);
        slot->setInputGainDb(p.inputGainDb);
        slot->setOutputGainDb(p.outputGainDb);
        return slot;
    }

    auto* rawInst = inst.get();
    auto slot = std::make_unique<audio::PluginSlot>(std::move(inst));
    applyPresetToSlot(p, slot.get(), rawInst);
    return slot;
}

ScenePreset SceneManager::captureGraphSnapshot(audio::GraphEngine& graph, const std::string& name) {
    ScenePreset scene;
    scene.name = name;

    const size_t numNodes = graph.numNodes();
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            if (slot) {
                NodePreset np;
                np.kind = NodePreset::Kind::Plugin;
                captureSlotToPreset(slot, np.slot);
                scene.nodes.push_back(std::move(np));
            }
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            if (block) {
                NodePreset np;
                np.kind = NodePreset::Kind::ParallelBlock;
                np.branchBlend = block->blend();
                for (size_t b = 0; b < block->numBranches(); ++b) {
                    auto* branch = block->getBranch(b);
                    if (!branch) continue;

                    BranchPreset bp;
                    bp.name = branch->name();
                    bp.gainDb = branch->gainDb();
                    bp.pan = branch->pan();
                    bp.muted = branch->isMuted();
                    bp.solo = branch->isSolo();
                    bp.phaseInvert = branch->isPhaseInvert();

                    for (size_t s = 0; s < branch->numSlots(); ++s) {
                        auto* bSlot = branch->getSlot(s);
                        if (!bSlot) continue;

                        PluginSlotPreset sp;
                        captureSlotToPreset(bSlot, sp);
                        bp.slots.push_back(std::move(sp));
                    }
                    np.branches.push_back(std::move(bp));
                }
                scene.nodes.push_back(std::move(np));
            }
        }
    }
    return scene;
}

static bool isSamePlugin(const PluginSlotPreset& p, audio::PluginSlot* slot) {
    if (!slot) return false;
    auto* inner = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    if (!inner) return false;
    if (!p.path.empty() && !inner->path().empty()) {
        if (p.path == inner->path()) return true;
    }
    if (!p.name.empty() && (p.name == slot->name() || p.name == inner->name())) {
        return true;
    }
    if ((p.path == "builtin://drive" || p.name == "Praccy Drive") &&
        dynamic_cast<plugins::OverdriveEffect*>(inner)) return true;
    if ((p.path == "builtin://amp" || p.name == "Praccy Amp Sim") &&
        dynamic_cast<plugins::TubeAmpEffect*>(inner)) return true;
    if ((p.path == "builtin://delay" || p.name == "Praccy Stereo Delay") &&
        dynamic_cast<plugins::StereoDelayEffect*>(inner)) return true;
    return false;
}

bool SceneManager::canApplyInPlace(const ScenePreset& scene, audio::GraphEngine& graph) {
    if (scene.nodes.empty()) return false;
    if (graph.numNodes() != scene.nodes.size()) return false;

    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        auto* node = graph.getNode(i);
        if (!node) return false;

        const auto& np = scene.nodes[i];
        if (np.kind == NodePreset::Kind::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            if (!slot || !isSamePlugin(np.slot, slot)) return false;
        } else if (np.kind == NodePreset::Kind::ParallelBlock) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            if (!block || block->numBranches() != np.branches.size()) return false;
            for (size_t b = 0; b < np.branches.size(); ++b) {
                auto* branch = block->getBranch(b);
                if (!branch || branch->numSlots() != np.branches[b].slots.size()) return false;
                for (size_t s = 0; s < np.branches[b].slots.size(); ++s) {
                    auto* bSlot = branch->getSlot(s);
                    if (!bSlot || !isSamePlugin(np.branches[b].slots[s], bSlot)) return false;
                }
            }
        } else {
            return false;
        }
    }
    return true;
}

bool SceneManager::applyGraphSnapshotInPlace(const ScenePreset& scene, audio::GraphEngine& graph) {
    for (size_t i = 0; i < scene.nodes.size(); ++i) {
        auto* node = graph.getNode(i);
        if (!node) continue;
        const auto& np = scene.nodes[i];
        if (np.kind == NodePreset::Kind::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            if (!slot) continue;
            auto* inner = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
            applyPresetToSlot(np.slot, slot, inner);
        } else if (np.kind == NodePreset::Kind::ParallelBlock) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            if (!block) continue;
            block->setBlend(np.branchBlend);
            for (size_t b = 0; b < np.branches.size(); ++b) {
                const auto& bp = np.branches[b];
                auto* branch = block->getBranch(b);
                if (!branch) continue;
                branch->setGainDb(bp.gainDb);
                branch->setPan(bp.pan);
                branch->setMuted(bp.muted);
                branch->setSolo(bp.solo);
                branch->setPhaseInvert(bp.phaseInvert);
                for (size_t s = 0; s < bp.slots.size(); ++s) {
                    auto* bSlot = branch->getSlot(s);
                    if (!bSlot) continue;
                    auto* bInner = dynamic_cast<plugins::IPluginInstance*>(bSlot->innerNode());
                    applyPresetToSlot(bp.slots[s], bSlot, bInner);
                }
            }
        }
    }
    graph.triggerCrossfade(10.0f);
    return true;
}

bool SceneManager::applyGraphSnapshot(const ScenePreset& scene, audio::GraphEngine& graph) {
    if (canApplyInPlace(scene, graph)) {
        return applyGraphSnapshotInPlace(scene, graph);
    }

    plugins::PluginWindowManager::instance().closeAllWindows();

    std::vector<std::unique_ptr<audio::AudioNode>> newNodes;

    for (const auto& np : scene.nodes) {
        if (np.kind == NodePreset::Kind::Plugin) {
            auto slot = createSlotFromPreset(np.slot);
            if (slot) {
                slot->prepare(graph.sampleRate(), graph.maxBlockSize());
                newNodes.push_back(std::move(slot));
            }
        } else if (np.kind == NodePreset::Kind::ParallelBlock) {
            auto block = std::make_unique<audio::ParallelSplitMergeBlock>();
            block->prepare(graph.sampleRate(), graph.maxBlockSize());
            block->setBlend(np.branchBlend);
            for (const auto& bp : np.branches) {
                auto* branch = block->addBranch(bp.name);
                branch->prepare(graph.sampleRate(), graph.maxBlockSize());
                branch->setGainDb(bp.gainDb);
                branch->setPan(bp.pan);
                branch->setMuted(bp.muted);
                branch->setSolo(bp.solo);
                branch->setPhaseInvert(bp.phaseInvert);

                for (const auto& sp : bp.slots) {
                    auto slot = createSlotFromPreset(sp);
                    if (slot) {
                        slot->prepare(graph.sampleRate(), graph.maxBlockSize());
                        branch->addSlot(std::move(slot));
                    }
                }
            }
            newNodes.push_back(std::move(block));
        }
    }

    graph.crossfadeToNodes(std::move(newNodes));
    return true;
}

void SceneManager::captureCurrentScene(int sceneIndex, audio::GraphEngine& graph) {
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(m_scenes.size())) return;

    std::string name = m_scenes[sceneIndex].name;
    m_scenes[sceneIndex] = captureGraphSnapshot(graph, name);
    m_statusMessage = "Preset saved: " + name;
    saveToFile();
}

bool SceneManager::applyScene(int sceneIndex, audio::GraphEngine& graph) {
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(m_scenes.size())) return false;

    m_activeSceneIndex = sceneIndex;
    bool ok = applyGraphSnapshot(m_scenes[sceneIndex], graph);
    m_statusMessage = "Loaded preset: " + m_scenes[sceneIndex].name;
    return ok;
}

void SceneManager::savePresetChain(const std::string& name, audio::GraphEngine& graph) {
    if (name.empty()) return;

    auto it = std::find_if(m_userPresets.begin(), m_userPresets.end(), [&](const ScenePreset& p) {
        return p.name == name;
    });

    std::string existingCat;
    if (it != m_userPresets.end()) {
        existingCat = it->category;
    }

    ScenePreset newPreset = captureGraphSnapshot(graph, name);
    newPreset.category = existingCat;
    if (it != m_userPresets.end()) {
        *it = std::move(newPreset);
    } else {
        m_userPresets.push_back(std::move(newPreset));
    }

    m_statusMessage = "Preset '" + name + "' saved successfully.";
    saveToFile();
}

bool SceneManager::loadPresetChain(const std::string& name, audio::GraphEngine& graph) {
    auto it = std::find_if(m_userPresets.begin(), m_userPresets.end(), [&](const ScenePreset& p) {
        return p.name == name;
    });
    if (it == m_userPresets.end()) return false;

    bool ok = applyGraphSnapshot(*it, graph);
    m_statusMessage = "Loaded preset: " + name;
    return ok;
}

void SceneManager::captureToneSnapshotA(audio::GraphEngine& graph) {
    m_toneSnapshotA = captureGraphSnapshot(graph, "Tone A");
    m_statusMessage = "Tone Snapshot A captured";
}

void SceneManager::captureToneSnapshotB(audio::GraphEngine& graph) {
    m_toneSnapshotB = captureGraphSnapshot(graph, "Tone B");
    m_statusMessage = "Tone Snapshot B captured";
}

bool SceneManager::applyToneSnapshotA(audio::GraphEngine& graph) {
    if (m_toneSnapshotA.nodes.empty()) return false;
    m_activeToneSlot = ToneSnapshotSlot::A;
    m_statusMessage = "Switched to Tone Snapshot A";
    return applyGraphSnapshot(m_toneSnapshotA, graph);
}

bool SceneManager::applyToneSnapshotB(audio::GraphEngine& graph) {
    if (m_toneSnapshotB.nodes.empty()) return false;
    m_activeToneSlot = ToneSnapshotSlot::B;
    m_statusMessage = "Switched to Tone Snapshot B";
    return applyGraphSnapshot(m_toneSnapshotB, graph);
}

void SceneManager::copyToneSnapshotAToB() {
    m_toneSnapshotB = m_toneSnapshotA;
    m_toneSnapshotB.name = "Tone B";
    m_statusMessage = "Copied Tone A to Tone B";
}

std::vector<std::string> SceneManager::savedPresetNames() const {
    std::vector<std::string> names;
    for (const auto& p : m_userPresets) {
        names.push_back(p.name);
    }
    return names;
}

const ScenePreset* SceneManager::findUserPreset(const std::string& name) const {
    for (const auto& p : m_userPresets) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

std::string SceneManager::presetCategory(const std::string& name) const {
    for (const auto& p : m_userPresets) {
        if (p.name == name) {
            return p.category;
        }
    }
    return "";
}

void SceneManager::setPresetCategory(const std::string& name, const std::string& category) {
    for (auto& p : m_userPresets) {
        if (p.name == name) {
            p.category = category;
            saveToFile();
            return;
        }
    }
}

std::vector<std::string> SceneManager::allPresetCategories() const {
    std::vector<std::string> cats = {
        "High Gain Metal",
        "Glassy Clean",
        "Ambient FX",
        "Edge of Breakup"
    };
    for (const auto& p : m_userPresets) {
        if (!p.category.empty()) {
            bool found = false;
            for (const auto& c : cats) {
                if (c == p.category) { found = true; break; }
            }
            if (!found) cats.push_back(p.category);
        }
    }
    return cats;
}

void SceneManager::initializeDefaultUserPresets() {
    // Purge obsolete raw test presets ("ss", "aa", "b") from prior iterations
    m_userPresets.erase(std::remove_if(m_userPresets.begin(), m_userPresets.end(), [](const ScenePreset& p) {
        return p.name == "ss" || p.name == "aa" || p.name == "b";
    }), m_userPresets.end());

    auto hasPreset = [this](const std::string& name) {
        for (const auto& p : m_userPresets) {
            if (p.name == name) return true;
        }
        return false;
    };

    auto addIfMissing = [this, &hasPreset](ScenePreset p) {
        if (!hasPreset(p.name)) {
            m_userPresets.push_back(std::move(p));
        }
    };

    // 1. High Gain Metal: Djent Rig
    {
        ScenePreset p;
        p.name = "Modern High Gain Djent";
        p.category = "High Gain Metal";
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Drive", .path = "builtin://drive", .type = "BuiltIn", .dryWet = 1.0f}
        });
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .dryWet = 1.0f}
        });
        addIfMissing(std::move(p));
    }

    // 2. Glassy Clean: Studio Clean
    {
        ScenePreset p;
        p.name = "California Glassy Clean";
        p.category = "Glassy Clean";
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .dryWet = 1.0f}
        });
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .dryWet = 0.20f}
        });
        addIfMissing(std::move(p));
    }

    // 3. Edge of Breakup: Dynamic Crunch
    {
        ScenePreset p;
        p.name = "Edge of Breakup Crunch";
        p.category = "Edge of Breakup";
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Drive", .path = "builtin://drive", .type = "BuiltIn", .dryWet = 0.60f}
        });
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .dryWet = 1.0f}
        });
        addIfMissing(std::move(p));
    }

    // 4. Ambient FX: Space Wash (Parallel Split)
    {
        ScenePreset p;
        p.name = "Post-Rock Ambient FX";
        p.category = "Ambient FX";
        NodePreset split;
        split.kind = NodePreset::Kind::ParallelBlock;
        BranchPreset bA;
        bA.name = "Branch A (Dry Amp)";
        bA.slots.push_back(PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn"});
        BranchPreset bB;
        bB.name = "Branch B (Ambient Delay)";
        bB.slots.push_back(PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .dryWet = 0.85f});
        split.branches.push_back(std::move(bA));
        split.branches.push_back(std::move(bB));
        p.nodes.push_back(std::move(split));
        addIfMissing(std::move(p));
    }

    // 5. High Gain Metal: Shred Solo
    {
        ScenePreset p;
        p.name = "Heavy Metal Shred Lead";
        p.category = "High Gain Metal";
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Drive", .path = "builtin://drive", .type = "BuiltIn", .dryWet = 0.90f}
        });
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .dryWet = 1.0f}
        });
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .dryWet = 0.40f}
        });
        addIfMissing(std::move(p));
    }

    // 6. Glassy Clean: Liquid Chime
    {
        ScenePreset p;
        p.name = "Liquid Chime Clean";
        p.category = "Glassy Clean";
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .dryWet = 1.0f}
        });
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .dryWet = 0.35f}
        });
        addIfMissing(std::move(p));
    }

    // 7. Edge of Breakup: Texas Blues
    {
        ScenePreset p;
        p.name = "Texas Blues Overdrive";
        p.category = "Edge of Breakup";
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Drive", .path = "builtin://drive", .type = "BuiltIn", .dryWet = 0.45f}
        });
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .dryWet = 1.0f}
        });
        addIfMissing(std::move(p));
    }

    // 8. Ambient FX: Cosmic Wash
    {
        ScenePreset p;
        p.name = "Cosmic Wash Shimmer";
        p.category = "Ambient FX";
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .dryWet = 1.0f}
        });
        p.nodes.push_back(NodePreset{
            .kind = NodePreset::Kind::Plugin,
            .slot = PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .dryWet = 0.90f}
        });
        addIfMissing(std::move(p));
    }
}

static void serializePreset(std::ostream& os, const ScenePreset& p, const std::string& sectionPrefix) {
    os << "[" << sectionPrefix << "]\n";
    os << "name=" << p.name << "\n";
    if (!p.category.empty()) {
        os << "category=" << p.category << "\n";
    }
    os << "numNodes=" << p.nodes.size() << "\n";
    for (size_t n = 0; n < p.nodes.size(); ++n) {
        const auto& node = p.nodes[n];
        os << "node_" << n << "_kind=" << (node.kind == NodePreset::Kind::Plugin ? "plugin" : "parallel") << "\n";
        if (node.kind == NodePreset::Kind::Plugin) {
            os << "node_" << n << "_name=" << node.slot.name << "\n";
            os << "node_" << n << "_path=" << node.slot.path << "\n";
            os << "node_" << n << "_type=" << node.slot.type << "\n";
            os << "node_" << n << "_bypassed=" << (node.slot.bypassed ? "1" : "0") << "\n";
            os << "node_" << n << "_dryWet=" << node.slot.dryWet << "\n";
            os << "node_" << n << "_inGain=" << node.slot.inputGainDb << "\n";
            os << "node_" << n << "_outGain=" << node.slot.outputGainDb << "\n";
            if (!node.slot.bundleFileName.empty()) os << "node_" << n << "_bundle=" << node.slot.bundleFileName << "\n";
            if (!node.slot.classGuid.empty()) os << "node_" << n << "_guid=" << node.slot.classGuid << "\n";
            if (!node.slot.clapId.empty()) os << "node_" << n << "_clapId=" << node.slot.clapId << "\n";
            if (!node.slot.state.empty()) {
                os << "node_" << n << "_state=" << bytesToHex(node.slot.state) << "\n";
            }
            os << "node_" << n << "_numMacros=" << node.slot.macros.size() << "\n";
            for (size_t m = 0; m < node.slot.macros.size(); ++m) {
                const auto& mb = node.slot.macros[m];
                os << "node_" << n << "_m_" << m << "_id=" << mb.paramId << "\n";
                os << "node_" << n << "_m_" << m << "_lbl=" << mb.label << "\n";
                os << "node_" << n << "_m_" << m << "_val=" << mb.value << "\n";
                os << "node_" << n << "_m_" << m << "_def=" << mb.defaultValue << "\n";
                os << "node_" << n << "_m_" << m << "_min=" << mb.minValue << "\n";
                os << "node_" << n << "_m_" << m << "_max=" << mb.maxValue << "\n";
                os << "node_" << n << "_m_" << m << "_bnd=" << (mb.bound ? "1" : "0") << "\n";
                os << "node_" << n << "_m_" << m << "_mix=" << (mb.isSlotMix ? "1" : "0") << "\n";
                os << "node_" << n << "_m_" << m << "_trm=" << (mb.isSlotTrim ? "1" : "0") << "\n";
            }
        } else {
            os << "node_" << n << "_branchBlend=" << node.branchBlend << "\n";
            os << "node_" << n << "_numBranches=" << node.branches.size() << "\n";
            for (size_t b = 0; b < node.branches.size(); ++b) {
                const auto& br = node.branches[b];
                os << "node_" << n << "_b_" << b << "_name=" << br.name << "\n";
                os << "node_" << n << "_b_" << b << "_gain=" << br.gainDb << "\n";
                os << "node_" << n << "_b_" << b << "_pan=" << br.pan << "\n";
                os << "node_" << n << "_b_" << b << "_muted=" << (br.muted ? "1" : "0") << "\n";
                os << "node_" << n << "_b_" << b << "_solo=" << (br.solo ? "1" : "0") << "\n";
                os << "node_" << n << "_b_" << b << "_phase=" << (br.phaseInvert ? "1" : "0") << "\n";
                os << "node_" << n << "_b_" << b << "_numSlots=" << br.slots.size() << "\n";
                for (size_t s = 0; s < br.slots.size(); ++s) {
                    const auto& sl = br.slots[s];
                    os << "node_" << n << "_b_" << b << "_s_" << s << "_name=" << sl.name << "\n";
                    os << "node_" << n << "_b_" << b << "_s_" << s << "_path=" << sl.path << "\n";
                    os << "node_" << n << "_b_" << b << "_s_" << s << "_type=" << sl.type << "\n";
                    os << "node_" << n << "_b_" << b << "_s_" << s << "_bypassed=" << (sl.bypassed ? "1" : "0") << "\n";
                    os << "node_" << n << "_b_" << b << "_s_" << s << "_dryWet=" << sl.dryWet << "\n";
                    os << "node_" << n << "_b_" << b << "_s_" << s << "_inGain=" << sl.inputGainDb << "\n";
                    os << "node_" << n << "_b_" << b << "_s_" << s << "_outGain=" << sl.outputGainDb << "\n";
                    if (!sl.bundleFileName.empty()) os << "node_" << n << "_b_" << b << "_s_" << s << "_bundle=" << sl.bundleFileName << "\n";
                    if (!sl.classGuid.empty()) os << "node_" << n << "_b_" << b << "_s_" << s << "_guid=" << sl.classGuid << "\n";
                    if (!sl.clapId.empty()) os << "node_" << n << "_b_" << b << "_s_" << s << "_clapId=" << sl.clapId << "\n";
                    if (!sl.state.empty()) {
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_state=" << bytesToHex(sl.state) << "\n";
                    }
                    os << "node_" << n << "_b_" << b << "_s_" << s << "_numMacros=" << sl.macros.size() << "\n";
                    for (size_t m = 0; m < sl.macros.size(); ++m) {
                        const auto& mb = sl.macros[m];
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_m_" << m << "_id=" << mb.paramId << "\n";
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_m_" << m << "_lbl=" << mb.label << "\n";
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_m_" << m << "_val=" << mb.value << "\n";
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_m_" << m << "_def=" << mb.defaultValue << "\n";
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_m_" << m << "_min=" << mb.minValue << "\n";
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_m_" << m << "_max=" << mb.maxValue << "\n";
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_m_" << m << "_bnd=" << (mb.bound ? "1" : "0") << "\n";
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_m_" << m << "_mix=" << (mb.isSlotMix ? "1" : "0") << "\n";
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_m_" << m << "_trm=" << (mb.isSlotTrim ? "1" : "0") << "\n";
                    }
                }
            }
        }
    }
    os << "\n";
}

bool SceneManager::saveToFile(const std::string& filePath) const {
    std::string path = filePath.empty() ? (AppConfig::getConfigDir() + "\\presets.ini") : filePath;
    std::ofstream file(path);
    if (!file.is_open()) return false;

    file << "# Praccy Preset & Scene Chains File\n";
    file << "numScenes=" << m_scenes.size() << "\n";
    file << "numUserPresets=" << m_userPresets.size() << "\n\n";

    for (size_t i = 0; i < m_scenes.size(); ++i) {
        serializePreset(file, m_scenes[i], "Scene_" + std::to_string(i));
    }

    for (size_t i = 0; i < m_userPresets.size(); ++i) {
        serializePreset(file, m_userPresets[i], "UserPreset_" + std::to_string(i));
    }

    return true;
}

bool SceneManager::loadFromFile(const std::string& filePath) {
    std::string path = filePath.empty() ? (AppConfig::getConfigDir() + "\\presets.ini") : filePath;
    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> sections;
    std::string currentSection;
    std::string line;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            currentSection = line.substr(1, line.length() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq != std::string::npos && !currentSection.empty()) {
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            sections[currentSection][k] = v;
        }
    }

    auto parseMacros = [](const std::unordered_map<std::string, std::string>& kv,
                          const std::string& pfx,
                          std::vector<PluginSlotPreset::MacroBinding>& outMacros) {
        size_t numMacros = kv.count(pfx + "numMacros") ? praccy::utils::parseInteger<size_t>(kv.at(pfx + "numMacros"), 0) : 0;
        for (size_t m = 0; m < numMacros; ++m) {
            std::string mpfx = pfx + "m_" + std::to_string(m) + "_";
            PluginSlotPreset::MacroBinding mb;
            mb.paramId = kv.count(mpfx + "id") ? praccy::utils::parseInteger<uint32_t>(kv.at(mpfx + "id"), 0) : 0;
            mb.label = kv.count(mpfx + "lbl") ? kv.at(mpfx + "lbl") : "";
            mb.value = kv.count(mpfx + "val") ? praccy::utils::parseFloat(kv.at(mpfx + "val"), 0.0f) : 0.0f;
            mb.defaultValue = kv.count(mpfx + "def") ? praccy::utils::parseFloat(kv.at(mpfx + "def"), 0.0f) : 0.0f;
            mb.minValue = kv.count(mpfx + "min") ? praccy::utils::parseFloat(kv.at(mpfx + "min"), 0.0f) : 0.0f;
            mb.maxValue = kv.count(mpfx + "max") ? praccy::utils::parseFloat(kv.at(mpfx + "max"), 1.0f) : 1.0f;
            mb.bound = kv.count(mpfx + "bnd") && kv.at(mpfx + "bnd") == "1";
            mb.isSlotMix = kv.count(mpfx + "mix") && kv.at(mpfx + "mix") == "1";
            mb.isSlotTrim = kv.count(mpfx + "trm") && kv.at(mpfx + "trm") == "1";
            outMacros.push_back(std::move(mb));
        }
    };

    auto parsePreset = [&](const std::unordered_map<std::string, std::string>& kv) -> ScenePreset {
        ScenePreset sp;
        auto itName = kv.find("name");
        if (itName != kv.end()) sp.name = itName->second;
        auto itCat = kv.find("category");
        if (itCat != kv.end()) sp.category = itCat->second;
        auto itNodes = kv.find("numNodes");
        size_t count = (itNodes != kv.end()) ? praccy::utils::parseInteger<size_t>(itNodes->second, 0) : 0;

        for (size_t n = 0; n < count; ++n) {
            std::string pfx = "node_" + std::to_string(n) + "_";
            NodePreset np;
            std::string kind = kv.count(pfx + "kind") ? kv.at(pfx + "kind") : "plugin";
            if (kind == "plugin") {
                np.kind = NodePreset::Kind::Plugin;
                np.slot.name = kv.count(pfx + "name") ? kv.at(pfx + "name") : "Plugin";
                np.slot.path = kv.count(pfx + "path") ? kv.at(pfx + "path") : "";
                np.slot.type = kv.count(pfx + "type") ? kv.at(pfx + "type") : "BuiltIn";
                np.slot.bundleFileName = kv.count(pfx + "bundle") ? kv.at(pfx + "bundle") : "";
                np.slot.classGuid = kv.count(pfx + "guid") ? kv.at(pfx + "guid") : "";
                np.slot.clapId = kv.count(pfx + "clapId") ? kv.at(pfx + "clapId") : "";
                np.slot.bypassed = kv.count(pfx + "bypassed") && kv.at(pfx + "bypassed") == "1";
                np.slot.dryWet = kv.count(pfx + "dryWet") ? praccy::utils::parseFloat(kv.at(pfx + "dryWet"), 1.0f) : 1.0f;
                np.slot.inputGainDb = kv.count(pfx + "inGain") ? praccy::utils::parseFloat(kv.at(pfx + "inGain"), 0.0f) : 0.0f;
                np.slot.outputGainDb = kv.count(pfx + "outGain") ? praccy::utils::parseFloat(kv.at(pfx + "outGain"), 0.0f) : 0.0f;
                if (kv.count(pfx + "state")) {
                    np.slot.state = hexToBytes(kv.at(pfx + "state"));
                }
                parseMacros(kv, pfx, np.slot.macros);
            } else {
                np.kind = NodePreset::Kind::ParallelBlock;
                np.branchBlend = kv.count(pfx + "branchBlend") ? praccy::utils::parseFloat(kv.at(pfx + "branchBlend"), 0.0f) : 0.0f;
                size_t numBr = kv.count(pfx + "numBranches") ? praccy::utils::parseInteger<size_t>(kv.at(pfx + "numBranches"), 0) : 0;
                for (size_t b = 0; b < numBr; ++b) {
                    std::string bpfx = pfx + "b_" + std::to_string(b) + "_";
                    BranchPreset bp;
                    bp.name = kv.count(bpfx + "name") ? kv.at(bpfx + "name") : "Branch";
                    bp.gainDb = kv.count(bpfx + "gain") ? praccy::utils::parseFloat(kv.at(bpfx + "gain"), 0.0f) : 0.0f;
                    bp.pan = kv.count(bpfx + "pan") ? praccy::utils::parseFloat(kv.at(bpfx + "pan"), 0.0f) : 0.0f;
                    bp.muted = kv.count(bpfx + "muted") && kv.at(bpfx + "muted") == "1";
                    bp.solo = kv.count(bpfx + "solo") && kv.at(bpfx + "solo") == "1";
                    bp.phaseInvert = kv.count(bpfx + "phase") && kv.at(bpfx + "phase") == "1";

                    size_t numSl = kv.count(bpfx + "numSlots") ? praccy::utils::parseInteger<size_t>(kv.at(bpfx + "numSlots"), 0) : 0;
                    for (size_t s = 0; s < numSl; ++s) {
                        std::string spfx = bpfx + "s_" + std::to_string(s) + "_";
                        PluginSlotPreset sl;
                        sl.name = kv.count(spfx + "name") ? kv.at(spfx + "name") : "Plugin";
                        sl.path = kv.count(spfx + "path") ? kv.at(spfx + "path") : "";
                        sl.type = kv.count(spfx + "type") ? kv.at(spfx + "type") : "BuiltIn";
                        sl.bundleFileName = kv.count(spfx + "bundle") ? kv.at(spfx + "bundle") : "";
                        sl.classGuid = kv.count(spfx + "guid") ? kv.at(spfx + "guid") : "";
                        sl.clapId = kv.count(spfx + "clapId") ? kv.at(spfx + "clapId") : "";
                        sl.bypassed = kv.count(spfx + "bypassed") && kv.at(spfx + "bypassed") == "1";
                        sl.dryWet = kv.count(spfx + "dryWet") ? praccy::utils::parseFloat(kv.at(spfx + "dryWet"), 1.0f) : 1.0f;
                        sl.inputGainDb = kv.count(spfx + "inGain") ? praccy::utils::parseFloat(kv.at(spfx + "inGain"), 0.0f) : 0.0f;
                        sl.outputGainDb = kv.count(spfx + "outGain") ? praccy::utils::parseFloat(kv.at(spfx + "outGain"), 0.0f) : 0.0f;
                        if (kv.count(spfx + "state")) {
                            sl.state = hexToBytes(kv.at(spfx + "state"));
                        }
                        parseMacros(kv, spfx, sl.macros);
                        bp.slots.push_back(std::move(sl));
                    }
                    np.branches.push_back(std::move(bp));
                }
            }
            sp.nodes.push_back(std::move(np));
        }
        return sp;
    };

    m_scenes.clear();
    for (int i = 0; i < 4; ++i) {
        std::string sec = "Scene_" + std::to_string(i);
        if (sections.count(sec)) {
            m_scenes.push_back(parsePreset(sections[sec]));
        }
    }
    if (m_scenes.empty()) {
        initializeDefaultScenes();
    }

    m_userPresets.clear();
    for (size_t i = 0; ; ++i) {
        std::string sec = "UserPreset_" + std::to_string(i);
        if (!sections.count(sec)) break;
        m_userPresets.push_back(parsePreset(sections[sec]));
    }
    initializeDefaultUserPresets();

    return true;
}

} // namespace praccy::state
