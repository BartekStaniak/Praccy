#pragma once

#include "daw_adapter.h"

namespace praccy::state {

class VstPresetAdapter : public IDawChainAdapter {
public:
    VstPresetAdapter() = default;
    ~VstPresetAdapter() override = default;

    [[nodiscard]] std::string formatName() const override {
        return "Steinberg VST3 Preset";
    }

    [[nodiscard]] std::vector<std::string> fileExtensions() const override {
        return {".vstpreset"};
    }

    bool canHandle(const std::filesystem::path& path) const override;

    std::optional<ScenePreset> parseToScene(const std::filesystem::path& path) override;
    bool exportFromScene(const ScenePreset& scene, const std::filesystem::path& outputPath) override;

    static bool parseVstPreset(
        const std::vector<uint8_t>& fileData,
        std::string& outClassGuid,
        std::vector<uint8_t>& outCompState
    );

    static std::vector<uint8_t> packVstPreset(
        const std::string& classGuid,
        const std::vector<uint8_t>& compState
    );
};

} // namespace praccy::state
