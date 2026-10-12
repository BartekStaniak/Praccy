#include "vstpreset_adapter.h"
#include "../../utils/guid_utils.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cctype>

namespace praccy::state {

bool VstPresetAdapter::canHandle(const std::filesystem::path& path) const {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
    return ext == ".vstpreset";
}

bool VstPresetAdapter::parseVstPreset(
    const std::vector<uint8_t>& fileData,
    std::string& outClassGuid,
    std::vector<uint8_t>& outCompState
) {
    outClassGuid.clear();
    outCompState.clear();

    if (fileData.size() < 48) {
        return false;
    }

    // Check magic: 'VST3'
    if (fileData[0] != 'V' || fileData[1] != 'S' || fileData[2] != 'T' || fileData[3] != '3') {
        return false;
    }

    // Read 32-byte class ID (at offset 8)
    std::string guidStr(reinterpret_cast<const char*>(fileData.data() + 8), 32);
    outClassGuid = utils::GuidUtils::normalizeToCanonicalGuid(guidStr, utils::DawGuidFormat::SteinbergFUID);

    // Look for 'Comp' tag starting from offset 40
    size_t compPos = 40;
    while (compPos + 12 <= fileData.size()) {
        if (fileData[compPos] == 'C' && fileData[compPos + 1] == 'o' &&
            fileData[compPos + 2] == 'm' && fileData[compPos + 3] == 'p') {
            uint64_t compSize = 0;
            std::memcpy(&compSize, fileData.data() + compPos + 4, 8);
            if (compPos + 12 + compSize <= fileData.size()) {
                outCompState.assign(
                    fileData.begin() + compPos + 12,
                    fileData.begin() + compPos + 12 + compSize
                );
                return true;
            }
        }
        compPos++;
    }

    // If 'Comp' tag wasn't found, read remainder of file past header as state
    if (fileData.size() > 40) {
        outCompState.assign(fileData.begin() + 40, fileData.end());
        return true;
    }

    return false;
}

std::vector<uint8_t> VstPresetAdapter::packVstPreset(
    const std::string& classGuid,
    const std::vector<uint8_t>& compState
) {
    std::vector<uint8_t> out;
    const uint64_t compSize = compState.size();
    out.resize(40 + 12 + compSize, 0);

    // Header: 'VST3'
    out[0] = 'V'; out[1] = 'S'; out[2] = 'T'; out[3] = '3';
    // Version = 1
    const uint32_t version = 1;
    std::memcpy(out.data() + 4, &version, 4);

    // Class ID: 32 bytes ASCII
    std::string cleanGuid = utils::GuidUtils::normalizeToCanonicalGuid(classGuid, utils::DawGuidFormat::SteinbergFUID);
    for (char& c : cleanGuid) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    size_t copyLen = std::min(cleanGuid.size(), size_t(32));
    std::memcpy(out.data() + 8, cleanGuid.data(), copyLen);

    // 'Comp' chunk
    out[40] = 'C'; out[41] = 'o'; out[42] = 'm'; out[43] = 'p';
    std::memcpy(out.data() + 44, &compSize, 8);

    if (compSize > 0) {
        std::memcpy(out.data() + 52, compState.data(), compSize);
    }

    return out;
}

std::optional<ScenePreset> VstPresetAdapter::parseToScene(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return std::nullopt;

    std::vector<uint8_t> fileBytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string classGuid;
    std::vector<uint8_t> compState;

    if (!parseVstPreset(fileBytes, classGuid, compState)) {
        return std::nullopt;
    }

    ScenePreset scene;
    scene.name = path.stem().string();
    scene.sourceDawName = "Steinberg";

    NodePreset node;
    node.kind = NodePreset::Kind::Plugin;
    node.slot.name = path.stem().string();
    node.slot.type = "VST3";
    node.slot.classGuid = classGuid;
    node.slot.state = std::move(compState);

    scene.nodes.push_back(std::move(node));
    return scene;
}

bool VstPresetAdapter::exportFromScene(const ScenePreset& scene, const std::filesystem::path& outputPath) {
    if (scene.nodes.empty()) return false;

    const PluginSlotPreset* slotToExport = nullptr;
    for (const auto& node : scene.nodes) {
        if (node.kind == NodePreset::Kind::Plugin) {
            slotToExport = &node.slot;
            break;
        } else if (node.kind == NodePreset::Kind::ParallelBlock && !node.branches.empty()) {
            if (!node.branches[0].slots.empty()) {
                slotToExport = &node.branches[0].slots[0];
                break;
            }
        }
    }

    if (!slotToExport) return false;

    std::vector<uint8_t> presetBytes = packVstPreset(slotToExport->classGuid, slotToExport->state);
    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    out.write(reinterpret_cast<const char*>(presetBytes.data()), presetBytes.size());
    return true;
}

} // namespace praccy::state
