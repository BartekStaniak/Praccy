#include "ableton_adapter.h"
#include "../../utils/archive_utils.h"
#include "../../utils/xml_parser.h"
#include "../../utils/base64_utils.h"
#include "../../utils/guid_utils.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace praccy::state {

bool AbletonAdapter::canHandle(const std::filesystem::path& path) const {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
    return ext == ".adg";
}

static void parseAbletonPluginDevice(const std::shared_ptr<utils::XmlNode>& node, PluginSlotPreset& slot) {
    bool hasExplicitType = false;
    slot.type.clear();

    if (node->hasAttribute("Type")) {
        slot.type = node->attribute("Type");
        hasExplicitType = true;
    }
    if (node->hasAttribute("Path")) {
        slot.path = node->attribute("Path");
    }

    // 1. Name & Guid
    auto desc = node->firstChild("PluginDesc");
    if (desc) {
        if (desc->hasAttribute("Type")) {
            slot.type = desc->attribute("Type");
            hasExplicitType = true;
        }
        auto typeNode = desc->firstChild("Type");
        if (typeNode && typeNode->hasAttribute("Value")) {
            slot.type = typeNode->attribute("Value");
            hasExplicitType = true;
        }

        if (desc->hasAttribute("Path")) {
            slot.path = desc->attribute("Path");
        }
        auto pathNode = desc->firstChild("Path");
        if (pathNode && pathNode->hasAttribute("Value")) {
            slot.path = pathNode->attribute("Value");
        }

        auto nameNode = desc->firstChild("Name");
        if (nameNode) {
            slot.name = nameNode->attribute("Value");
        }

        auto vst3Info = desc->firstChild("Vst3PluginInfo");
        if (vst3Info) {
            slot.type = "VST3";
            hasExplicitType = true;
            auto plugId = vst3Info->firstChild("PlugId");
            if (plugId) {
                std::string rawId = plugId->attribute("Value");
                slot.classGuid = utils::GuidUtils::normalizeToCanonicalGuid(rawId, utils::DawGuidFormat::AbletonBigEndian);
            }
        }

        auto clapInfo = desc->firstChild("ClapPluginInfo");
        if (clapInfo) {
            slot.type = "CLAP";
            hasExplicitType = true;
            auto plugId = clapInfo->firstChild("PlugId");
            if (plugId) {
                slot.clapId = plugId->attribute("Value");
            }
        }

        auto builtInInfo = desc->firstChild("BuiltInPluginInfo");
        if (builtInInfo) {
            slot.type = "BuiltIn";
            hasExplicitType = true;
            auto bPath = builtInInfo->firstChild("BuiltInPath");
            if (bPath && bPath->hasAttribute("Value")) {
                slot.path = bPath->attribute("Value");
            }
        }
    }

    if (!hasExplicitType) {
        slot.type.clear();
    }

    if (slot.name.empty()) {
        auto userName = node->firstChild("UserName");
        if (userName) {
            slot.name = userName->attribute("Value");
        }
    }

    if (slot.name.empty()) {
        if (slot.type == "CLAP") slot.name = "CLAP Plugin";
        else if (slot.type == "BuiltIn") slot.name = "BuiltIn Plugin";
        else slot.name = "VST3 Plugin";
    }

    // 2. Binary processor state
    auto procState = node->firstChild("ProcessorState");
    if (procState && !procState->text.empty()) {
        utils::Base64::decode(procState->text, slot.state);
    }
}

static void extractDevicesRecursive(
    const std::shared_ptr<utils::XmlNode>& containerNode,
    std::vector<PluginSlotPreset>& outSlots
) {
    if (!containerNode) return;

    for (const auto& child : containerNode->children) {
        if (!child) continue;

        if (child->tag == "PluginDevice") {
            PluginSlotPreset slot;
            parseAbletonPluginDevice(child, slot);
            outSlots.push_back(std::move(slot));
        } else if (child->tag == "AudioEffectGroupDevice") {
            // Nested rack: unroll all branches recursively into parent chain
            auto branchesNode = child->firstChild("Branches");
            if (branchesNode) {
                for (const auto& branch : branchesNode->children) {
                    if (branch) {
                        auto devChain = branch->firstChild("DeviceChain");
                        if (devChain) {
                            auto devices = devChain->firstChild("Devices");
                            if (devices) {
                                extractDevicesRecursive(devices, outSlots);
                            }
                        }
                    }
                }
            }
        } else {
            // Check deeper in case devices are inside a wrapper tag
            if (!child->children.empty()) {
                extractDevicesRecursive(child, outSlots);
            }
        }
    }
}

static BranchPreset parseAbletonBranch(const std::shared_ptr<utils::XmlNode>& branch, size_t defaultIndex) {
    BranchPreset bp;
    bp.name = (defaultIndex == 0 ? "Branch A" : "Branch B");

    if (!branch) return bp;

    auto userNameNode = branch->firstChild("UserName");
    if (userNameNode && userNameNode->hasAttribute("Value") && !userNameNode->attribute("Value").empty()) {
        bp.name = userNameNode->attribute("Value");
    } else if (branch->hasAttribute("Name") && !branch->attribute("Name").empty()) {
        bp.name = branch->attribute("Name");
    }

    auto getFloatVal = [](const std::shared_ptr<utils::XmlNode>& parent, std::string_view tag, std::string_view attr, float defaultVal) -> float {
        if (!parent) return defaultVal;
        if (parent->hasAttribute(attr)) {
            return std::strtof(parent->attribute(attr).c_str(), nullptr);
        }
        if (parent->hasAttribute(tag)) {
            return std::strtof(parent->attribute(tag).c_str(), nullptr);
        }
        auto child = parent->firstChild(tag);
        if (child) {
            if (child->hasAttribute("Value")) {
                return std::strtof(child->attribute("Value").c_str(), nullptr);
            }
            if (!child->text.empty()) {
                return std::strtof(child->text.c_str(), nullptr);
            }
        }
        return defaultVal;
    };

    auto getBoolVal = [](const std::shared_ptr<utils::XmlNode>& parent, std::string_view tag, std::string_view attr, bool defaultVal) -> bool {
        if (!parent) return defaultVal;
        auto parseBoolStr = [](std::string_view s) -> bool {
            return s == "1" || s == "true" || s == "True" || s == "TRUE" || s == "yes";
        };
        if (parent->hasAttribute(attr)) {
            return parseBoolStr(parent->attribute(attr));
        }
        if (parent->hasAttribute(tag)) {
            return parseBoolStr(parent->attribute(tag));
        }
        auto child = parent->firstChild(tag);
        if (child) {
            if (child->hasAttribute("Value")) {
                return parseBoolStr(child->attribute("Value"));
            }
            if (!child->text.empty()) {
                return parseBoolStr(child->text);
            }
        }
        return defaultVal;
    };

    auto mixerNode = branch->firstChild("BranchMixer");
    if (!mixerNode) mixerNode = branch->firstChild("Mixer");

    if (mixerNode) {
        bp.gainDb = getFloatVal(mixerNode, "GainDb", "gainDb", 0.0f);
        bp.pan = getFloatVal(mixerNode, "Pan", "pan", 0.0f);
        bp.muted = getBoolVal(mixerNode, "Muted", "muted", false);
        bp.solo = getBoolVal(mixerNode, "Soloed", "soloed", false) || getBoolVal(mixerNode, "Solo", "solo", false);
        bp.phaseInvert = getBoolVal(mixerNode, "PhaseInverted", "phaseInverted", false) || getBoolVal(mixerNode, "PhaseInvert", "phaseInvert", false);
    }
    if (bp.gainDb == 0.0f) {
        bp.gainDb = getFloatVal(branch, "GainDb", "gainDb", 0.0f);
    }
    if (bp.pan == 0.0f) {
        bp.pan = getFloatVal(branch, "Pan", "pan", 0.0f);
    }
    if (!bp.muted) {
        bp.muted = getBoolVal(branch, "Muted", "muted", false);
    }
    if (!bp.solo) {
        bp.solo = getBoolVal(branch, "Soloed", "soloed", false) || getBoolVal(branch, "Solo", "solo", false);
    }
    if (!bp.phaseInvert) {
        bp.phaseInvert = getBoolVal(branch, "PhaseInverted", "phaseInverted", false) || getBoolVal(branch, "PhaseInvert", "phaseInvert", false);
    }

    auto devChain = branch->firstChild("DeviceChain");
    if (devChain) {
        auto devices = devChain->firstChild("Devices");
        if (devices) {
            extractDevicesRecursive(devices, bp.slots);
        }
    }

    if (!bp.slots.empty()) {
        for (auto& s : bp.slots) {
            if (s.type.empty()) {
                if (branch->hasAttribute("Type") && branch->attribute("Type") != "Mixer") {
                    s.type = branch->attribute("Type");
                } else {
                    s.type = "VST3";
                }
            }
            if (s.path.empty()) {
                if (branch->hasAttribute("Path")) s.path = branch->attribute("Path");
            }
        }
    } else {
        // Branch has no devices in DeviceChain, but branch itself may declare an effect
        if (branch->hasAttribute("Type")) {
            std::string bType = branch->attribute("Type");
            if (bType == "CLAP" || bType == "BuiltIn" || bType == "VST3") {
                PluginSlotPreset slot;
                slot.type = bType;
                if (branch->hasAttribute("Path")) {
                    slot.path = branch->attribute("Path");
                }
                slot.name = bp.name;
                bp.slots.push_back(std::move(slot));
            }
        }
    }

    return bp;
}

std::optional<ScenePreset> AbletonAdapter::parseToScene(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    std::vector<uint8_t> fileBytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (fileBytes.empty()) return std::nullopt;

    std::string xmlStr;
    // Check for Gzip magic: 0x1F, 0x8B
    if (fileBytes.size() >= 2 && fileBytes[0] == 0x1F && fileBytes[1] == 0x8B) {
        if (!utils::ArchiveUtils::gzipDecompress(fileBytes, xmlStr)) {
            return std::nullopt;
        }
    } else {
        xmlStr.assign(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
    }

    auto root = utils::XmlParser::parse(xmlStr);
    if (!root) return std::nullopt;

    ScenePreset scene;
    scene.name = path.stem().string();
    scene.sourceDawName = "Ableton";
    scene.tempoBpm = 120.0f;
    scene.timeSigNumerator = 4;
    scene.timeSigDenominator = 4;

    // 1. Check for root serial chain in <DeviceChain><Devices> or <Devices>
    std::shared_ptr<utils::XmlNode> devicesContainer;
    auto devChain = root->firstChild("DeviceChain");
    if (devChain) {
        devicesContainer = devChain->firstChild("Devices");
    } else if (root->tag == "Devices") {
        devicesContainer = root;
    } else {
        devicesContainer = root->firstChild("Devices");
    }

    if (devicesContainer) {
        for (const auto& child : devicesContainer->children) {
            if (!child) continue;
            if (child->tag == "PluginDevice") {
                PluginSlotPreset slot;
                parseAbletonPluginDevice(child, slot);
                if (slot.type.empty()) slot.type = "VST3";
                NodePreset np;
                np.kind = NodePreset::Kind::Plugin;
                np.slot = std::move(slot);
                scene.nodes.push_back(std::move(np));
            } else if (child->tag == "AudioEffectGroupDevice") {
                auto branchesNode = child->firstChild("Branches");
                if (branchesNode && !branchesNode->children.empty()) {
                    std::vector<BranchPreset> parsedBranches;
                    size_t bIndex = 0;
                    for (const auto& branch : branchesNode->children) {
                        if (!branch) continue;
                        parsedBranches.push_back(parseAbletonBranch(branch, bIndex++));
                    }

                    if (parsedBranches.size() == 1) {
                        for (auto& s : parsedBranches[0].slots) {
                            NodePreset np;
                            np.kind = NodePreset::Kind::Plugin;
                            np.slot = std::move(s);
                            scene.nodes.push_back(std::move(np));
                        }
                    } else if (parsedBranches.size() >= 2) {
                        NodePreset parBlock;
                        parBlock.kind = NodePreset::Kind::ParallelBlock;
                        parBlock.branchBlend = 0.0f;

                        BranchPreset bA = std::move(parsedBranches[0]);
                        BranchPreset bB = std::move(parsedBranches[1]);

                        for (size_t bIdx = 2; bIdx < parsedBranches.size(); ++bIdx) {
                            for (auto& extraSlot : parsedBranches[bIdx].slots) {
                                bB.slots.push_back(std::move(extraSlot));
                            }
                        }

                        parBlock.branches.push_back(std::move(bA));
                        parBlock.branches.push_back(std::move(bB));
                        scene.nodes.push_back(std::move(parBlock));
                    }
                }
            }
        }
    } else {
        // Fallback: primary rack directly at root: <AudioEffectGroupDevice>
        std::shared_ptr<utils::XmlNode> groupDevice = root->tag == "AudioEffectGroupDevice" ? root : root->firstChild("AudioEffectGroupDevice");
        if (groupDevice) {
            auto branchesNode = groupDevice->firstChild("Branches");
            if (branchesNode && !branchesNode->children.empty()) {
                std::vector<BranchPreset> parsedBranches;
                size_t bIndex = 0;
                for (const auto& branch : branchesNode->children) {
                    if (!branch) continue;
                    parsedBranches.push_back(parseAbletonBranch(branch, bIndex++));
                }

                if (parsedBranches.size() == 1) {
                    for (auto& s : parsedBranches[0].slots) {
                        NodePreset np;
                        np.kind = NodePreset::Kind::Plugin;
                        np.slot = std::move(s);
                        scene.nodes.push_back(std::move(np));
                    }
                } else if (parsedBranches.size() >= 2) {
                    NodePreset parBlock;
                    parBlock.kind = NodePreset::Kind::ParallelBlock;
                    parBlock.branchBlend = 0.0f;

                    BranchPreset bA = std::move(parsedBranches[0]);
                    BranchPreset bB = std::move(parsedBranches[1]);

                    for (size_t bIdx = 2; bIdx < parsedBranches.size(); ++bIdx) {
                        for (auto& extraSlot : parsedBranches[bIdx].slots) {
                            bB.slots.push_back(std::move(extraSlot));
                        }
                    }

                    parBlock.branches.push_back(std::move(bA));
                    parBlock.branches.push_back(std::move(bB));
                    scene.nodes.push_back(std::move(parBlock));
                }
            } else {
                std::vector<PluginSlotPreset> serialSlots;
                extractDevicesRecursive(groupDevice, serialSlots);
                for (auto& s : serialSlots) {
                    if (s.type.empty()) s.type = "VST3";
                    NodePreset np;
                    np.kind = NodePreset::Kind::Plugin;
                    np.slot = std::move(s);
                    scene.nodes.push_back(std::move(np));
                }
            }
        } else {
            std::vector<PluginSlotPreset> serialSlots;
            extractDevicesRecursive(root, serialSlots);
            for (auto& s : serialSlots) {
                if (s.type.empty()) s.type = "VST3";
                NodePreset np;
                np.kind = NodePreset::Kind::Plugin;
                np.slot = std::move(s);
                scene.nodes.push_back(std::move(np));
            }
        }
    }

    if (scene.nodes.empty()) return std::nullopt;
    return scene;
}

bool AbletonAdapter::exportFromScene(const ScenePreset& scene, const std::filesystem::path& outputPath) {
    std::ostringstream xml;
    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml << "<Ableton MajorVersion=\"5\" MinorVersion=\"12.0_12000\" SchemaChangeCount=\"3\" Creator=\"Ableton Live 12.0\" Revision=\"\">\n";
    xml << "  <DeviceChain>\n";
    xml << "    <Devices>\n";

    auto writePluginDeviceXml = [&](const PluginSlotPreset& slot, int id, int indent = 6) {
        std::string pad(indent, ' ');
        std::string plugType = slot.type.empty() ? "VST3" : slot.type;
        xml << pad << "<PluginDevice Id=\"" << id << "\" Type=\"" << plugType << "\"";
        if (!slot.path.empty()) {
            xml << " Path=\"" << utils::XmlParser::encodeEntities(slot.path) << "\"";
        }
        xml << ">\n";
        xml << pad << "  <PluginDesc>\n";
        xml << pad << "    <Type Value=\"" << plugType << "\" />\n";
        if (!slot.path.empty()) {
            xml << pad << "    <Path Value=\"" << utils::XmlParser::encodeEntities(slot.path) << "\" />\n";
        }
        if (plugType == "CLAP") {
            xml << pad << "    <ClapPluginInfo>\n";
            xml << pad << "      <PlugId Value=\"" << utils::XmlParser::encodeEntities(slot.clapId.empty() ? slot.name : slot.clapId) << "\" />\n";
            xml << pad << "    </ClapPluginInfo>\n";
        } else if (plugType == "BuiltIn") {
            xml << pad << "    <BuiltInPluginInfo>\n";
            xml << pad << "      <BuiltInPath Value=\"" << utils::XmlParser::encodeEntities(slot.path) << "\" />\n";
            xml << pad << "    </BuiltInPluginInfo>\n";
        } else {
            xml << pad << "    <Vst3PluginInfo>\n";
            xml << pad << "      <PlugId Value=\"" << utils::GuidUtils::toAbletonGuidString(slot.classGuid) << "\" />\n";
            xml << pad << "    </Vst3PluginInfo>\n";
        }
        xml << pad << "    <Name Value=\"" << utils::XmlParser::encodeEntities(slot.name) << "\" />\n";
        xml << pad << "  </PluginDesc>\n";
        if (!slot.state.empty()) {
            xml << pad << "  <ProcessorState><![CDATA["
                << utils::Base64::encode(slot.state)
                << "]]></ProcessorState>\n";
        }
        xml << pad << "</PluginDevice>\n";
    };

    int devId = 1;

    for (const auto& node : scene.nodes) {
        if (node.kind == NodePreset::Kind::Plugin) {
            writePluginDeviceXml(node.slot, devId++, 6);
        } else if (node.kind == NodePreset::Kind::ParallelBlock) {
            xml << "      <AudioEffectGroupDevice Id=\"" << devId++ << "\">\n";
            xml << "        <UserName Value=\"Parallel Rack\" />\n";
            xml << "        <Branches>\n";

            auto writeBranchXml = [&](const BranchPreset& branch, int branchId) {
                xml << "          <AudioEffectBranch Id=\"" << branchId
                    << "\" Type=\"" << (branch.slots.empty() ? "Mixer" : (branch.slots[0].type.empty() ? "VST3" : branch.slots[0].type)) << "\""
                    << (branch.slots.empty() || branch.slots[0].path.empty() ? "" : (" Path=\"" + utils::XmlParser::encodeEntities(branch.slots[0].path) + "\""))
                    << " GainDb=\"" << branch.gainDb
                    << "\" Pan=\"" << branch.pan
                    << "\" Muted=\"" << (branch.muted ? "true" : "false")
                    << "\" Soloed=\"" << (branch.solo ? "true" : "false")
                    << "\" PhaseInverted=\"" << (branch.phaseInvert ? "true" : "false")
                    << "\">\n";
                xml << "            <UserName Value=\"" << utils::XmlParser::encodeEntities(branch.name) << "\" />\n";
                xml << "            <BranchMixer>\n";
                xml << "              <GainDb Value=\"" << branch.gainDb << "\" />\n";
                xml << "              <Pan Value=\"" << branch.pan << "\" />\n";
                xml << "              <Muted Value=\"" << (branch.muted ? "true" : "false") << "\" />\n";
                xml << "              <Soloed Value=\"" << (branch.solo ? "true" : "false") << "\" />\n";
                xml << "              <PhaseInverted Value=\"" << (branch.phaseInvert ? "true" : "false") << "\" />\n";
                xml << "            </BranchMixer>\n";
                xml << "            <DeviceChain>\n";
                xml << "              <Devices>\n";
                for (const auto& slot : branch.slots) {
                    writePluginDeviceXml(slot, devId++, 16);
                }
                xml << "              </Devices>\n";
                xml << "            </DeviceChain>\n";
                xml << "          </AudioEffectBranch>\n";
            };

            for (size_t b = 0; b < node.branches.size(); ++b) {
                writeBranchXml(node.branches[b], static_cast<int>(b));
            }

            xml << "        </Branches>\n";
            xml << "      </AudioEffectGroupDevice>\n";
        }
    }

    xml << "    </Devices>\n";
    xml << "  </DeviceChain>\n";
    xml << "</Ableton>\n";

    std::string xmlContent = xml.str();
    std::vector<uint8_t> gzBytes;
    if (!utils::ArchiveUtils::gzipCompress(xmlContent, gzBytes)) {
        return false;
    }

    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(reinterpret_cast<const char*>(gzBytes.data()), static_cast<std::streamsize>(gzBytes.size()));
    return true;
}

} // namespace praccy::state
