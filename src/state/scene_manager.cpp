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
        .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .bypassed = false, .dryWet = 1.0f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}},
        .branches = {}
    });
    s1.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .bypassed = false, .dryWet = 0.25f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}},
        .branches = {}
    });
    m_scenes.push_back(std::move(s1));

    // 2: Crunch
    ScenePreset s2;
    s2.name = "2: Crunch";
    s2.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Drive", .path = "builtin://drive", .type = "BuiltIn", .bypassed = false, .dryWet = 0.8f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}},
        .branches = {}
    });
    s2.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .bypassed = false, .dryWet = 1.0f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}},
        .branches = {}
    });
    m_scenes.push_back(std::move(s2));

    // 3: Lead
    ScenePreset s3;
    s3.name = "3: Lead";
    s3.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Drive", .path = "builtin://drive", .type = "BuiltIn", .bypassed = false, .dryWet = 1.0f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}},
        .branches = {}
    });
    s3.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .bypassed = false, .dryWet = 1.0f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}},
        .branches = {}
    });
    s3.nodes.push_back(NodePreset{
        .kind = NodePreset::Kind::Plugin,
        .slot = PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .bypassed = false, .dryWet = 0.45f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}},
        .branches = {}
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
    bA.slots.push_back(PluginSlotPreset{.name = "Praccy Amp Sim", .path = "builtin://amp", .type = "BuiltIn", .bypassed = false, .dryWet = 1.0f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}});

    BranchPreset bB;
    bB.name = "Branch B (Ambient Delay)";
    bB.gainDb = -2.0f;
    bB.pan = 0.3f;
    bB.slots.push_back(PluginSlotPreset{.name = "Praccy Stereo Delay", .path = "builtin://delay", .type = "BuiltIn", .bypassed = false, .dryWet = 0.9f, .inputGainDb = 0.0f, .outputGainDb = 0.0f, .state = {}});

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
        return nullptr;
    }

    if (!p.state.empty()) {
        inst->loadState(p.state);
    }

    auto slot = std::make_unique<audio::PluginSlot>(std::move(inst));
    slot->setBypassed(p.bypassed);
    slot->setDryWet(p.dryWet);
    slot->setInputGainDb(p.inputGainDb);
    slot->setOutputGainDb(p.outputGainDb);
    return slot;
}

void SceneManager::captureCurrentScene(int sceneIndex, audio::GraphEngine& graph) {
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(m_scenes.size())) return;

    auto& scene = m_scenes[sceneIndex];
    scene.nodes.clear();

    const size_t numNodes = graph.numNodes();
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            if (slot) {
                NodePreset np;
                np.kind = NodePreset::Kind::Plugin;
                np.slot.name = slot->name();
                np.slot.bypassed = slot->isBypassed();
                np.slot.dryWet = slot->dryWet();
                np.slot.inputGainDb = slot->inputGainDb();
                np.slot.outputGainDb = slot->outputGainDb();

                auto* inner = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
                if (inner) {
                    np.slot.path = inner->path();
                    if (dynamic_cast<plugins::Vst3PluginInstance*>(inner)) {
                        np.slot.type = "VST3";
                    } else if (dynamic_cast<plugins::ClapPluginInstance*>(inner)) {
                        np.slot.type = "CLAP";
                    } else {
                        np.slot.type = "BuiltIn";
                    }
                    np.slot.state = inner->saveState();
                }
                scene.nodes.push_back(std::move(np));
            }
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            if (block) {
                NodePreset np;
                np.kind = NodePreset::Kind::ParallelBlock;
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
                        sp.name = bSlot->name();
                        sp.bypassed = bSlot->isBypassed();
                        sp.dryWet = bSlot->dryWet();
                        sp.inputGainDb = bSlot->inputGainDb();
                        sp.outputGainDb = bSlot->outputGainDb();

                        auto* inner = dynamic_cast<plugins::IPluginInstance*>(bSlot->innerNode());
                        if (inner) {
                            sp.path = inner->path();
                            if (dynamic_cast<plugins::Vst3PluginInstance*>(inner)) {
                                sp.type = "VST3";
                            } else if (dynamic_cast<plugins::ClapPluginInstance*>(inner)) {
                                sp.type = "CLAP";
                            } else {
                                sp.type = "BuiltIn";
                            }
                            sp.state = inner->saveState();
                        }
                        bp.slots.push_back(std::move(sp));
                    }
                    np.branches.push_back(std::move(bp));
                }
                scene.nodes.push_back(std::move(np));
            }
        }
    }

    m_statusMessage = "Preset saved: " + scene.name;
    saveToFile();
}

bool SceneManager::applyScene(int sceneIndex, audio::GraphEngine& graph) {
    if (sceneIndex < 0 || sceneIndex >= static_cast<int>(m_scenes.size())) return false;

    plugins::PluginWindowManager::instance().closeAllWindows();

    const auto& scene = m_scenes[sceneIndex];
    m_activeSceneIndex = sceneIndex;

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
    m_statusMessage = "Loaded preset: " + scene.name;
    return true;
}

void SceneManager::savePresetChain(const std::string& name, audio::GraphEngine& graph) {
    if (name.empty()) return;

    // Check if updating existing user preset or creating new
    auto it = std::find_if(m_userPresets.begin(), m_userPresets.end(), [&](const ScenePreset& p) {
        return p.name == name;
    });

    ScenePreset newPreset;
    newPreset.name = name;

    const size_t numNodes = graph.numNodes();
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            if (slot) {
                NodePreset np;
                np.kind = NodePreset::Kind::Plugin;
                np.slot.name = slot->name();
                np.slot.bypassed = slot->isBypassed();
                np.slot.dryWet = slot->dryWet();
                np.slot.inputGainDb = slot->inputGainDb();
                np.slot.outputGainDb = slot->outputGainDb();

                auto* inner = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
                if (inner) {
                    np.slot.path = inner->path();
                    if (dynamic_cast<plugins::Vst3PluginInstance*>(inner)) {
                        np.slot.type = "VST3";
                    } else if (dynamic_cast<plugins::ClapPluginInstance*>(inner)) {
                        np.slot.type = "CLAP";
                    } else {
                        np.slot.type = "BuiltIn";
                    }
                    np.slot.state = inner->saveState();
                }
                newPreset.nodes.push_back(std::move(np));
            }
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            if (block) {
                NodePreset np;
                np.kind = NodePreset::Kind::ParallelBlock;
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
                        sp.name = bSlot->name();
                        sp.bypassed = bSlot->isBypassed();
                        sp.dryWet = bSlot->dryWet();
                        sp.inputGainDb = bSlot->inputGainDb();
                        sp.outputGainDb = bSlot->outputGainDb();

                        auto* inner = dynamic_cast<plugins::IPluginInstance*>(bSlot->innerNode());
                        if (inner) {
                            sp.path = inner->path();
                            if (dynamic_cast<plugins::Vst3PluginInstance*>(inner)) {
                                sp.type = "VST3";
                            } else if (dynamic_cast<plugins::ClapPluginInstance*>(inner)) {
                                sp.type = "CLAP";
                            } else {
                                sp.type = "BuiltIn";
                            }
                            sp.state = inner->saveState();
                        }
                        bp.slots.push_back(std::move(sp));
                    }
                    np.branches.push_back(std::move(bp));
                }
                newPreset.nodes.push_back(std::move(np));
            }
        }
    }

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

    plugins::PluginWindowManager::instance().closeAllWindows();

    std::vector<std::unique_ptr<audio::AudioNode>> newNodes;

    for (const auto& np : it->nodes) {
        if (np.kind == NodePreset::Kind::Plugin) {
            auto slot = createSlotFromPreset(np.slot);
            if (slot) {
                slot->prepare(graph.sampleRate(), graph.maxBlockSize());
                newNodes.push_back(std::move(slot));
            }
        } else if (np.kind == NodePreset::Kind::ParallelBlock) {
            auto block = std::make_unique<audio::ParallelSplitMergeBlock>();
            block->prepare(graph.sampleRate(), graph.maxBlockSize());
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
    m_statusMessage = "Loaded preset: " + name;
    return true;
}

std::vector<std::string> SceneManager::savedPresetNames() const {
    std::vector<std::string> names;
    for (const auto& p : m_userPresets) {
        names.push_back(p.name);
    }
    return names;
}

static void serializePreset(std::ostream& os, const ScenePreset& p, const std::string& sectionPrefix) {
    os << "[" << sectionPrefix << "]\n";
    os << "name=" << p.name << "\n";
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
            if (!node.slot.state.empty()) {
                os << "node_" << n << "_state=" << bytesToHex(node.slot.state) << "\n";
            }
        } else {
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
                    if (!sl.state.empty()) {
                        os << "node_" << n << "_b_" << b << "_s_" << s << "_state=" << bytesToHex(sl.state) << "\n";
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

    auto parsePreset = [&](const std::unordered_map<std::string, std::string>& kv) -> ScenePreset {
        ScenePreset sp;
        auto itName = kv.find("name");
        if (itName != kv.end()) sp.name = itName->second;
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
                np.slot.bypassed = kv.count(pfx + "bypassed") && kv.at(pfx + "bypassed") == "1";
                np.slot.dryWet = kv.count(pfx + "dryWet") ? praccy::utils::parseFloat(kv.at(pfx + "dryWet"), 1.0f) : 1.0f;
                np.slot.inputGainDb = kv.count(pfx + "inGain") ? praccy::utils::parseFloat(kv.at(pfx + "inGain"), 0.0f) : 0.0f;
                np.slot.outputGainDb = kv.count(pfx + "outGain") ? praccy::utils::parseFloat(kv.at(pfx + "outGain"), 0.0f) : 0.0f;
                if (kv.count(pfx + "state")) {
                    np.slot.state = hexToBytes(kv.at(pfx + "state"));
                }
            } else {
                np.kind = NodePreset::Kind::ParallelBlock;
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
                        sl.bypassed = kv.count(spfx + "bypassed") && kv.at(spfx + "bypassed") == "1";
                        sl.dryWet = kv.count(spfx + "dryWet") ? praccy::utils::parseFloat(kv.at(spfx + "dryWet"), 1.0f) : 1.0f;
                        sl.inputGainDb = kv.count(spfx + "inGain") ? praccy::utils::parseFloat(kv.at(spfx + "inGain"), 0.0f) : 0.0f;
                        sl.outputGainDb = kv.count(spfx + "outGain") ? praccy::utils::parseFloat(kv.at(spfx + "outGain"), 0.0f) : 0.0f;
                        if (kv.count(spfx + "state")) {
                            sl.state = hexToBytes(kv.at(spfx + "state"));
                        }
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

    return true;
}

} // namespace praccy::state
