#pragma once

#include "daw_adapter.h"

namespace praccy::state {

class ReaperAdapter : public IDawChainAdapter {
public:
    ReaperAdapter() = default;
    ~ReaperAdapter() override = default;

    [[nodiscard]] std::string formatName() const override {
        return "Reaper FX Chain";
    }

    [[nodiscard]] std::vector<std::string> fileExtensions() const override {
        return {".RfxChain", ".rfxchain"};
    }

    bool canHandle(const std::filesystem::path& path) const override;

    std::optional<ScenePreset> parseToScene(const std::filesystem::path& path) override;
    bool exportFromScene(const ScenePreset& scene, const std::filesystem::path& outputPath) override;

    // Helper functions for parsing chunks and PINMAP bitmasks
    static bool parsePinmap(std::string_view line, uint64_t& inMask, uint64_t& outMask);
    static bool unpackVst3ChunkEnvelope(const std::vector<uint8_t>& rawChunk, std::vector<uint8_t>& outCompState, std::vector<uint8_t>& outCtrlState);
    static std::vector<uint8_t> packVst3ChunkEnvelope(const std::vector<uint8_t>& compState, const std::vector<uint8_t>& ctrlState);
};

} // namespace praccy::state
