#include "reaper_adapter.h"
#include "../../utils/base64_utils.h"
#include "../../utils/guid_utils.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace praccy::state {

bool ReaperAdapter::canHandle(const std::filesystem::path& path) const {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
    return ext == ".rfxchain";
}

bool ReaperAdapter::parsePinmap(std::string_view line, uint64_t& inMask, uint64_t& outMask) {
    // Expected format: PINMAP <in_mask> <out_mask>
    size_t pos = line.find("PINMAP");
    if (pos == std::string_view::npos) return false;
    pos += 6;
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) pos++;
    if (pos >= line.size()) return false;

    char* endA = nullptr;
    const char* strA = line.data() + pos;
    inMask = std::strtoull(strA, &endA, 10);
    if (!endA || endA == strA) return false;

    while (*endA && std::isspace(static_cast<unsigned char>(*endA))) endA++;
    if (!*endA) {
        outMask = inMask;
        return true;
    }

    char* endB = nullptr;
    outMask = std::strtoull(endA, &endB, 10);
    return (endB && endB != endA);
}

bool ReaperAdapter::unpackVst3ChunkEnvelope(
    const std::vector<uint8_t>& rawChunk,
    std::vector<uint8_t>& outCompState,
    std::vector<uint8_t>& outCtrlState
) {
    outCompState.clear();
    outCtrlState.clear();

    if (rawChunk.size() < 8) {
        // Not a wrapped VST3 envelope; treat entire rawChunk as component state
        outCompState = rawChunk;
        return true;
    }

    // Check for 'vst3' magic (0x76, 0x73, 0x74, 0x33)
    if (rawChunk[0] == 'v' && rawChunk[1] == 's' && rawChunk[2] == 't' && rawChunk[3] == '3') {
        uint32_t compLen = 0;
        std::memcpy(&compLen, rawChunk.data() + 4, 4);

        if (rawChunk.size() >= 8 && static_cast<size_t>(compLen) <= rawChunk.size() - 8) {
            outCompState.assign(rawChunk.begin() + 8, rawChunk.begin() + 8 + compLen);

            size_t ctrlOffset = 8 + compLen;
            if (rawChunk.size() >= ctrlOffset && rawChunk.size() - ctrlOffset >= 4) {
                uint32_t ctrlLen = 0;
                std::memcpy(&ctrlLen, rawChunk.data() + ctrlOffset, 4);
                if (rawChunk.size() - ctrlOffset - 4 >= static_cast<size_t>(ctrlLen)) {
                    outCtrlState.assign(rawChunk.begin() + ctrlOffset + 4, rawChunk.begin() + ctrlOffset + 4 + ctrlLen);
                }
            }
            return true;
        }
    }

    // Fallback: raw chunk without envelope
    outCompState = rawChunk;
    return true;
}

std::vector<uint8_t> ReaperAdapter::packVst3ChunkEnvelope(
    const std::vector<uint8_t>& compState,
    const std::vector<uint8_t>& ctrlState
) {
    std::vector<uint8_t> out;
    const uint32_t compLen = static_cast<uint32_t>(compState.size());
    const uint32_t ctrlLen = static_cast<uint32_t>(ctrlState.size());

    out.resize(8 + compLen + 4 + ctrlLen);
    out[0] = 'v'; out[1] = 's'; out[2] = 't'; out[3] = '3';
    std::memcpy(out.data() + 4, &compLen, 4);
    if (compLen > 0) {
        std::memcpy(out.data() + 8, compState.data(), compLen);
    }

    size_t ctrlOffset = 8 + compLen;
    std::memcpy(out.data() + ctrlOffset, &ctrlLen, 4);
    if (ctrlLen > 0) {
        std::memcpy(out.data() + ctrlOffset + 4, ctrlState.data(), ctrlLen);
    }

    return out;
}

std::optional<ScenePreset> ReaperAdapter::parseToScene(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;

    ScenePreset scene;
    scene.name = path.stem().string();
    scene.sourceDawName = "Reaper";
    scene.tempoBpm = 120.0f;
    scene.timeSigNumerator = 4;
    scene.timeSigDenominator = 4;

    struct ParsedSlot {
        PluginSlotPreset slot;
        bool hasPinmap{false};
        uint64_t inPins{0};
        uint64_t outPins{0};
        bool isBranchB{false};
    };

    std::vector<ParsedSlot> parsedSlots;
    std::string line;
    std::optional<std::string> pendingLine;

    auto getNextLine = [&]() -> bool {
        if (pendingLine.has_value()) {
            line = std::move(*pendingLine);
            pendingLine.reset();
            return true;
        }
        return static_cast<bool>(std::getline(file, line));
    };

    while (getNextLine()) {
        // Trim leading whitespace
        size_t start = 0;
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) start++;
        if (start >= line.size()) continue;
        std::string_view trimmed = std::string_view(line).substr(start);

        if (trimmed.starts_with("<REAPER_FXCHAIN")) {
            continue;
        }

        if (trimmed.starts_with("<VST") || trimmed.starts_with("<CLAP")) {
            ParsedSlot ps;
            ps.slot.type = trimmed.starts_with("<VST") ? "VST3" : "CLAP";

            // Extract plugin name and bundle name: <VST "name" "bundle" ...
            size_t q1 = trimmed.find('"');
            if (q1 != std::string_view::npos) {
                size_t q2 = trimmed.find('"', q1 + 1);
                if (q2 != std::string_view::npos) {
                    std::string rawName = std::string(trimmed.substr(q1 + 1, q2 - (q1 + 1)));
                    // Strip "VST3: " or "CLAP: " prefix if present
                    if (rawName.starts_with("VST3: ")) rawName = rawName.substr(6);
                    else if (rawName.starts_with("CLAP: ")) rawName = rawName.substr(6);
                    ps.slot.name = rawName;

                    size_t q3 = trimmed.find('"', q2 + 1);
                    if (q3 != std::string_view::npos) {
                        size_t q4 = trimmed.find('"', q3 + 1);
                        if (q4 != std::string_view::npos) {
                            ps.slot.bundleFileName = std::string(trimmed.substr(q3 + 1, q4 - (q3 + 1)));
                        }
                    }
                }
            }

            // Read base64 chunk lines until line with '>'
            std::string b64Accum;
            while (getNextLine()) {
                size_t bStart = 0;
                while (bStart < line.size() && std::isspace(static_cast<unsigned char>(line[bStart]))) bStart++;
                if (bStart >= line.size()) continue;

                if (line[bStart] == '>') {
                    break;
                }
                b64Accum += line.substr(bStart);
            }

            if (!b64Accum.empty()) {
                std::vector<uint8_t> rawBytes;
                if (utils::Base64::decode(b64Accum, rawBytes)) {
                    std::vector<uint8_t> comp, ctrl;
                    unpackVst3ChunkEnvelope(rawBytes, comp, ctrl);
                    ps.slot.state = std::move(comp);
                }
            }

            // Look forward for FXID, PINMAP or other properties
            while (getNextLine()) {
                size_t pStart = 0;
                while (pStart < line.size() && std::isspace(static_cast<unsigned char>(line[pStart]))) pStart++;
                if (pStart >= line.size()) continue;
                std::string_view pTrimmed = std::string_view(line).substr(pStart);

                if (pTrimmed.starts_with("FXID")) {
                    size_t fxPos = pTrimmed.find("FXID") + 4;
                    while (fxPos < pTrimmed.size() && std::isspace(static_cast<unsigned char>(pTrimmed[fxPos]))) fxPos++;
                    if (fxPos < pTrimmed.size()) {
                        ps.slot.classGuid = utils::GuidUtils::normalizeToCanonicalGuid(pTrimmed.substr(fxPos), utils::DawGuidFormat::SteinbergFUID);
                    }
                } else if (pTrimmed.starts_with("PINMAP")) {
                    ps.hasPinmap = true;
                    parsePinmap(pTrimmed, ps.inPins, ps.outPins);
                    if ((ps.inPins & 12) != 0 || (ps.outPins & 12) != 0) { // 4|8
                        ps.isBranchB = true;
                    }
                } else if (pTrimmed.starts_with("<") || pTrimmed == ">") {
                    // Start of next plugin or end of chain: buffer line and finish slot
                    pendingLine = line;
                    break;
                }
            }

            parsedSlots.push_back(std::move(ps));
        }
    }

    if (parsedSlots.empty()) {
        return std::nullopt;
    }

    // Partition into Serial slots vs Parallel branches
    size_t idx = 0;
    while (idx < parsedSlots.size()) {
        if (!parsedSlots[idx].hasPinmap) {
            // Serial slot
            NodePreset np;
            np.kind = NodePreset::Kind::Plugin;
            np.slot = std::move(parsedSlots[idx].slot);
            scene.nodes.push_back(std::move(np));
            idx++;
        } else {
            // Consecutive parallel slots
            NodePreset parBlock;
            parBlock.kind = NodePreset::Kind::ParallelBlock;
            parBlock.branchBlend = 0.0f;

            BranchPreset branchA;
            branchA.name = "Branch A";
            BranchPreset branchB;
            branchB.name = "Branch B";

            while (idx < parsedSlots.size() && parsedSlots[idx].hasPinmap) {
                if (parsedSlots[idx].isBranchB) {
                    branchB.slots.push_back(std::move(parsedSlots[idx].slot));
                } else {
                    branchA.slots.push_back(std::move(parsedSlots[idx].slot));
                }
                idx++;
            }

            parBlock.branches.push_back(std::move(branchA));
            parBlock.branches.push_back(std::move(branchB));
            scene.nodes.push_back(std::move(parBlock));
        }
    }

    return scene;
}

bool ReaperAdapter::exportFromScene(const ScenePreset& scene, const std::filesystem::path& outputPath) {
    std::ofstream out(outputPath, std::ios::trunc);
    if (!out.is_open()) return false;

    out << "<REAPER_FXCHAIN\n";
    out << "  WNDRECT 0 0 0 0\n";
    out << "  SHOW 0\n";
    out << "  LASTSEL 0\n";
    out << "  DOCKED 0\n";
    out << "  BYPASS 0 0 0\n";

    auto writeSlot = [&](const PluginSlotPreset& slot, bool hasPinmap, uint64_t inPins, uint64_t outPins) {
        out << "  <VST \"VST3: " << slot.name << "\" \""
            << (slot.bundleFileName.empty() ? (slot.name + ".vst3") : slot.bundleFileName)
            << "\" 0 \"\" 0\n";

        // Pack envelope & base64
        std::vector<uint8_t> emptyCtrl;
        std::vector<uint8_t> env = packVst3ChunkEnvelope(slot.state, emptyCtrl);
        std::string b64 = utils::Base64::encode(env);

        // Break base64 into 76-char lines
        for (size_t pos = 0; pos < b64.size(); pos += 76) {
            out << "    " << b64.substr(pos, 76) << "\n";
        }
        out << "  >\n";
        out << "  FLOATPOS 0 0 0 0\n";
        out << "  FXID " << utils::GuidUtils::toSteinbergGuidString(slot.classGuid) << "\n";
        if (hasPinmap) {
            out << "  PINMAP " << inPins << " " << outPins << "\n";
        }
        out << "  WAK 0 0\n";
    };

    for (const auto& node : scene.nodes) {
        if (node.kind == NodePreset::Kind::Plugin) {
            writeSlot(node.slot, false, 0, 0); // Default/unmapped -> Serial slot
        } else if (node.kind == NodePreset::Kind::ParallelBlock) {
            // Branch A
            if (!node.branches.empty()) {
                for (const auto& slot : node.branches[0].slots) {
                    writeSlot(slot, true, 3, 3); // 1|2 pins -> Branch A
                }
            }
            // Branch B
            if (node.branches.size() > 1) {
                for (const auto& slot : node.branches[1].slots) {
                    writeSlot(slot, true, 12, 12); // 4|8 pins -> Branch B
                }
            }
        }
    }

    out << ">\n";
    return true;
}

} // namespace praccy::state
