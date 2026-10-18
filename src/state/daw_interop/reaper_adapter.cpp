#include "reaper_adapter.h"
#include "../../utils/base64_utils.h"
#include "../../utils/guid_utils.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <charconv>

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

    const char* first = line.data() + pos;
    const char* last = line.data() + line.size();

    auto resA = std::from_chars(first, last, inMask, 10);
    if (resA.ec != std::errc{}) return false;

    first = resA.ptr;
    while (first < last && std::isspace(static_cast<unsigned char>(*first))) first++;
    if (first >= last) {
        outMask = inMask;
        return true;
    }

    auto resB = std::from_chars(first, last, outMask, 10);
    return resB.ec == std::errc{};
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
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    if (ec || sz == 0 || sz > kMaxPresetFileSize) return std::nullopt;

    std::ifstream file(path);
    if (!file.is_open()) return std::nullopt;

    ScenePreset scene;
    scene.name = path.stem().string();
    scene.sourceDawName = "Reaper";
    scene.tempoBpm = 120.0f;
    scene.timeSigNumerator = 4;
    scene.timeSigDenominator = 4;

    struct BranchMeta {
        std::string name;
        float gainDb{0.0f};
        float pan{0.0f};
        bool muted{false};
        bool solo{false};
        bool phaseInvert{false};
        bool hasData{false};
    };

    struct ParsedSlot {
        PluginSlotPreset slot;
        bool hasPinmap{false};
        uint64_t inPins{0};
        uint64_t outPins{0};
        bool isBranchB{false};
        BranchMeta branchMeta;
    };

    auto parseBranchMeta = [](std::string_view metaLine, BranchMeta& meta) {
        meta = BranchMeta{};
        meta.hasData = true;
        auto getVal = [&](std::string_view key) -> std::string {
            size_t pos = metaLine.find(key);
            if (pos == std::string_view::npos) return "";
            pos += key.size();
            if (pos >= metaLine.size()) return "";
            if (metaLine[pos] == '"') {
                size_t endQ = metaLine.find('"', pos + 1);
                if (endQ != std::string_view::npos) {
                    return std::string(metaLine.substr(pos + 1, endQ - (pos + 1)));
                }
            }
            size_t endW = pos;
            while (endW < metaLine.size() && !std::isspace(static_cast<unsigned char>(metaLine[endW]))) endW++;
            return std::string(metaLine.substr(pos, endW - pos));
        };
        std::string n = getVal("name=");
        if (!n.empty()) meta.name = n;
        std::string g = getVal("gainDb=");
        if (!g.empty()) {
            const float v = std::strtof(g.c_str(), nullptr);
            if (std::isfinite(v)) meta.gainDb = std::clamp(v, -60.0f, 24.0f);
        }
        std::string p = getVal("pan=");
        if (!p.empty()) {
            const float v = std::strtof(p.c_str(), nullptr);
            if (std::isfinite(v)) meta.pan = std::clamp(v, -1.0f, 1.0f);
        }
        std::string m = getVal("muted=");
        if (!m.empty()) meta.muted = (m == "1" || m == "true");
        std::string s = getVal("solo=");
        if (!s.empty()) meta.solo = (s == "1" || s == "true");
        std::string pi = getVal("phaseInvert=");
        if (!pi.empty()) meta.phaseInvert = (pi == "1" || pi == "true");
    };

    BranchMeta currentBranchAMeta;
    BranchMeta currentBranchBMeta;
    std::string pendingSlotType;

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

        if (trimmed.starts_with("; PRACCY_BRANCH_A") || trimmed.starts_with("// PRACCY_BRANCH_A")) {
            parseBranchMeta(trimmed, currentBranchAMeta);
            continue;
        }

        if (trimmed.starts_with("; PRACCY_BRANCH_B") || trimmed.starts_with("// PRACCY_BRANCH_B")) {
            parseBranchMeta(trimmed, currentBranchBMeta);
            continue;
        }

        if (trimmed.starts_with("; PRACCY_TYPE") || trimmed.starts_with("// PRACCY_TYPE")) {
            size_t sepPos = trimmed.find_first_of("=:\t ");
            if (sepPos != std::string_view::npos) {
                size_t valStart = trimmed.find_first_not_of("=:\t ", sepPos);
                if (valStart != std::string_view::npos) {
                    pendingSlotType = std::string(trimmed.substr(valStart));
                    while (!pendingSlotType.empty() && std::isspace(static_cast<unsigned char>(pendingSlotType.back()))) {
                        pendingSlotType.pop_back();
                    }
                }
            }
            continue;
        }

        if (trimmed.starts_with("<VST") || trimmed.starts_with("<CLAP")) {
            ParsedSlot ps;
            if (!pendingSlotType.empty()) {
                ps.slot.type = pendingSlotType;
                pendingSlotType.clear();
            } else {
                ps.slot.type = trimmed.starts_with("<VST") ? "VST3" : "CLAP";
            }

            // Extract plugin name and bundle name: <VST "name" "bundle" ...
            size_t q1 = trimmed.find('"');
            if (q1 != std::string_view::npos) {
                size_t q2 = trimmed.find('"', q1 + 1);
                if (q2 != std::string_view::npos) {
                    std::string rawName = std::string(trimmed.substr(q1 + 1, q2 - (q1 + 1)));
                    // Strip "VST3: ", "CLAP: ", or "BuiltIn: " prefix if present
                    if (rawName.starts_with("VST3: ")) {
                        rawName = rawName.substr(6);
                    } else if (rawName.starts_with("CLAP: ")) {
                        rawName = rawName.substr(6);
                        ps.slot.type = "CLAP";
                    } else if (rawName.starts_with("BuiltIn: ")) {
                        rawName = rawName.substr(9);
                        ps.slot.type = "BuiltIn";
                    }
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
                } else if (pTrimmed.starts_with("PRACCY_TYPE")) {
                    size_t tPos = 11;
                    while (tPos < pTrimmed.size() && (std::isspace(static_cast<unsigned char>(pTrimmed[tPos])) || pTrimmed[tPos] == '=')) tPos++;
                    if (tPos < pTrimmed.size()) {
                        std::string t = std::string(pTrimmed.substr(tPos));
                        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
                        if (!t.empty()) ps.slot.type = t;
                    }
                } else if (pTrimmed.starts_with("PRACCY_PATH")) {
                    size_t pPos = 11;
                    while (pPos < pTrimmed.size() && (std::isspace(static_cast<unsigned char>(pTrimmed[pPos])) || pTrimmed[pPos] == '=')) pPos++;
                    if (pPos < pTrimmed.size()) {
                        std::string p = std::string(pTrimmed.substr(pPos));
                        while (!p.empty() && std::isspace(static_cast<unsigned char>(p.back()))) p.pop_back();
                        if (!p.empty()) ps.slot.path = p;
                    }
                } else if (pTrimmed.starts_with("PRACCY_CLAP_ID")) {
                    size_t cPos = 14;
                    while (cPos < pTrimmed.size() && (std::isspace(static_cast<unsigned char>(pTrimmed[cPos])) || pTrimmed[cPos] == '=')) cPos++;
                    if (cPos < pTrimmed.size()) {
                        std::string cid = std::string(pTrimmed.substr(cPos));
                        while (!cid.empty() && std::isspace(static_cast<unsigned char>(cid.back()))) cid.pop_back();
                        if (!cid.empty()) ps.slot.clapId = cid;
                    }
                } else if (pTrimmed.starts_with("<") || pTrimmed == ">" ||
                           pTrimmed.starts_with(";") || pTrimmed.starts_with("//")) {
                    // Start of next plugin, comment/metadata, or end of chain: buffer line and finish slot
                    pendingLine = line;
                    break;
                }
            }

            if (ps.hasPinmap) {
                if (ps.isBranchB) {
                    ps.branchMeta = currentBranchBMeta;
                } else {
                    ps.branchMeta = currentBranchAMeta;
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
                    if (parsedSlots[idx].branchMeta.hasData) {
                        if (!parsedSlots[idx].branchMeta.name.empty()) branchB.name = parsedSlots[idx].branchMeta.name;
                        branchB.gainDb = parsedSlots[idx].branchMeta.gainDb;
                        branchB.pan = parsedSlots[idx].branchMeta.pan;
                        branchB.muted = parsedSlots[idx].branchMeta.muted;
                        branchB.solo = parsedSlots[idx].branchMeta.solo;
                        branchB.phaseInvert = parsedSlots[idx].branchMeta.phaseInvert;
                    }
                    branchB.slots.push_back(std::move(parsedSlots[idx].slot));
                } else {
                    if (parsedSlots[idx].branchMeta.hasData) {
                        if (!parsedSlots[idx].branchMeta.name.empty()) branchA.name = parsedSlots[idx].branchMeta.name;
                        branchA.gainDb = parsedSlots[idx].branchMeta.gainDb;
                        branchA.pan = parsedSlots[idx].branchMeta.pan;
                        branchA.muted = parsedSlots[idx].branchMeta.muted;
                        branchA.solo = parsedSlots[idx].branchMeta.solo;
                        branchA.phaseInvert = parsedSlots[idx].branchMeta.phaseInvert;
                    }
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

    auto sanitize = [](std::string_view input) -> std::string {
        std::string s;
        s.reserve(input.size());
        for (char c : input) {
            if (c == '"' || c == '\r' || c == '\n') {
                s.push_back(' ');
            } else {
                s.push_back(c);
            }
        }
        return s;
    };

    auto writeSlot = [&](const PluginSlotPreset& slot, bool hasPinmap, uint64_t inPins, uint64_t outPins) {
        std::string plugType = slot.type;
        if (plugType.empty()) {
            plugType = "VST3";
        }

        std::string displayName = sanitize(slot.name);
        if (displayName.starts_with("VST3: ")) displayName = displayName.substr(6);
        else if (displayName.starts_with("CLAP: ")) displayName = displayName.substr(6);
        else if (displayName.starts_with("BuiltIn: ")) displayName = displayName.substr(9);

        std::string bundleFile = sanitize(slot.bundleFileName);
        std::string cleanPath = sanitize(slot.path);
        std::string cleanClapId = sanitize(slot.clapId);

        if (plugType == "CLAP") {
            out << "  <CLAP \"CLAP: " << displayName << "\" \""
                << (bundleFile.empty() ? (displayName + ".clap") : bundleFile)
                << "\" 0 \"\" 0\n";
        } else if (plugType == "BuiltIn") {
            out << "  ; PRACCY_TYPE=BuiltIn\n";
            out << "  <VST \"BuiltIn: " << displayName << "\" \""
                << (bundleFile.empty() ? (displayName + ".vst3") : bundleFile)
                << "\" 0 \"\" 0\n";
        } else {
            out << "  <VST \"VST3: " << displayName << "\" \""
                << (bundleFile.empty() ? (displayName + ".vst3") : bundleFile)
                << "\" 0 \"\" 0\n";
        }

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
        out << "  PRACCY_TYPE " << plugType << "\n";
        if (!cleanPath.empty()) {
            out << "  PRACCY_PATH " << cleanPath << "\n";
        }
        if (!cleanClapId.empty()) {
            out << "  PRACCY_CLAP_ID " << cleanClapId << "\n";
        }
        out << "  WAK 0 0\n";
    };

    for (const auto& node : scene.nodes) {
        if (node.kind == NodePreset::Kind::Plugin) {
            writeSlot(node.slot, false, 0, 0); // Default/unmapped -> Serial slot
        } else if (node.kind == NodePreset::Kind::ParallelBlock) {
            // Branch A
            if (!node.branches.empty()) {
                const auto& bA = node.branches[0];
                out << "  ; PRACCY_BRANCH_A name=\"" << sanitize(bA.name)
                    << "\" gainDb=" << bA.gainDb
                    << " pan=" << bA.pan
                    << " muted=" << (bA.muted ? 1 : 0)
                    << " solo=" << (bA.solo ? 1 : 0)
                    << " phaseInvert=" << (bA.phaseInvert ? 1 : 0)
                    << "\n";
                for (const auto& slot : bA.slots) {
                    writeSlot(slot, true, 3, 3); // 1|2 pins -> Branch A
                }
            }
            // Branch B
            if (node.branches.size() > 1) {
                const auto& bB = node.branches[1];
                out << "  ; PRACCY_BRANCH_B name=\"" << sanitize(bB.name)
                    << "\" gainDb=" << bB.gainDb
                    << " pan=" << bB.pan
                    << " muted=" << (bB.muted ? 1 : 0)
                    << " solo=" << (bB.solo ? 1 : 0)
                    << " phaseInvert=" << (bB.phaseInvert ? 1 : 0)
                    << "\n";
                for (const auto& slot : bB.slots) {
                    writeSlot(slot, true, 12, 12); // 4|8 pins -> Branch B
                }
            }
        }
    }

    out << ">\n";
    return true;
}

} // namespace praccy::state
