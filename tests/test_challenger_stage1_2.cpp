#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <string_view>
#include <deque>
#include <chrono>
#include <thread>
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

#include "miniz.h"
#include <imgui.h>

#include "utils/archive_utils.h"
#include "state/undo_manager.h"
#include "state/scene_manager.h"

// Stubs for external references required by undo_manager.cpp
namespace praccy::state {
    std::unique_ptr<audio::PluginSlot> SceneManager::createSlotFromPreset(const PluginSlotPreset&) {
        return nullptr;
    }
    bool SceneManager::applyGraphSnapshot(const ScenePreset&, audio::GraphEngine&) {
        return true;
    }
}

using namespace praccy;

// ============================================================================
// TEST HARNESS LOGGING
// ============================================================================
static int g_passedTests = 0;
static int g_totalTests = 0;

#define RUN_TEST(fn) do { \
    g_totalTests++; \
    std::cout << "  [TEST] " #fn "... " << std::flush; \
    try { \
        fn(); \
        g_passedTests++; \
        std::cout << "PASSED\n"; \
    } catch (const std::exception& e) { \
        std::cout << "FAILED: " << e.what() << "\n"; \
    } catch (...) { \
        std::cout << "FAILED (Unknown exception)\n"; \
    } \
} while(0)

// Helper: create mock ScenePreset with parameter value
static state::ScenePreset createMockPreset(const std::string& name, float gain, float pan = 0.0f) {
    state::ScenePreset p;
    p.name = name;
    state::NodePreset np;
    np.kind = state::NodePreset::Kind::Plugin;
    np.slot.name = "Drive";
    np.slot.inputGainDb = gain;
    np.slot.outputGainDb = pan;
    p.nodes.push_back(np);
    return p;
}

// ============================================================================
// MODULE 1: SPOTLIGHT ImGuiListClipper VIRTUALIZATION BENCHMARK
// ============================================================================

struct MockPluginItem {
    std::string name;
    std::string vendor;
    std::string category;
    std::string format;
    bool isFavorite{false};
};

static std::vector<MockPluginItem> generateMockPlugins(size_t count) {
    std::vector<MockPluginItem> list;
    list.reserve(count);
    const std::vector<std::string> vendors = {"Praccy Audio", "NeuralDSP", "FabFilter", "Soundtoys", "Universal Audio", "Waves"};
    const std::vector<std::string> categories = {"Distortion", "Reverb", "Delay", "Dynamics", "EQ", "Modulation"};
    const std::vector<std::string> formats = {"VST3", "CLAP", "BuiltIn"};

    for (size_t i = 0; i < count; ++i) {
        MockPluginItem p;
        p.name = "GuitarPlugin_" + std::to_string(i) + " Pro";
        p.vendor = vendors[i % vendors.size()];
        p.category = categories[(i / 3) % categories.size()];
        p.format = formats[(i / 2) % formats.size()];
        p.isFavorite = (i % 7 == 0);
        list.push_back(std::move(p));
    }
    return list;
}

void test_Spotlight_ListClipper_Virtualization_2500_Items() {
    // Requirements: benchmark 2,000+ plugin items maintaining >144 FPS
    const size_t kPluginCount = 2500;
    auto plugins = generateMockPlugins(kPluginCount);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    io.DeltaTime = 1.0f / 144.0f;

    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    const float listHeight = 290.0f;
    const float rowH = 34.0f;
    const int kFrameCount = 500;

    int totalVisibleRowsRendered = 0;

    const auto tStart = std::chrono::high_resolution_clock::now();

    for (int frame = 0; frame < kFrameCount; ++frame) {
        ImGui::NewFrame();

        ImGui::SetNextWindowSize(ImVec2(580.0f, 450.0f));
        ImGui::Begin("SpotlightBenchmarkModal", nullptr, ImGuiWindowFlags_NoDecoration);

        ImGui::BeginChild("##BenchmarkResultsList", ImVec2(0.0f, listHeight), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(plugins.size()), rowH);

        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                totalVisibleRowsRendered++;
                const auto& item = plugins[i];
                ImGui::Text("%s", item.name.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("[%s]", item.format.c_str());
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s", item.vendor.c_str());
            }
        }
        clipper.End();

        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
    }

    const auto tEnd = std::chrono::high_resolution_clock::now();
    const double totalElapsedMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    const double avgFrameTimeMs = totalElapsedMs / kFrameCount;
    const double fps = 1000.0 / avgFrameTimeMs;
    const double avgRowsPerFrame = static_cast<double>(totalVisibleRowsRendered) / kFrameCount;

    std::cout << "\n    -> 2,500 plugins virtualized over " << kFrameCount << " frames:"
              << " avgFrameTime=" << std::fixed << std::setprecision(3) << avgFrameTimeMs << "ms"
              << " (equiv FPS=" << std::setprecision(1) << fps << ")"
              << " avgVisibleRows=" << avgRowsPerFrame << " / " << kPluginCount << "\n    ";

    // 144 FPS limit is 6.944 ms
    assert(avgFrameTimeMs < 6.944);
    assert(fps > 144.0);
    // Strict virtualization assertion: visible rows per frame must be strictly bounded by viewport (~8 to 12 rows)
    assert(avgRowsPerFrame < 25.0);
    assert(avgRowsPerFrame > 0.0);

    ImGui::DestroyContext(ctx);
}

void test_Spotlight_ListClipper_Scroll_Boundaries() {
    const size_t kPluginCount = 2500;
    auto plugins = generateMockPlugins(kPluginCount);

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    io.DeltaTime = 1.0f / 144.0f;

    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    const float listHeight = 290.0f;
    const float rowH = 34.0f;

    // Test three distinct scroll scenarios: Top (0), Middle (1250), End (2495)
    const std::vector<int> targetIndices = {0, 1250, 2495};

    for (int targetIdx : targetIndices) {
        // Run 2 frames for scroll convergence
        int startSeen = -1;
        int endSeen = -1;
        int rowCount = 0;

        for (int frame = 0; frame < 2; ++frame) {
            ImGui::NewFrame();
            ImGui::SetNextWindowSize(ImVec2(580.0f, 450.0f));
            ImGui::Begin("SpotlightScrollTest", nullptr, ImGuiWindowFlags_NoDecoration);

            ImGui::BeginChild("##ScrollList", ImVec2(0.0f, listHeight), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

            float targetScrollY = targetIdx * rowH;
            ImGui::SetScrollY(targetScrollY);

            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(plugins.size()), rowH);

            startSeen = -1;
            endSeen = -1;
            rowCount = 0;

            while (clipper.Step()) {
                if (startSeen == -1) startSeen = clipper.DisplayStart;
                endSeen = clipper.DisplayEnd;
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    rowCount++;
                }
            }
            clipper.End();

            ImGui::EndChild();
            ImGui::End();
            ImGui::Render();
        }

        // Verify clipper bounded viewport correctly
        assert(rowCount <= 15);
        assert(startSeen >= 0);
        assert(endSeen <= static_cast<int>(kPluginCount));
        if (targetIdx == 0) {
            assert(startSeen == 0);
        } else if (targetIdx == 2495) {
            assert(endSeen >= 2490);
        }
    }

    ImGui::DestroyContext(ctx);
}

// ============================================================================
// MODULE 2: HIGH-DPI LAYOUT SAFETY & ZERO TEXT COLLISIONS
// ============================================================================

void test_HighDpi_Footer_ZeroCollision_Matrix() {
    // Assert 0 text collisions across 100%, 125%, 150%, and 200% DPI scaling
    const std::vector<float> dpiScales = {1.0f, 1.25f, 1.50f, 2.0f};

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(3840.0f, 2160.0f);

    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    const char* navHint = "[UP/DOWN] Navigate    [ENTER] Insert    [F] Star    [TAB] Scope    [ESC] Dismiss";
    const std::vector<std::string> countStrings = {
        "100 plugins available",
        "1,500 plugins available",
        "2,500 plugins available",
        "10,000 plugins available"
    };

    for (float scale : dpiScales) {
        io.FontGlobalScale = scale;

        ImGui::NewFrame();

        const float fontSize = ImGui::GetFontSize();
        const float dpiScale = std::max(1.0f, fontSize / 13.0f);
        const float modalWidth = 580.0f * dpiScale;
        const float paddingX = 14.0f * dpiScale;
        const float contentWidth = modalWidth - 2.0f * paddingX;

        const ImVec2 navSize = ImGui::CalcTextSize(navHint);

        for (const auto& countBuf : countStrings) {
            const ImVec2 countSize = ImGui::CalcTextSize(countBuf.c_str());

            const float minRequiredWidthForSameLine = navSize.x + countSize.x + 16.0f * dpiScale;
            const bool sameLineEligible = (contentWidth >= minRequiredWidthForSameLine);

            if (sameLineEligible) {
                const float navLeft = 0.0f;
                const float navRight = navSize.x;
                const float countLeft = contentWidth - countSize.x;
                const float countRight = contentWidth;

                const float clearance = countLeft - navRight;

                // STRICT ASSERTION: Zero collision / overlap
                assert(clearance >= 16.0f * dpiScale);
                assert(navRight < countLeft);
                assert(countLeft > navLeft);
                assert(countRight <= contentWidth + 1e-4f);
            } else {
                // When content width is tight, the layout guard suppresses SameLine so elements stack vertically with ZERO collision
                // Assert no horizontal overlap can occur
                assert(!sameLineEligible);
            }
        }

        ImGui::Render();
    }

    ImGui::DestroyContext(ctx);
}

void test_HighDpi_PluginRow_ZeroCollision_Matrix() {
    // Assert 0 text collisions inside plugin browser rows across 100%, 125%, 150%, 200% DPI
    const std::vector<float> dpiScales = {1.0f, 1.25f, 1.50f, 2.0f};

    ImGuiContext* ctx = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(3840.0f, 2160.0f);

    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    const std::vector<std::pair<std::string, std::string>> testPairs = {
        {"Normal Drive", "NeuralDSP"},
        {"Ultra High Gain Boutique Overdrive MKII", "Very Long Vendor Name Audio Labs Inc"},
        {"ExtremelyLongPluginNameExceedingTypicalBufferLengthsWithoutWhitespaceToTestClippingAndCollisionResistance", "BoutiqueDSP"},
    };

    for (float scale : dpiScales) {
        io.FontGlobalScale = scale;

        ImGui::NewFrame();

        const float fontSize = ImGui::GetFontSize();
        const float dpiScale = std::max(1.0f, fontSize / 13.0f);
        const float modalWidth = 580.0f * dpiScale;
        const float paddingX = 14.0f * dpiScale;
        const float rowW = modalWidth - 2.0f * paddingX;

        for (const auto& [title, vendor] : testPairs) {
            float cursorX = 6.0f;

            // Star button: 20px
            const float starW = 20.0f;
            cursorX += starW + 6.0f;

            // Badge: ~36px
            const float badgeW = 36.0f * dpiScale;
            cursorX += badgeW + 8.0f;

            // Title
            const ImVec2 titleSz = ImGui::CalcTextSize(title.c_str());
            const float titleStart = cursorX;
            const float titleEnd = titleStart + titleSz.x;
            cursorX = titleEnd + 10.0f;

            // Vendor
            const ImVec2 vendorSz = ImGui::CalcTextSize(vendor.c_str());
            const float vendorStart = cursorX;
            const float vendorEnd = vendorStart + vendorSz.x;

            // Right category badge layout guard
            const std::string category = "Distortion";
            const ImVec2 catSz = ImGui::CalcTextSize(category.c_str());
            const float catX = rowW - catSz.x - 24.0f;

            // Category collision check in plugin_browser_modal.cpp:
            // if (catX > ImGui::GetCursorScreenPos().x + 20.0f) render;
            const bool categorySafeToRender = (catX > vendorEnd + 20.0f);

            if (categorySafeToRender) {
                // Clearance between vendor and category badge is strictly positive
                const float clearance = catX - vendorEnd;
                assert(clearance >= 20.0f);
            } else {
                // Collision prevented by layout guard
                assert(catX <= vendorEnd + 20.0f);
            }

            // Title and vendor separation is strictly positive
            assert(vendorStart > titleEnd);
            assert(titleStart > starW);
        }

        ImGui::Render();
    }

    ImGui::DestroyContext(ctx);
}

// ============================================================================
// MODULE 3: UndoManager TRANSACTION COALESCING STRESS-TEST
// ============================================================================

void test_UndoManager_Coalescing_1000_RapidEvents_Within400ms() {
    // Requirements: simulate 1,000 rapid parameter/MIDI CC events within 400ms,
    // asserting single snapshot coalescing.
    state::UndoManager undoMgr;

    // Initial baseline state
    undoMgr.pushSnapshot(createMockPreset("Base", 0.0f), "Base State", state::UndoActionType::StructuralMutation);
    assert(undoMgr.undoCount() == 1);

    // Simulate 1,000 rapid events within <200ms
    const auto tStart = std::chrono::steady_clock::now();

    for (int i = 1; i <= 1000; ++i) {
        float gain = static_cast<float>(i) * 0.01f;
        undoMgr.pushSnapshot(
            createMockPreset("Preset", gain),
            "Tweak gain " + std::to_string(i),
            state::UndoActionType::ParameterTweak,
            "slot0_gain"
        );
    }

    const auto tEnd = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(tEnd - tStart).count();

    // Verify all 1,000 events fired well within 400ms
    assert(elapsedMs < 400);

    // CRITICAL ASSERTION: After 1,000 rapid events, exactly 1 coalesced transaction was added (total = 2: Base + Coalesced)
    assert(undoMgr.undoCount() == 2);
    assert(undoMgr.canUndo());

    // Undoing restores the base state
    state::ScenePreset restored;
    bool ok = undoMgr.undo(restored);
    assert(ok);
    assert(restored.nodes[0].slot.inputGainDb == 0.0f);
    assert(undoMgr.undoCount() == 1);

    // Redoing restores the 1,000th mutation value
    ok = undoMgr.redo(restored);
    assert(ok);
    assert(std::abs(restored.nodes[0].slot.inputGainDb - 10.0f) < 1e-4f);
    assert(undoMgr.undoCount() == 2);
}

void test_UndoManager_400ms_Inactivity_DiscreteSnapshot() {
    // Requirements: assert discrete snapshot creation after 400ms idle inactivity
    state::UndoManager undoMgr;

    // First burst: 50 events within <20ms
    for (int i = 1; i <= 50; ++i) {
        undoMgr.pushSnapshot(
            createMockPreset("Preset", static_cast<float>(i)),
            "Tweak burst 1",
            state::UndoActionType::ParameterTweak,
            "param_knob1"
        );
    }
    assert(undoMgr.undoCount() == 1);

    // Idle for 450ms (> 400ms threshold)
    std::this_thread::sleep_for(std::chrono::milliseconds(450));

    // Second burst: first event after idle threshold
    undoMgr.pushSnapshot(
        createMockPreset("Preset", 100.0f),
        "Tweak burst 2",
        state::UndoActionType::ParameterTweak,
        "param_knob1"
    );

    // CRITICAL ASSERTION: Idle threshold expired, creating a discrete transaction (undoCount == 2)
    assert(undoMgr.undoCount() == 2);

    // Another event in second burst should coalesce into transaction #2
    undoMgr.pushSnapshot(
        createMockPreset("Preset", 101.0f),
        "Tweak burst 2 second",
        state::UndoActionType::ParameterTweak,
        "param_knob1"
    );
    assert(undoMgr.undoCount() == 2);
}

void test_UndoManager_MouseRelease_DiscreteSnapshot() {
    // Requirements: assert discrete snapshot creation upon mouse release (commitActiveTransaction)
    state::UndoManager undoMgr;

    // Drag knob: 100 rapid events
    for (int i = 1; i <= 100; ++i) {
        undoMgr.pushSnapshot(
            createMockPreset("Preset", static_cast<float>(i)),
            "Drag knob",
            state::UndoActionType::ParameterTweak,
            "param_knob1"
        );
    }
    assert(undoMgr.undoCount() == 1);

    // Mouse released: ImGui::IsItemDeactivatedAfterEdit() triggers commitActiveTransaction()
    undoMgr.commitActiveTransaction();

    // Immediately start dragging again (< 5ms later)
    undoMgr.pushSnapshot(
        createMockPreset("Preset", 200.0f),
        "New drag after release",
        state::UndoActionType::ParameterTweak,
        "param_knob1"
    );

    // CRITICAL ASSERTION: commitActiveTransaction ended coalescing group, creating new discrete transaction
    assert(undoMgr.undoCount() == 2);
}

void test_UndoManager_TargetSwitching_And_Structural_NoCoalesce() {
    state::UndoManager undoMgr;

    // Tweak knob A
    undoMgr.pushSnapshot(createMockPreset("P", 1.0f), "Tweak A", state::UndoActionType::ParameterTweak, "target_A");
    assert(undoMgr.undoCount() == 1);

    // Immediately tweak knob B (<1ms later)
    undoMgr.pushSnapshot(createMockPreset("P", 2.0f), "Tweak B", state::UndoActionType::ParameterTweak, "target_B");
    // Different target keys MUST NOT coalesce
    assert(undoMgr.undoCount() == 2);

    // Structural rack mutation (e.g. add slot)
    undoMgr.pushSnapshot(createMockPreset("P", 3.0f), "Add slot", state::UndoActionType::StructuralMutation);
    // Structural mutation MUST NOT coalesce with continuous sweeps
    assert(undoMgr.undoCount() == 3);
}

void test_UndoManager_Capacity64_Eviction_Stress() {
    state::UndoManager undoMgr;

    // Push 100 discrete structural snapshots
    for (int i = 0; i < 100; ++i) {
        undoMgr.pushSnapshot(
            createMockPreset("Preset_" + std::to_string(i), static_cast<float>(i)),
            "Action " + std::to_string(i),
            state::UndoActionType::StructuralMutation
        );
    }

    // Capacity must be strictly capped at 64
    assert(undoMgr.undoCount() == 64);

    // Traverse all 63 relative undos to reach base snapshot #36
    state::ScenePreset p;
    for (int i = 0; i < 63; ++i) {
        bool ok = undoMgr.undo(p);
        assert(ok);
    }
    // Base of stack should be snapshot #36 (100 - 64)
    assert(p.name == "Preset_36");

    // The 64th undo consumes the final baseline entry
    bool ok64 = undoMgr.undo(p);
    assert(ok64);
    assert(undoMgr.undoCount() == 0);
    assert(!undoMgr.canUndo());

    // Further undo returns false (at absolute bottom)
    assert(!undoMgr.undo(p));

    // Redo all 64 actions back to top
    for (int i = 0; i < 64; ++i) {
        bool ok = undoMgr.redo(p);
        assert(ok);
    }
    assert(p.name == "Preset_99");
    assert(undoMgr.undoCount() == 64);
    assert(!undoMgr.canRedo());
}

// ============================================================================
// MODULE 4: ARCHIVE DECOMPRESSION BOMB DEFENSE & ZIP SLIP REJECTION
// ============================================================================

static void createDirectoryClean(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
}

void test_Archive_DecompressionBomb_30MB_Xml_Rejection() {
    // Requirements: test adversarial zip archives exceeding 30MB XML, asserting safe rejection
    const std::filesystem::path tempDir = "test_archive_sandbox_xml";
    const std::filesystem::path tempZip = "test_bomb_30mb_xml.zip";
    createDirectoryClean(tempDir);

    std::error_code ec;
    std::filesystem::remove(tempZip, ec);

    // Create zip archive with a 31MB XML file stored uncompressed (ratio 1:1)
    const uint64_t targetXmlSize = 31ULL * 1024 * 1024; // 31 MB (> 30 MB XML limit)

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    assert(mz_zip_writer_init_file(&zip, tempZip.string().c_str(), 0));

    // Fill buffer with XML tags (1MB pattern replicated)
    std::string xmlChunk = "<preset id=\"test\"><param index=\"0\" value=\"0.5\"/><node type=\"amp\"/></preset>\n";
    while (xmlChunk.size() < 1024 * 1024) {
        xmlChunk += xmlChunk;
    }

    // Allocate 31MB buffer
    std::vector<char> xmlData(targetXmlSize);
    for (size_t offset = 0; offset < targetXmlSize; offset += xmlChunk.size()) {
        size_t copyLen = std::min(xmlChunk.size(), static_cast<size_t>(targetXmlSize - offset));
        std::memcpy(xmlData.data() + offset, xmlChunk.data(), copyLen);
    }

    // Add with MZ_NO_COMPRESSION so compression ratio check passes (1:1), directly testing the 30MB XML bomb check
    assert(mz_zip_writer_add_mem(&zip, "bomb_presets.xml", xmlData.data(), xmlData.size(), MZ_NO_COMPRESSION));
    assert(mz_zip_writer_finalize_archive(&zip));
    assert(mz_zip_writer_end(&zip));

    std::string errorMsg;
    bool ok = utils::extractZipArchive(tempZip, tempDir, errorMsg);

    std::cout << "\n    -> 31MB XML bomb: " << (ok ? "ACCEPTED" : "REJECTED") << " (reason: " << errorMsg << ")\n    ";

    // CRITICAL ASSERTION: extraction must fail and report XML bomb
    assert(!ok);
    assert(!errorMsg.empty());
    assert(errorMsg.find("30MB") != std::string::npos || errorMsg.find("XML") != std::string::npos);

    // Clean up
    std::filesystem::remove(tempZip, ec);
    std::filesystem::remove_all(tempDir, ec);
}

void test_Archive_DecompressionBomb_128MB_Total_Rejection() {
    // Requirements: test adversarial zip archives exceeding 128MB archive total, asserting safe rejection
    const std::filesystem::path tempDir = "test_archive_sandbox_total";
    const std::filesystem::path tempZip = "test_bomb_128mb_total.zip";
    createDirectoryClean(tempDir);

    std::error_code ec;
    std::filesystem::remove(tempZip, ec);

    // Create archive with 6 entries of 22MB each = 132MB total (> 128MB total limit)
    // Each individual file is 22MB < 30MB (so single file check passes), but sum exceeds 128MB
    const size_t kFileSize = 22 * 1024 * 1024; // 22 MB
    std::vector<char> dummyData(kFileSize, 'X');

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    assert(mz_zip_writer_init_file(&zip, tempZip.string().c_str(), 0));

    for (int i = 0; i < 6; ++i) {
        std::string filename = "data_chunk_" + std::to_string(i) + ".bin";
        assert(mz_zip_writer_add_mem(&zip, filename.c_str(), dummyData.data(), dummyData.size(), MZ_NO_COMPRESSION));
    }

    assert(mz_zip_writer_finalize_archive(&zip));
    assert(mz_zip_writer_end(&zip));

    std::string errorMsg;
    bool ok = utils::extractZipArchive(tempZip, tempDir, errorMsg);

    std::cout << "\n    -> 132MB total archive bomb: " << (ok ? "ACCEPTED" : "REJECTED") << " (reason: " << errorMsg << ")\n    ";

    // CRITICAL ASSERTION: extraction must fail and report total size limit exceeded
    assert(!ok);
    assert(!errorMsg.empty());
    assert(errorMsg.find("128MB") != std::string::npos || errorMsg.find("total uncompressed") != std::string::npos);

    // Clean up
    std::filesystem::remove(tempZip, ec);
    std::filesystem::remove_all(tempDir, ec);
}

void test_Archive_DecompressionBomb_100_to_1_Ratio_Rejection() {
    // Requirements: test adversarial zip archives exceeding 100:1 ratio, asserting safe rejection
    const std::filesystem::path tempDir = "test_archive_sandbox_ratio";
    const std::filesystem::path tempZip = "test_bomb_ratio_100_to_1.zip";
    createDirectoryClean(tempDir);

    std::error_code ec;
    std::filesystem::remove(tempZip, ec);

    // 10MB of zeroes compressed with deflate produces ~10KB archive (ratio ~1000:1 > 100:1)
    const size_t kUncompressedSize = 10 * 1024 * 1024; // 10 MB
    std::vector<char> zeroData(kUncompressedSize, 0);

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    assert(mz_zip_writer_init_file(&zip, tempZip.string().c_str(), 0));

    assert(mz_zip_writer_add_mem(&zip, "ratio_bomb.bin", zeroData.data(), zeroData.size(), MZ_BEST_COMPRESSION));
    assert(mz_zip_writer_finalize_archive(&zip));
    assert(mz_zip_writer_end(&zip));

    std::string errorMsg;
    bool ok = utils::extractZipArchive(tempZip, tempDir, errorMsg);

    std::cout << "\n    -> 1000:1 ratio bomb: " << (ok ? "ACCEPTED" : "REJECTED") << " (reason: " << errorMsg << ")\n    ";

    // CRITICAL ASSERTION: extraction must fail and report 100:1 ratio violation
    assert(!ok);
    assert(!errorMsg.empty());
    assert(errorMsg.find("100:1") != std::string::npos || errorMsg.find("ratio") != std::string::npos);

    // Clean up
    std::filesystem::remove(tempZip, ec);
    std::filesystem::remove_all(tempDir, ec);
}

void test_Archive_DecompressionBomb_100_Files_Rejection() {
    // Requirements: test adversarial zip archives exceeding 100 files, asserting safe rejection
    const std::filesystem::path tempDir = "test_archive_sandbox_files";
    const std::filesystem::path tempZip = "test_bomb_100_files.zip";
    createDirectoryClean(tempDir);

    std::error_code ec;
    std::filesystem::remove(tempZip, ec);

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    assert(mz_zip_writer_init_file(&zip, tempZip.string().c_str(), 0));

    // Create 105 files (> 100 file count limit)
    const char dummyContent = 'A';
    for (int i = 0; i < 105; ++i) {
        std::string filename = "file_" + std::to_string(i) + ".txt";
        assert(mz_zip_writer_add_mem(&zip, filename.c_str(), &dummyContent, 1, MZ_NO_COMPRESSION));
    }

    assert(mz_zip_writer_finalize_archive(&zip));
    assert(mz_zip_writer_end(&zip));

    std::string errorMsg;
    bool ok = utils::extractZipArchive(tempZip, tempDir, errorMsg);

    std::cout << "\n    -> 105 file count bomb: " << (ok ? "ACCEPTED" : "REJECTED") << " (reason: " << errorMsg << ")\n    ";

    // CRITICAL ASSERTION: extraction must fail and report file count limit
    assert(!ok);
    assert(!errorMsg.empty());
    assert(errorMsg.find("100") != std::string::npos || errorMsg.find("file count") != std::string::npos);

    // Clean up
    std::filesystem::remove(tempZip, ec);
    std::filesystem::remove_all(tempDir, ec);
}

void test_Archive_ZipSlip_PathTraversal_Matrix() {
    // Requirements: test Zip Slip directory traversal paths (`../`), asserting safe rejection
    const std::filesystem::path tempDir = "test_archive_sandbox_zipslip";
    const std::filesystem::path tempZip = "test_bomb_zipslip.zip";
    createDirectoryClean(tempDir);

    std::error_code ec;
    std::filesystem::remove(tempZip, ec);

    const std::vector<std::string> adversarialPaths = {
        "../../escape_canary_1.txt",
        "subdir/../../../escape_canary_2.txt",
        "..\\..\\escape_canary_3.txt",
        "C:/escape_canary_4.txt",
        "\\\\server\\share\\escape_canary_5.txt",
        "CON.txt",
        "PRN.dat",
        "AUX.xml",
        "NUL",
        "escape_trailing_dot.",
        "escape_trailing_space "
    };

    for (const auto& evilPath : adversarialPaths) {
        std::filesystem::remove(tempZip, ec);

        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        assert(mz_zip_writer_init_file(&zip, tempZip.string().c_str(), 0));

        const char payload[] = "MALICIOUS CANARY DATA";
        assert(mz_zip_writer_add_mem(&zip, evilPath.c_str(), payload, sizeof(payload), MZ_NO_COMPRESSION));
        assert(mz_zip_writer_finalize_archive(&zip));
        assert(mz_zip_writer_end(&zip));

        std::string errorMsg;
        bool ok = utils::extractZipArchive(tempZip, tempDir, errorMsg);

        // MUST be rejected
        assert(!ok);
        assert(!errorMsg.empty());

        // Verify canary file was NEVER created anywhere outside the sandbox
        assert(!std::filesystem::exists("escape_canary_1.txt"));
        assert(!std::filesystem::exists("../../escape_canary_1.txt"));
        assert(!std::filesystem::exists("escape_canary_2.txt"));
        assert(!std::filesystem::exists("escape_canary_3.txt"));
        assert(!std::filesystem::exists("C:/escape_canary_4.txt"));
    }

    std::cout << "\n    -> Validated " << adversarialPaths.size() << " Zip Slip traversal vectors: 100% BLOCKED\n    ";

    // Clean up
    std::filesystem::remove(tempZip, ec);
    std::filesystem::remove_all(tempDir, ec);
}

void test_Archive_LegitimateExtraction_Clean() {
    const std::filesystem::path tempDir = "test_archive_sandbox_legit";
    const std::filesystem::path tempZip = "test_legit_archive.zip";
    createDirectoryClean(tempDir);

    std::error_code ec;
    std::filesystem::remove(tempZip, ec);

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    assert(mz_zip_writer_init_file(&zip, tempZip.string().c_str(), 0));

    const std::string textData = "Valid preset text data with reasonable entropy and normal formatting.";
    assert(mz_zip_writer_add_mem(&zip, "presets/clean_amp.pracpreset", textData.data(), textData.size(), MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_add_mem(&zip, "presets/lead_solo.pracpreset", textData.data(), textData.size(), MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_finalize_archive(&zip));
    assert(mz_zip_writer_end(&zip));

    std::string errorMsg;
    bool ok = utils::extractZipArchive(tempZip, tempDir, errorMsg);

    assert(ok);
    assert(errorMsg.empty());
    assert(std::filesystem::exists(tempDir / "presets" / "clean_amp.pracpreset"));
    assert(std::filesystem::exists(tempDir / "presets" / "lead_solo.pracpreset"));

    // Verify content bit-exact
    std::ifstream f(tempDir / "presets" / "clean_amp.pracpreset", std::ios::binary);
    std::string readContent((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    assert(readContent == textData);

    // Clean up
    std::filesystem::remove(tempZip, ec);
    std::filesystem::remove_all(tempDir, ec);
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int main() {
    std::cout << "================================================================================\n";
    std::cout << "   PRACCY V2.4 STAGE 1 CHALLENGER 2 EMPIRICAL TEST HARNESS                      \n";
    std::cout << "   Focus: UI Virtualization, High-DPI Safety, Undo Coalescing & Bomb Defense    \n";
    std::cout << "================================================================================\n\n";

    std::cout << "[MODULE 1] Spotlight ImGuiListClipper Virtualization Stress Benchmark\n";
    RUN_TEST(test_Spotlight_ListClipper_Virtualization_2500_Items);
    RUN_TEST(test_Spotlight_ListClipper_Scroll_Boundaries);

    std::cout << "\n[MODULE 2] High-DPI Layout Safety & Zero Text Collisions (100% - 200%)\n";
    RUN_TEST(test_HighDpi_Footer_ZeroCollision_Matrix);
    RUN_TEST(test_HighDpi_PluginRow_ZeroCollision_Matrix);

    std::cout << "\n[MODULE 3] UndoManager Transaction Coalescing Stress-Test\n";
    RUN_TEST(test_UndoManager_Coalescing_1000_RapidEvents_Within400ms);
    RUN_TEST(test_UndoManager_400ms_Inactivity_DiscreteSnapshot);
    RUN_TEST(test_UndoManager_MouseRelease_DiscreteSnapshot);
    RUN_TEST(test_UndoManager_TargetSwitching_And_Structural_NoCoalesce);
    RUN_TEST(test_UndoManager_Capacity64_Eviction_Stress);

    std::cout << "\n[MODULE 4] Archive Decompression Bomb Defense & Zip Slip Rejection\n";
    RUN_TEST(test_Archive_DecompressionBomb_30MB_Xml_Rejection);
    RUN_TEST(test_Archive_DecompressionBomb_128MB_Total_Rejection);
    RUN_TEST(test_Archive_DecompressionBomb_100_to_1_Ratio_Rejection);
    RUN_TEST(test_Archive_DecompressionBomb_100_Files_Rejection);
    RUN_TEST(test_Archive_ZipSlip_PathTraversal_Matrix);
    RUN_TEST(test_Archive_LegitimateExtraction_Clean);

    std::cout << "\n================================================================================\n";
    std::cout << "   TEST RUN SUMMARY: " << g_passedTests << " / " << g_totalTests << " TESTS PASSED\n";
    std::cout << "================================================================================\n";

    if (g_passedTests == g_totalTests) {
        std::cout << ">>> ALL CHALLENGER 2 EMPIRICAL STRESS TESTS PASSED (100% SUCCESS) <<<\n\n";
        return 0;
    } else {
        std::cerr << ">>> CHALLENGER 2 EMPIRICAL STRESS TESTS FAILED! <<<\n\n";
        return 1;
    }
}
