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
    slot.type = "VST3";

    // 1. Name & Guid
    auto desc = node->firstChild("PluginDesc");
    if (desc) {
        auto nameNode = desc->firstChild("Name");
        if (nameNode) {
            slot.name = nameNode->attribute("Value");
        }

        auto vst3Info = desc->firstChild("Vst3PluginInfo");
        if (vst3Info) {
            auto plugId = vst3Info->firstChild("PlugId");
            if (plugId) {
                std::string rawId = plugId->attribute("Value");
                slot.classGuid = utils::GuidUtils::normalizeToCanonicalGuid(rawId, utils::DawGuidFormat::AbletonBigEndian);
            }
        }
    }

    if (slot.name.empty()) {
        auto userName = node->firstChild("UserName");
        if (userName) {
            slot.name = userName->attribute("Value");
        }
    }

    if (slot.name.empty()) {
        slot.name = "VST3 Plugin";
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
                NodePreset np;
                np.kind = NodePreset::Kind::Plugin;
                np.slot = std::move(slot);
                scene.nodes.push_back(std::move(np));
            } else if (child->tag == "AudioEffectGroupDevice") {
                auto branchesNode = child->firstChild("Branches");
                if (branchesNode && !branchesNode->children.empty()) {
                    std::vector<std::vector<PluginSlotPreset>> branchSlotsList;
                    for (const auto& branch : branchesNode->children) {
                        if (!branch) continue;
                        std::vector<PluginSlotPreset> bSlots;
                        auto bDevChain = branch->firstChild("DeviceChain");
                        if (bDevChain) {
                            auto bDevices = bDevChain->firstChild("Devices");
                            if (bDevices) {
                                extractDevicesRecursive(bDevices, bSlots);
                            }
                        }
                        branchSlotsList.push_back(std::move(bSlots));
                    }

                    if (branchSlotsList.size() == 1) {
                        for (auto& s : branchSlotsList[0]) {
                            NodePreset np;
                            np.kind = NodePreset::Kind::Plugin;
                            np.slot = std::move(s);
                            scene.nodes.push_back(std::move(np));
                        }
                    } else if (branchSlotsList.size() >= 2) {
                        NodePreset parBlock;
                        parBlock.kind = NodePreset::Kind::ParallelBlock;
                        parBlock.branchBlend = 0.5f;

                        BranchPreset bA;
                        bA.name = "Branch A";
                        bA.slots = std::move(branchSlotsList[0]);

                        BranchPreset bB;
                        bB.name = "Branch B";
                        bB.slots = std::move(branchSlotsList[1]);

                        for (size_t bIdx = 2; bIdx < branchSlotsList.size(); ++bIdx) {
                            for (auto& extraSlot : branchSlotsList[bIdx]) {
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
                std::vector<std::vector<PluginSlotPreset>> branchSlotsList;
                for (const auto& branch : branchesNode->children) {
                    if (!branch) continue;
                    std::vector<PluginSlotPreset> bSlots;
                    auto bDevChain = branch->firstChild("DeviceChain");
                    if (bDevChain) {
                        auto bDevices = bDevChain->firstChild("Devices");
                        if (bDevices) {
                            extractDevicesRecursive(bDevices, bSlots);
                        }
                    }
                    branchSlotsList.push_back(std::move(bSlots));
                }

                if (branchSlotsList.size() == 1) {
                    for (auto& s : branchSlotsList[0]) {
                        NodePreset np;
                        np.kind = NodePreset::Kind::Plugin;
                        np.slot = std::move(s);
                        scene.nodes.push_back(std::move(np));
                    }
                } else if (branchSlotsList.size() >= 2) {
                    NodePreset parBlock;
                    parBlock.kind = NodePreset::Kind::ParallelBlock;
                    parBlock.branchBlend = 0.5f;

                    BranchPreset bA;
                    bA.name = "Branch A";
                    bA.slots = std::move(branchSlotsList[0]);

                    BranchPreset bB;
                    bB.name = "Branch B";
                    bB.slots = std::move(branchSlotsList[1]);

                    for (size_t bIdx = 2; bIdx < branchSlotsList.size(); ++bIdx) {
                        for (auto& extraSlot : branchSlotsList[bIdx]) {
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
        xml << pad << "<PluginDevice Id=\"" << id << "\">\n";
        xml << pad << "  <PluginDesc>\n";
        xml << pad << "    <Vst3PluginInfo>\n";
        xml << pad << "      <PlugId Value=\"" << utils::GuidUtils::toAbletonGuidString(slot.classGuid) << "\" />\n";
        xml << pad << "    </Vst3PluginInfo>\n";
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

            // Branch A
            if (!node.branches.empty()) {
                xml << "          <AudioEffectBranch Id=\"0\">\n";
                xml << "            <UserName Value=\"Branch A\" />\n";
                xml << "            <DeviceChain>\n";
                xml << "              <Devices>\n";
                for (const auto& slot : node.branches[0].slots) {
                    writePluginDeviceXml(slot, devId++, 16);
                }
                xml << "              </Devices>\n";
                xml << "            </DeviceChain>\n";
                xml << "          </AudioEffectBranch>\n";
            }

            // Branch B
            if (node.branches.size() > 1) {
                xml << "          <AudioEffectBranch Id=\"1\">\n";
                xml << "            <UserName Value=\"Branch B\" />\n";
                xml << "            <DeviceChain>\n";
                xml << "              <Devices>\n";
                for (const auto& slot : node.branches[1].slots) {
                    writePluginDeviceXml(slot, devId++, 16);
                }
                xml << "              </Devices>\n";
                xml << "            </DeviceChain>\n";
                xml << "          </AudioEffectBranch>\n";
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
    out.write(reinterpret_cast<const char*>(gzBytes.data()), gzBytes.size());
    return true;
}

} // namespace praccy::state
