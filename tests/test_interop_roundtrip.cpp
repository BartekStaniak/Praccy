#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <string_view>
#include <array>
#include <cmath>
#undef NDEBUG
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <memory>

#include "../src/utils/base64_utils.h"
#include "../src/utils/xml_parser.h"
#include "../src/utils/guid_utils.h"
#include "../src/utils/archive_utils.h"
#include "../src/state/daw_interop/reaper_adapter.h"
#include "../src/state/daw_interop/ableton_adapter.h"
#include "../src/state/daw_interop/dawproject_adapter.h"
#include "../src/state/daw_interop/vstpreset_adapter.h"
#include "../src/state/daw_interop/universal_daw_interop_manager.h"
#include "../src/audio/audio_node.h"
#include "../src/audio/audio_buffer.h"
#include "../src/version.h"

using namespace praccy;

// ============================================================================
// TEST REPORTING HARNESS
// ============================================================================
static int g_passedTests = 0;
static int g_totalTests = 0;

#define RUN_TEST(fn) do { \
    g_totalTests++; \
    std::cout << "  [TEST] " << #fn << "... " << std::flush; \
    try { \
        fn(); \
        g_passedTests++; \
        std::cout << "PASSED\n"; \
    } catch (const std::exception& e) { \
        std::cout << "FAILED: " << e.what() << "\n"; \
    } catch (...) { \
        std::cout << "FAILED: Unknown exception\n"; \
    } \
} while(0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        throw std::runtime_error("Assertion failed: " #cond); \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        std::ostringstream ss; \
        ss << "Assertion failed: (" #a " == " #b ") [" << (a) << " != " << (b) << "]"; \
        throw std::runtime_error(ss.str()); \
    } \
} while(0)

// ============================================================================
// TEST CASES
// ============================================================================

void test_guid_normalization_roundtrip() {
    // 1. Steinberg FUID (Mixed-endian Microsoft GUID string)
    std::string steinbergGuid = "{56535433-5345-4E44-4D49-584552000000}";
    std::string canonicalGuid = utils::GuidUtils::normalizeToCanonicalGuid(steinbergGuid, utils::DawGuidFormat::SteinbergFUID);
    ASSERT_EQ(canonicalGuid.size(), 32);

    // 2. Ableton Big-Endian conversion
    std::string abletonGuid = utils::GuidUtils::toAbletonGuidString(canonicalGuid);
    ASSERT_EQ(abletonGuid.size(), 32);
    std::string backFromAbleton = utils::GuidUtils::normalizeToCanonicalGuid(abletonGuid, utils::DawGuidFormat::AbletonBigEndian);
    ASSERT_EQ(backFromAbleton, canonicalGuid);

    // 3. DAWproject UUID conversion (RFC 4122 hyphenated)
    std::string dawprojectUuid = utils::GuidUtils::toDawprojectUuidString(canonicalGuid);
    ASSERT_EQ(dawprojectUuid.size(), 36);
    ASSERT_EQ(dawprojectUuid[8], '-');
    ASSERT_EQ(dawprojectUuid[13], '-');
    ASSERT_EQ(dawprojectUuid[18], '-');
    ASSERT_EQ(dawprojectUuid[23], '-');
    std::string backFromDawproject = utils::GuidUtils::normalizeToCanonicalGuid(dawprojectUuid, utils::DawGuidFormat::DawprojectUuid);
    ASSERT_EQ(backFromDawproject, canonicalGuid);

    // 4. Back to Steinberg format
    std::string backToSteinberg = utils::GuidUtils::toSteinbergGuidString(canonicalGuid);
    ASSERT_EQ(backToSteinberg.front(), '{');
    ASSERT_EQ(backToSteinberg.back(), '}');
    std::string roundtripCanonical = utils::GuidUtils::normalizeToCanonicalGuid(backToSteinberg, utils::DawGuidFormat::SteinbergFUID);
    ASSERT_EQ(roundtripCanonical, canonicalGuid);
}

void test_base64_codec_roundtrip() {
    // Test empty
    std::vector<uint8_t> emptyData;
    std::string encEmpty = utils::Base64::encode(emptyData);
    ASSERT_TRUE(encEmpty.empty());
    std::vector<uint8_t> decEmpty;
    ASSERT_TRUE(utils::Base64::decode(encEmpty, decEmpty));
    ASSERT_TRUE(decEmpty.empty());

    // Test arbitrary binary data (including null bytes and 0xFF)
    std::vector<uint8_t> sampleData;
    sampleData.resize(256);
    for (size_t i = 0; i < 256; ++i) {
        sampleData[i] = static_cast<uint8_t>(i);
    }

    std::string encoded = utils::Base64::encode(sampleData);
    ASSERT_TRUE(!encoded.empty());

    std::vector<uint8_t> decoded;
    ASSERT_TRUE(utils::Base64::decode(encoded, decoded));
    ASSERT_EQ(decoded.size(), sampleData.size());
    ASSERT_TRUE(decoded == sampleData);

    // Test with whitespace and newlines injected
    std::string formatted = " \r\n  " + encoded.substr(0, 40) + "\r\n  " + encoded.substr(40) + " \n\t ";
    std::vector<uint8_t> decodedFormatted;
    ASSERT_TRUE(utils::Base64::decode(formatted, decodedFormatted));
    ASSERT_TRUE(decodedFormatted == sampleData);
}

void test_reaper_roundtrip() {
    auto tempDir = std::filesystem::temp_directory_path() / "praccy_roundtrip_test";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    auto rfxFile = tempDir / "test_reaper.RfxChain";

    state::ScenePreset originalScene;
    originalScene.name = "Reaper_Lead_Rig";
    originalScene.sourceDawName = "Praccy";

    // 1. Pre-FX Serial Slot
    state::NodePreset serialNode;
    serialNode.kind = state::NodePreset::Kind::Plugin;
    serialNode.slot.name = "Gate Plugin";
    serialNode.slot.bundleFileName = "GatePlugin.vst3";
    serialNode.slot.classGuid = "11112222333344445555666677778888";
    serialNode.slot.state = {0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB, 0xCC, 0xDD};
    originalScene.nodes.push_back(std::move(serialNode));

    // 2. Parallel Block with Branch A and Branch B
    state::NodePreset parNode;
    parNode.kind = state::NodePreset::Kind::ParallelBlock;

    state::BranchPreset branchA;
    branchA.name = "Branch A";
    state::PluginSlotPreset slotA;
    slotA.name = "Amp Sim";
    slotA.bundleFileName = "AmpSim.vst3";
    slotA.classGuid = "22223333444455556666777788889999";
    slotA.state = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
    branchA.slots.push_back(std::move(slotA));

    state::BranchPreset branchB;
    branchB.name = "Branch B";
    state::PluginSlotPreset slotB;
    slotB.name = "Cab Loader";
    slotB.bundleFileName = "CabLoader.vst3";
    slotB.classGuid = "33334444555566667777888899990000";
    slotB.state = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    branchB.slots.push_back(std::move(slotB));

    parNode.branches.push_back(std::move(branchA));
    parNode.branches.push_back(std::move(branchB));
    originalScene.nodes.push_back(std::move(parNode));

    // Export to .RfxChain
    state::ReaperAdapter reaper;
    ASSERT_TRUE(reaper.exportFromScene(originalScene, rfxFile));
    ASSERT_TRUE(std::filesystem::exists(rfxFile));

    // Re-import from .RfxChain
    auto parsedSceneOpt = reaper.parseToScene(rfxFile);
    ASSERT_TRUE(parsedSceneOpt.has_value());
    const auto& parsedScene = *parsedSceneOpt;

    // Verify topology and parameters
    ASSERT_EQ(parsedScene.nodes.size(), 2);
    ASSERT_TRUE(parsedScene.nodes[0].kind == state::NodePreset::Kind::Plugin);
    ASSERT_EQ(parsedScene.nodes[0].slot.name, "Gate Plugin");
    ASSERT_EQ(parsedScene.nodes[0].slot.classGuid, "11112222333344445555666677778888");
    ASSERT_TRUE(parsedScene.nodes[0].slot.state == originalScene.nodes[0].slot.state);

    ASSERT_TRUE(parsedScene.nodes[1].kind == state::NodePreset::Kind::ParallelBlock);
    ASSERT_EQ(parsedScene.nodes[1].branches.size(), 2);

    // Branch A
    ASSERT_EQ(parsedScene.nodes[1].branches[0].slots.size(), 1);
    ASSERT_EQ(parsedScene.nodes[1].branches[0].slots[0].name, "Amp Sim");
    ASSERT_EQ(parsedScene.nodes[1].branches[0].slots[0].classGuid, "22223333444455556666777788889999");
    ASSERT_TRUE(parsedScene.nodes[1].branches[0].slots[0].state == originalScene.nodes[1].branches[0].slots[0].state);

    // Branch B
    ASSERT_EQ(parsedScene.nodes[1].branches[1].slots.size(), 1);
    ASSERT_EQ(parsedScene.nodes[1].branches[1].slots[0].name, "Cab Loader");
    ASSERT_EQ(parsedScene.nodes[1].branches[1].slots[0].classGuid, "33334444555566667777888899990000");
    ASSERT_TRUE(parsedScene.nodes[1].branches[1].slots[0].state == originalScene.nodes[1].branches[1].slots[0].state);

    std::filesystem::remove(rfxFile, ec);
}

void test_ableton_roundtrip() {
    auto tempDir = std::filesystem::temp_directory_path() / "praccy_roundtrip_test";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    auto adgFile = tempDir / "test_ableton.adg";

    state::ScenePreset originalScene;
    originalScene.name = "Ableton_Ambient_Chain";
    originalScene.sourceDawName = "Praccy";

    // 1. Serial Node
    state::NodePreset serialNode;
    serialNode.kind = state::NodePreset::Kind::Plugin;
    serialNode.slot.name = "Compressor";
    serialNode.slot.bundleFileName = "Compressor.vst3";
    serialNode.slot.classGuid = "aabbccddeeff00112233445566778899";
    serialNode.slot.state = {0x05, 0x10, 0x15, 0x20, 0x25, 0x30};
    originalScene.nodes.push_back(std::move(serialNode));

    // 2. Parallel Block
    state::NodePreset parNode;
    parNode.kind = state::NodePreset::Kind::ParallelBlock;

    state::BranchPreset branchA;
    branchA.name = "Branch A";
    state::PluginSlotPreset slotA;
    slotA.name = "Delay Plugin";
    slotA.bundleFileName = "Delay.vst3";
    slotA.classGuid = "11223344556677889900aabbccddeeff";
    slotA.state = {0x42, 0x43, 0x44, 0x45};
    branchA.slots.push_back(std::move(slotA));

    state::BranchPreset branchB;
    branchB.name = "Branch B";
    state::PluginSlotPreset slotB;
    slotB.name = "Reverb Plugin";
    slotB.bundleFileName = "Reverb.vst3";
    slotB.classGuid = "ffff0000eeee1111dddd2222cccc3333";
    slotB.state = {0x99, 0x88, 0x77, 0x66, 0x55, 0x44};
    branchB.slots.push_back(std::move(slotB));

    parNode.branches.push_back(std::move(branchA));
    parNode.branches.push_back(std::move(branchB));
    originalScene.nodes.push_back(std::move(parNode));

    // Export to .adg
    state::AbletonAdapter ableton;
    ASSERT_TRUE(ableton.exportFromScene(originalScene, adgFile));
    ASSERT_TRUE(std::filesystem::exists(adgFile));

    // Verify gzip compression integrity
    std::ifstream f(adgFile, std::ios::binary);
    std::vector<uint8_t> gzBytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    ASSERT_TRUE(gzBytes.size() >= 2);
    ASSERT_EQ(gzBytes[0], 0x1F);
    ASSERT_EQ(gzBytes[1], 0x8B);

    std::string decompressedXml;
    ASSERT_TRUE(utils::ArchiveUtils::gzipDecompress(gzBytes, decompressedXml));
    ASSERT_TRUE(decompressedXml.find("<Ableton") != std::string::npos);
    ASSERT_TRUE(decompressedXml.find("<AudioEffectGroupDevice") != std::string::npos);

    // Re-import from .adg
    auto parsedSceneOpt = ableton.parseToScene(adgFile);
    ASSERT_TRUE(parsedSceneOpt.has_value());
    const auto& parsedScene = *parsedSceneOpt;

    // Verify topology and parameters
    ASSERT_TRUE(!parsedScene.nodes.empty());
    // Find the parallel block
    const state::NodePreset* foundPar = nullptr;
    const state::NodePreset* foundSerial = nullptr;
    for (const auto& n : parsedScene.nodes) {
        if (n.kind == state::NodePreset::Kind::ParallelBlock) foundPar = &n;
        else if (n.kind == state::NodePreset::Kind::Plugin) foundSerial = &n;
    }

    ASSERT_TRUE(foundPar != nullptr);
    (void)foundSerial;
    ASSERT_EQ(foundPar->branches.size(), 2);

    // Verify Branch A & B slot contents
    ASSERT_TRUE(!foundPar->branches[0].slots.empty());
    ASSERT_TRUE(!foundPar->branches[1].slots.empty());
    ASSERT_EQ(foundPar->branches[0].slots[0].name, "Delay Plugin");
    ASSERT_EQ(foundPar->branches[0].slots[0].classGuid, "11223344556677889900aabbccddeeff");
    ASSERT_TRUE(foundPar->branches[0].slots[0].state == originalScene.nodes[1].branches[0].slots[0].state);

    ASSERT_EQ(foundPar->branches[1].slots[0].name, "Reverb Plugin");
    ASSERT_EQ(foundPar->branches[1].slots[0].classGuid, "ffff0000eeee1111dddd2222cccc3333");
    ASSERT_TRUE(foundPar->branches[1].slots[0].state == originalScene.nodes[1].branches[1].slots[0].state);

    std::filesystem::remove(adgFile, ec);
}

void test_dawproject_roundtrip() {
    auto tempDir = std::filesystem::temp_directory_path() / "praccy_roundtrip_test";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    auto dawprojectFile = tempDir / "test_project.dawproject";

    state::ScenePreset originalScene;
    originalScene.name = "DAWproject_Rig";
    originalScene.sourceDawName = "Praccy";
    originalScene.tempoBpm = 136.5f;
    originalScene.timeSigNumerator = 7;
    originalScene.timeSigDenominator = 8;

    // 1. Serial Node
    state::NodePreset serialNode;
    serialNode.kind = state::NodePreset::Kind::Plugin;
    serialNode.slot.name = "Modern Drive";
    serialNode.slot.bundleFileName = "ModernDrive.vst3";
    serialNode.slot.classGuid = "1234567890abcdef1234567890abcdef";
    serialNode.slot.state = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
    originalScene.nodes.push_back(std::move(serialNode));

    // 2. Parallel Block
    state::NodePreset parNode;
    parNode.kind = state::NodePreset::Kind::ParallelBlock;

    state::BranchPreset branchA;
    branchA.name = "Clean Amp";
    state::PluginSlotPreset slotA;
    slotA.name = "Clean Amp";
    slotA.bundleFileName = "CleanAmp.vst3";
    slotA.classGuid = "abcdef0123456789abcdef0123456789";
    slotA.state = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    branchA.slots.push_back(std::move(slotA));

    state::BranchPreset branchB;
    branchB.name = "Dirty Amp";
    state::PluginSlotPreset slotB;
    slotB.name = "Dirty Amp";
    slotB.bundleFileName = "DirtyAmp.vst3";
    slotB.classGuid = "fedcba9876543210fedcba9876543210";
    slotB.state = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    branchB.slots.push_back(std::move(slotB));

    parNode.branches.push_back(std::move(branchA));
    parNode.branches.push_back(std::move(branchB));
    originalScene.nodes.push_back(std::move(parNode));

    // Export to .dawproject
    state::DawprojectAdapter dawproject;
    ASSERT_TRUE(dawproject.exportFromScene(originalScene, dawprojectFile));
    ASSERT_TRUE(std::filesystem::exists(dawprojectFile));

    // Unzip and rigorously verify manifest.xml & project.xml structure (Task 2.2.3 & Invariant 3)
    auto inspectDir = tempDir / "inspect_dawproject";
    std::filesystem::create_directories(inspectDir, ec);
    ASSERT_TRUE(utils::ArchiveUtils::unzipFile(dawprojectFile, inspectDir));

    // Verify manifest.xml
    auto manifestPath = inspectDir / "manifest.xml";
    ASSERT_TRUE(std::filesystem::exists(manifestPath));
    auto manifestNode = utils::XmlParser::parseFile(manifestPath);
    ASSERT_TRUE(manifestNode != nullptr);
    ASSERT_EQ(manifestNode->tag, "manifest");
    ASSERT_EQ(manifestNode->attribute("formatVersion"), "1.0");
    ASSERT_EQ(manifestNode->attribute("application"), "Praccy");
    ASSERT_EQ(manifestNode->attribute("appVersion"), praccy::kDawprojectAppVersion);

    // Verify project.xml
    auto projectPath = inspectDir / "project.xml";
    ASSERT_TRUE(std::filesystem::exists(projectPath));
    auto projectNode = utils::XmlParser::parseFile(projectPath);
    ASSERT_TRUE(projectNode != nullptr);
    ASSERT_EQ(projectNode->tag, "project");

    auto transportNode = projectNode->firstChild("transport");
    ASSERT_TRUE(transportNode != nullptr);
    float parsedBpm = std::strtof(transportNode->attribute("tempo").c_str(), nullptr);
    ASSERT_TRUE(std::fabs(parsedBpm - 136.5f) < 0.01f);
    ASSERT_EQ(transportNode->attribute("timeSignatureNumerator"), "7");
    ASSERT_EQ(transportNode->attribute("timeSignatureDenominator"), "8");

    // Re-import via DawprojectAdapter
    auto parsedSceneOpt = dawproject.parseToScene(dawprojectFile);
    ASSERT_TRUE(parsedSceneOpt.has_value());
    const auto& parsedScene = *parsedSceneOpt;

    // Verify transport parity
    ASSERT_TRUE(std::fabs(parsedScene.tempoBpm - 136.5f) < 0.01f);
    ASSERT_EQ(parsedScene.timeSigNumerator, 7);
    ASSERT_EQ(parsedScene.timeSigDenominator, 8);

    // Verify nodes & bit-exact binary state
    ASSERT_EQ(parsedScene.nodes.size(), 2);
    ASSERT_TRUE(parsedScene.nodes[0].kind == state::NodePreset::Kind::Plugin);
    ASSERT_EQ(parsedScene.nodes[0].slot.name, "Modern Drive");
    ASSERT_EQ(parsedScene.nodes[0].slot.classGuid, "1234567890abcdef1234567890abcdef");
    ASSERT_TRUE(parsedScene.nodes[0].slot.state == originalScene.nodes[0].slot.state);

    ASSERT_TRUE(parsedScene.nodes[1].kind == state::NodePreset::Kind::ParallelBlock);
    ASSERT_EQ(parsedScene.nodes[1].branches.size(), 2);
    ASSERT_EQ(parsedScene.nodes[1].branches[0].slots.size(), 1);
    ASSERT_EQ(parsedScene.nodes[1].branches[0].slots[0].name, "Clean Amp");
    ASSERT_EQ(parsedScene.nodes[1].branches[0].slots[0].classGuid, "abcdef0123456789abcdef0123456789");
    ASSERT_TRUE(parsedScene.nodes[1].branches[0].slots[0].state == originalScene.nodes[1].branches[0].slots[0].state);

    ASSERT_EQ(parsedScene.nodes[1].branches[1].slots.size(), 1);
    ASSERT_EQ(parsedScene.nodes[1].branches[1].slots[0].name, "Dirty Amp");
    ASSERT_EQ(parsedScene.nodes[1].branches[1].slots[0].classGuid, "fedcba9876543210fedcba9876543210");
    ASSERT_TRUE(parsedScene.nodes[1].branches[1].slots[0].state == originalScene.nodes[1].branches[1].slots[0].state);

    std::filesystem::remove_all(inspectDir, ec);
    std::filesystem::remove(dawprojectFile, ec);
}

void test_vstpreset_roundtrip() {
    auto tempDir = std::filesystem::temp_directory_path() / "praccy_roundtrip_test";
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);
    auto vstpresetFile = tempDir / "test_slot.vstpreset";

    state::ScenePreset originalScene;
    originalScene.name = "Solo_Preset";

    state::NodePreset node;
    node.kind = state::NodePreset::Kind::Plugin;
    node.slot.name = "Stereo Chorus";
    node.slot.classGuid = "112233445566778899aabbccddeeff00";
    node.slot.state = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0xAA, 0xBB, 0xCC, 0xDD};
    originalScene.nodes.push_back(std::move(node));

    state::VstPresetAdapter vstAdapter;
    ASSERT_TRUE(vstAdapter.exportFromScene(originalScene, vstpresetFile));
    ASSERT_TRUE(std::filesystem::exists(vstpresetFile));

    // Verify RIFF structure
    std::ifstream vf(vstpresetFile, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(vf)), std::istreambuf_iterator<char>());
    vf.close();

    ASSERT_TRUE(bytes.size() >= 48);
    ASSERT_EQ(bytes[0], 'V'); ASSERT_EQ(bytes[1], 'S');
    ASSERT_EQ(bytes[2], 'T'); ASSERT_EQ(bytes[3], '3');

    // Parse back
    auto parsedSceneOpt = vstAdapter.parseToScene(vstpresetFile);
    ASSERT_TRUE(parsedSceneOpt.has_value());
    const auto& parsedScene = *parsedSceneOpt;

    ASSERT_EQ(parsedScene.nodes.size(), 1);
    ASSERT_EQ(parsedScene.nodes[0].slot.classGuid, "112233445566778899aabbccddeeff00");
    ASSERT_TRUE(parsedScene.nodes[0].slot.state == originalScene.nodes[0].slot.state);

    std::filesystem::remove(vstpresetFile, ec);
}

void test_missing_plugin_placeholder_dry_passthrough() {
    audio::MissingPluginPlaceholder placeholder(
        "Archetype Gojira X",
        "VST3",
        "1234567890abcdef1234567890abcdef",
        "com.neuraldsp.gojira"
    );

    ASSERT_TRUE(placeholder.isMissing());
    ASSERT_EQ(placeholder.name(), "Archetype Gojira X");
    ASSERT_EQ(placeholder.expectedFormat(), "VST3");
    ASSERT_EQ(placeholder.classGuid(), "1234567890abcdef1234567890abcdef");
    ASSERT_EQ(placeholder.clapId(), "com.neuraldsp.gojira");
    ASSERT_TRUE(placeholder.type() == audio::NodeType::Plugin);

    // Test audio processing: Bit-exact dry pass-through (Invariant 4)
    constexpr uint32_t kBlockSize = 256;
    std::vector<float> inL(kBlockSize);
    std::vector<float> inR(kBlockSize);
    std::vector<float> outL(kBlockSize, 0.0f);
    std::vector<float> outR(kBlockSize, 0.0f);

    for (uint32_t i = 0; i < kBlockSize; ++i) {
        inL[i] = std::sin(2.0f * 3.14159265f * static_cast<float>(i) / 64.0f);
        inR[i] = std::cos(2.0f * 3.14159265f * static_cast<float>(i) / 64.0f);
    }

    float* inChannels[2] = { inL.data(), inR.data() };
    float* outChannels[2] = { outL.data(), outR.data() };

    audio::AudioBufferView inputView(inChannels, 2, kBlockSize);
    audio::AudioBufferView outputView(outChannels, 2, kBlockSize);

    audio::AudioProcessContext ctx{
        .input = inputView,
        .output = outputView,
        .sampleRate = 48000.0,
        .numSamples = kBlockSize
    };

    placeholder.process(ctx);

    for (uint32_t i = 0; i < kBlockSize; ++i) {
        ASSERT_EQ(outL[i], inL[i]);
        ASSERT_EQ(outR[i], inR[i]);
    }
}

void test_universal_interop_manager_adapters() {
    auto& mgr = state::UniversalDawInteropManager::instance();

    // Check adapter counts
    ASSERT_EQ(mgr.adapters().size(), 4);

    // Test format detection by path and extension
    ASSERT_TRUE(mgr.findAdapter("test.RfxChain") != nullptr);
    ASSERT_TRUE(mgr.findAdapter("test.rfxchain") != nullptr);
    ASSERT_TRUE(mgr.findAdapter("test.adg") != nullptr);
    ASSERT_TRUE(mgr.findAdapter("test.ADG") != nullptr);
    ASSERT_TRUE(mgr.findAdapter("test.dawproject") != nullptr);
    ASSERT_TRUE(mgr.findAdapter("test.vstpreset") != nullptr);

    ASSERT_TRUE(mgr.findAdapter("test.wav") == nullptr);
    ASSERT_TRUE(mgr.findAdapter("test.mp3") == nullptr);
    ASSERT_TRUE(mgr.findAdapter("test.txt") == nullptr);

    ASSERT_TRUE(mgr.findAdapterByExtension(".rfxchain") != nullptr);
    ASSERT_TRUE(mgr.findAdapterByExtension(".adg") != nullptr);
    ASSERT_TRUE(mgr.findAdapterByExtension(".dawproject") != nullptr);
    ASSERT_TRUE(mgr.findAdapterByExtension(".vstpreset") != nullptr);
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================
int main(int argc, char* argv[]) {
    bool runAll = true;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--all") {
            runAll = true;
        }
    }
    (void)runAll;

    std::cout << "================================================================================\n";
    std::cout << "   PRACCY V2.4 STAGE 2 UNIVERSAL DAW INTEROP ROUNDTRIP HARNESS (Gate 2.2)\n";
    std::cout << "================================================================================\n";

    RUN_TEST(test_guid_normalization_roundtrip);
    RUN_TEST(test_base64_codec_roundtrip);
    RUN_TEST(test_reaper_roundtrip);
    RUN_TEST(test_ableton_roundtrip);
    RUN_TEST(test_dawproject_roundtrip);
    RUN_TEST(test_vstpreset_roundtrip);
    RUN_TEST(test_missing_plugin_placeholder_dry_passthrough);
    RUN_TEST(test_universal_interop_manager_adapters);

    std::cout << "================================================================================\n";
    std::cout << "RESULTS: " << g_passedTests << " / " << g_totalTests << " tests passed.\n";
    if (g_passedTests == g_totalTests) {
        std::cout << ">>> GATE 2.2 CROSS-DAW ROUNDTRIP PARITY SUITE 100% PASS <<<\n";
        std::cout << "================================================================================\n";
        return 0;
    } else {
        std::cout << ">>> GATE 2.2 CROSS-DAW ROUNDTRIP SUITE FAILED <<<\n";
        std::cout << "================================================================================\n";
        return 1;
    }
}
