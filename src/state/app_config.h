#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragma-once-outside-header"
#endif
#pragma once
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "../audio/input_config.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace praccy::state {

struct ScannedPluginEntry {
    std::string name;
    std::string path;
    std::string type; // "VST3", "CLAP", "Built-In"
    std::string vendor{"Unknown"};
    std::string category{"Effects"};
    std::string bundleFileName;
    std::string classGuid;
    std::string clapId;

    bool operator==(const ScannedPluginEntry& other) const {
        return name == other.name && path == other.path && type == other.type &&
               vendor == other.vendor && category == other.category &&
               bundleFileName == other.bundleFileName && classGuid == other.classGuid &&
               clapId == other.clapId;
    }
};

struct KeyBindingEntry {
    int key{0};
    int slotIndex{-1};
    int branchIndex{-1};
    int branchSlotIndex{-1};
    uint32_t targetType{0};
    uint32_t paramId{0};

    bool operator==(const KeyBindingEntry& other) const {
        return key == other.key &&
               slotIndex == other.slotIndex &&
               branchIndex == other.branchIndex &&
               branchSlotIndex == other.branchSlotIndex &&
               targetType == other.targetType &&
               paramId == other.paramId;
    }
};

struct AppConfig {
    using KeyBindingEntry = praccy::state::KeyBindingEntry;

    std::string lastAsioDriver;
    audio::InputRoutingMode inputMode{audio::InputRoutingMode::MonoLeft};
    float inputGainDb{0.0f};
    float masterVolumeDb{0.0f};
    float metronomeBpm{120.0f};
    bool checkBetaUpdates{false};
    bool enableLiveThumbnails{false};
    std::vector<std::string> customPluginPaths;
    std::vector<std::string> favoritePlugins;
    std::vector<std::string> favoritePresets;
    bool presetFavoritesInitialized{false};
    std::string lastLoadedPreset;
    std::unordered_map<std::string, std::string> customPluginCategories;
    std::vector<std::string> recentPlugins;
    std::vector<ScannedPluginEntry> scannedPlugins;
    std::vector<KeyBindingEntry> keyBindings;

    uint32_t retroactiveBufferMinutes{2};
    std::string riffDirectory;

    enum class RackViewMode {
        Simple = 0,
        Modular = 1
    };
    RackViewMode rackViewMode{RackViewMode::Modular};

    int themeId{0}; // 0: ObsidianStudio, 1: CyberMidnight, 2: NordicSlate, 3: VintageConsole

    // Tuner Settings Persistence
    bool tunerEnabled{true};
    float tunerA4Reference{440.0f};
    int tunerInstrument{0};     // 0: Guitar, 1: Bass, etc.
    int tunerStringCount{6};
    int tunerTuningMode{0};      // 0: Preset, 1: Custom
    int tunerTuningType{0};      // 0: Standard, 1: Drop, etc.
    int tunerRootNote{4};        // 4: E
    std::string tunerCustomStrings;

    // Window geometry persistence
    int windowX{100};
    int windowY{100};
    int windowW{1280};
    int windowH{720};
    bool windowMaximized{false};

    static std::string getConfigDir();
    static std::string getConfigFilePath();
    bool load(const std::string& customPath = "");
    bool save(const std::string& customPath = "") const;

#if defined(_WIN32)
    void saveWindowPlacement(HWND hwnd);
    bool restoreWindowPlacement(HWND hwnd) const;
#endif
};

} // namespace praccy::state
