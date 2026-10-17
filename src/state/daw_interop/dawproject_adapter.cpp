#include "dawproject_adapter.h"
#include "../../version.h"
#include "../../utils/archive_utils.h"
#include "../../utils/xml_parser.h"
#include "../../utils/guid_utils.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <algorithm>
#include <cctype>

namespace praccy::state {

struct TempDirGuard {
    std::filesystem::path path;
    ~TempDirGuard() {
        if (!path.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    }
};

bool DawprojectAdapter::canHandle(const std::filesystem::path& path) const {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
    return ext == ".dawproject";
}

std::optional<ScenePreset> DawprojectAdapter::parseToScene(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return std::nullopt;

    // Generate safe unique temporary directory
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto tempDir = std::filesystem::temp_directory_path() / ("dawproj_in_" + std::to_string(now));
    TempDirGuard guard{tempDir};

    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    if (ec) return std::nullopt;

    if (!utils::ArchiveUtils::unzipFile(path, tempDir)) {
        return std::nullopt;
    }

    // 1. Invariant 3 / Task 2.2.3: Validate root manifest.xml
    auto manifestPath = tempDir / "manifest.xml";
    if (!std::filesystem::exists(manifestPath)) {
        return std::nullopt;
    }

    auto manifestNode = utils::XmlParser::parseFile(manifestPath);
    if (!manifestNode || manifestNode->tag != "manifest") {
        return std::nullopt;
    }

    std::string formatVer = manifestNode->attribute("formatVersion");
    if (formatVer.empty()) {
        formatVer = manifestNode->attribute("version");
    }
    if (formatVer != "1.0") {
        return std::nullopt; // Strictly adhere to DAWproject 1.0
    }

    // 2. Parse project.xml
    auto projectPath = tempDir / "project.xml";
    if (!std::filesystem::exists(projectPath)) {
        return std::nullopt;
    }

    auto projectNode = utils::XmlParser::parseFile(projectPath);
    if (!projectNode) {
        return std::nullopt;
    }

    ScenePreset scene;
    scene.name = path.stem().string();
    scene.sourceDawName = "DAWproject";

    // Transport info
    auto transportNode = projectNode->firstChild("transport");
    if (transportNode) {
        std::string bpmStr = transportNode->attribute("tempo");
        if (!bpmStr.empty()) {
            scene.tempoBpm = std::strtof(bpmStr.c_str(), nullptr);
        }
        std::string numStr = transportNode->attribute("timeSignatureNumerator");
        if (!numStr.empty()) {
            scene.timeSigNumerator = static_cast<uint8_t>(std::strtoul(numStr.c_str(), nullptr, 10));
        }
        std::string denStr = transportNode->attribute("timeSignatureDenominator");
        if (!denStr.empty()) {
            scene.timeSigDenominator = static_cast<uint8_t>(std::strtoul(denStr.c_str(), nullptr, 10));
        }
    }

    // Find tracks and device chains
    auto tracksNode = projectNode->firstChild("tracks");
    std::vector<std::shared_ptr<utils::XmlNode>> trackList;
    if (tracksNode) {
        trackList = tracksNode->childrenNamed("track");
    } else {
        trackList = projectNode->childrenNamed("track");
    }

    auto parsePluginNode = [&](const std::shared_ptr<utils::XmlNode>& plugNode) -> PluginSlotPreset {
        PluginSlotPreset slot;
        slot.name = plugNode->attribute("name", "Plugin");
        slot.type = "VST3";

        std::string id = plugNode->attribute("id");
        if (!id.empty()) {
            slot.classGuid = utils::GuidUtils::normalizeToCanonicalGuid(id, utils::DawGuidFormat::DawprojectUuid);
        }

        auto fmtNode = plugNode->firstChild("pluginFormat");
        if (fmtNode && !fmtNode->text.empty()) {
            std::string fmt = fmtNode->text;
            std::transform(fmt.begin(), fmt.end(), fmt.begin(), [](unsigned char c){ return std::tolower(c); });
            if (fmt == "clap") slot.type = "CLAP";
            else if (fmt == "vst3") slot.type = "VST3";
        }

        auto idNode = plugNode->firstChild("identifier");
        if (idNode && !idNode->text.empty()) {
            slot.classGuid = utils::GuidUtils::normalizeToCanonicalGuid(idNode->text, utils::DawGuidFormat::DawprojectUuid);
        }

        // State file path: e.g. plugins/<uuid>/state.bin or attribute
        std::string stateRelPath = plugNode->attribute("stateFile");
        if (stateRelPath.empty()) {
            auto sFileNode = plugNode->firstChild("stateFile");
            if (sFileNode && !sFileNode->text.empty()) {
                stateRelPath = sFileNode->text;
            }
        }
        if (stateRelPath.empty() && !id.empty()) {
            stateRelPath = "plugins/" + id + "/state.bin";
        }

        if (!stateRelPath.empty()) {
            std::filesystem::path safeRel;
            if (utils::sanitizeZipPath(stateRelPath, safeRel)) {
                std::error_code ec1, ec2;
                auto canTemp = std::filesystem::weakly_canonical(tempDir, ec1);
                auto canTarget = std::filesystem::weakly_canonical(tempDir / safeRel, ec2);
                if (!ec1 && !ec2) {
                    auto tempIt = canTemp.begin();
                    auto targetIt = canTarget.begin();
                    bool isSubpath = true;
                    for (; tempIt != canTemp.end(); ++tempIt, ++targetIt) {
                        if (targetIt == canTarget.end() || *tempIt != *targetIt) {
                            isSubpath = false;
                            break;
                        }
                    }
                    if (isSubpath && std::filesystem::exists(canTarget)) {
                        std::ifstream sf(canTarget, std::ios::binary);
                        if (sf.is_open()) {
                            slot.state.assign((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
                        }
                    }
                }
            }
        }

        return slot;
    };

    for (const auto& track : trackList) {
        if (!track) continue;

        auto devChain = track->firstChild("deviceChain");
        if (!devChain) devChain = track;

        auto devicesNode = devChain->firstChild("devices");
        if (!devicesNode) devicesNode = devChain;

        for (const auto& child : devicesNode->children) {
            if (!child) continue;
            if (child->tag == "plugin") {
                NodePreset np;
                np.kind = NodePreset::Kind::Plugin;
                np.slot = parsePluginNode(child);
                scene.nodes.push_back(std::move(np));
            } else if (child->tag == "parallelBlock" || child->tag == "split") {
                NodePreset parBlock;
                parBlock.kind = NodePreset::Kind::ParallelBlock;
                parBlock.branchBlend = 0.0f;

                auto branches = child->childrenNamed("branch");
                for (size_t bIdx = 0; bIdx < branches.size() && bIdx < 2; ++bIdx) {
                    BranchPreset bp;
                    bp.name = branches[bIdx]->attribute("name", (bIdx == 0 ? "Branch A" : "Branch B"));
                    auto bDevs = branches[bIdx]->firstChild("devices");
                    if (!bDevs) bDevs = branches[bIdx];
                    for (const auto& bPlug : bDevs->childrenNamed("plugin")) {
                        bp.slots.push_back(parsePluginNode(bPlug));
                    }
                    parBlock.branches.push_back(std::move(bp));
                }
                scene.nodes.push_back(std::move(parBlock));
            }
        }
    }

    if (scene.nodes.empty()) return std::nullopt;
    return scene;
}

bool DawprojectAdapter::exportFromScene(const ScenePreset& scene, const std::filesystem::path& outputPath) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto tempDir = std::filesystem::temp_directory_path() / ("dawproj_out_" + std::to_string(now));
    TempDirGuard guard{tempDir};

    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    if (ec) return false;

    // 1. Root manifest.xml
    {
        std::ofstream mf(tempDir / "manifest.xml", std::ios::trunc);
        if (!mf.is_open()) return false;
        mf << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        mf << "<manifest formatVersion=\"1.0\" application=\"Praccy\" appVersion=\"" << praccy::kDawprojectAppVersion << "\" />\n";
    }

    // 2. project.xml and binary state files
    auto pluginsDir = tempDir / "plugins";
    std::filesystem::create_directories(pluginsDir, ec);

    std::ostringstream px;
    px << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    px << "<project version=\"1.0\">\n";
    px << "  <application name=\"Praccy\" version=\"" << praccy::kDawprojectAppVersion << "\" />\n";
    px << "  <transport tempo=\"" << scene.tempoBpm << "\" timeSignatureNumerator=\""
       << static_cast<int>(scene.timeSigNumerator) << "\" timeSignatureDenominator=\""
       << static_cast<int>(scene.timeSigDenominator) << "\" />\n";
    px << "  <tracks>\n";
    px << "    <track name=\"" << utils::XmlParser::encodeEntities(scene.name) << "\">\n";
    px << "      <deviceChain>\n";
    px << "        <devices>\n";

    int plugIdx = 0;
    auto writePluginXml = [&](const PluginSlotPreset& slot) {
        std::string classUuidStr = utils::GuidUtils::toDawprojectUuidString(slot.classGuid);
        if (classUuidStr.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "00000000-0000-0000-0000-%012d", plugIdx + 1);
            classUuidStr = buf;
        }

        const int currentIdx = ++plugIdx;
        std::string instanceId = "slot_" + std::to_string(currentIdx) + "_" + classUuidStr;
        std::string stateDirRel = "plugins/" + instanceId;

        px << "          <plugin id=\"" << instanceId << "\" name=\""
           << utils::XmlParser::encodeEntities(slot.name) << "\" deviceRole=\"audioEffect\">\n";
        px << "            <pluginFormat>" << (slot.type == "CLAP" ? "clap" : "vst3") << "</pluginFormat>\n";
        px << "            <identifier>" << classUuidStr << "</identifier>\n";

        if (!slot.state.empty()) {
            auto slotPlugDir = tempDir / stateDirRel;
            std::filesystem::create_directories(slotPlugDir, ec);
            auto stateFilePath = slotPlugDir / "state.bin";
            std::ofstream sf(stateFilePath, std::ios::binary | std::ios::trunc);
            if (sf.is_open()) {
                sf.write(reinterpret_cast<const char*>(slot.state.data()), slot.state.size());
            }
            px << "            <stateFile>" << stateDirRel << "/state.bin</stateFile>\n";
        }
        px << "          </plugin>\n";
    };

    for (const auto& node : scene.nodes) {
        if (node.kind == NodePreset::Kind::Plugin) {
            writePluginXml(node.slot);
        } else if (node.kind == NodePreset::Kind::ParallelBlock) {
            px << "          <parallelBlock name=\"Parallel Block\">\n";
            for (size_t bIdx = 0; bIdx < node.branches.size(); ++bIdx) {
                px << "            <branch name=\"" << utils::XmlParser::encodeEntities(node.branches[bIdx].name) << "\">\n";
                px << "              <devices>\n";
                for (const auto& slot : node.branches[bIdx].slots) {
                    writePluginXml(slot);
                }
                px << "              </devices>\n";
                px << "            </branch>\n";
            }
            px << "          </parallelBlock>\n";
        }
    }

    px << "        </devices>\n";
    px << "      </deviceChain>\n";
    px << "    </track>\n";
    px << "  </tracks>\n";
    px << "</project>\n";

    {
        std::ofstream pf(tempDir / "project.xml", std::ios::trunc);
        if (!pf.is_open()) return false;
        pf << px.str();
    }

    return utils::ArchiveUtils::zipDirectory(tempDir, outputPath);
}

} // namespace praccy::state
