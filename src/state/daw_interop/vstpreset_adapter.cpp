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

    // 1. Official Steinberg layout: Chunk list at chunkListOffset (offset 40)
    uint64_t chunkListOffset = 0;
    std::memcpy(&chunkListOffset, fileData.data() + 40, 8);
    if (chunkListOffset <= fileData.size() && (fileData.size() - chunkListOffset) >= 8) {
        const size_t listPos = static_cast<size_t>(chunkListOffset);
        if (fileData[listPos] == 'L' && fileData[listPos + 1] == 'i' &&
            fileData[listPos + 2] == 's' && fileData[listPos + 3] == 't') {
            uint32_t entryCount = 0;
            std::memcpy(&entryCount, fileData.data() + listPos + 4, 4);
            size_t entryPos = listPos + 8;
            for (uint32_t i = 0; i < entryCount; ++i) {
                if (entryPos + 20 > fileData.size()) {
                    break;
                }
                if (fileData[entryPos] == 'C' && fileData[entryPos + 1] == 'o' &&
                    fileData[entryPos + 2] == 'm' && fileData[entryPos + 3] == 'p') {
                    uint64_t chunkOffset = 0;
                    uint64_t chunkSize = 0;
                    std::memcpy(&chunkOffset, fileData.data() + entryPos + 4, 8);
                    std::memcpy(&chunkSize, fileData.data() + entryPos + 12, 8);
                    if (chunkOffset <= fileData.size() && chunkSize <= fileData.size() - chunkOffset) {
                        outCompState.assign(
                            fileData.begin() + chunkOffset,
                            fileData.begin() + chunkOffset + chunkSize
                        );
                        return true;
                    }
                }
                entryPos += 20;
            }
        }
    }

    // 2. Backward-compatible fallback: Look for 'Comp' tag starting from offset 40
    size_t compPos = 40;
    while (compPos + 12 <= fileData.size()) {
        if (fileData[compPos] == 'C' && fileData[compPos + 1] == 'o' &&
            fileData[compPos + 2] == 'm' && fileData[compPos + 3] == 'p') {
            uint64_t compSize = 0;
            std::memcpy(&compSize, fileData.data() + compPos + 4, 8);
            if (compPos + 12 <= fileData.size() && compSize <= fileData.size() - (compPos + 12)) {
                outCompState.assign(
                    fileData.begin() + compPos + 12,
                    fileData.begin() + compPos + 12 + compSize
                );
                return true;
            }
        }
        compPos++;
    }

    // 3. Backward-compatible fallback: read remainder past header as state
    if (fileData.size() > 48) {
        outCompState.assign(fileData.begin() + 48, fileData.end());
        return true;
    } else if (fileData.size() > 40) {
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
    // Official Steinberg .vstpreset container layout:
    // Header (48 bytes):
    //   0..3: 'VST3'
    //   4..7: version = 1
    //   8..39: class ID (32 bytes ASCII)
    //   40..47: chunkListOffset (uint64_t) = 48 + compSize
    // Component data (compSize bytes):
    //   48..(48 + compSize - 1)
    // Chunk List (28 bytes):
    //   (48 + compSize)..(48 + compSize + 3): 'List'
    //   (48 + compSize + 4)..(48 + compSize + 7): entryCount = 1
    //   Entry:
    //     id: 'Comp' (4 bytes)
    //     offset: 48 (8 bytes)
    //     size: compSize (8 bytes)
    const uint64_t chunkListOffset = 48 + compSize;
    const size_t totalSize = static_cast<size_t>(chunkListOffset + 4 + 4 + 4 + 8 + 8);
    out.resize(totalSize, 0);

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

    // chunkListOffset at offset 40
    std::memcpy(out.data() + 40, &chunkListOffset, 8);

    // Component data at offset 48
    if (compSize > 0) {
        std::memcpy(out.data() + 48, compState.data(), compSize);
    }

    // Chunk List at chunkListOffset
    size_t listPos = static_cast<size_t>(chunkListOffset);
    out[listPos]     = 'L';
    out[listPos + 1] = 'i';
    out[listPos + 2] = 's';
    out[listPos + 3] = 't';
    const uint32_t entryCount = 1;
    std::memcpy(out.data() + listPos + 4, &entryCount, 4);

    // Entry 0: 'Comp'
    size_t entryPos = listPos + 8;
    out[entryPos]     = 'C';
    out[entryPos + 1] = 'o';
    out[entryPos + 2] = 'm';
    out[entryPos + 3] = 'p';
    const uint64_t compDataOffset = 48;
    std::memcpy(out.data() + entryPos + 4, &compDataOffset, 8);
    std::memcpy(out.data() + entryPos + 12, &compSize, 8);

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
