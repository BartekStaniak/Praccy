#pragma once

#include <windows.h>
#include <shellapi.h>
#include <filesystem>
#include <vector>
#include <string>
#include <functional>

namespace praccy::audio { class GraphEngine; }
namespace praccy::tools { class AudioPlayer; class QuickLooper; }
namespace praccy::state { class SceneManager; }

namespace praccy::ui {

enum class DroppedFileType {
    AudioTrack,  // .wav, .wave, .flac, .mp3, .ogg, .aiff, .aif
    Preset,      // .ini, .pracpreset, .preset, .vstpreset, .clappreset
    SignalChain, // .chain, .pracchaim, .rack
    Chain = SignalChain,
    Unknown
};

using DropType = DroppedFileType;

struct ClassifiedDropItem {
    std::filesystem::path path;
    DroppedFileType type{DroppedFileType::Unknown};
};

struct DropContext {
    tools::AudioPlayer* player{nullptr};
    tools::QuickLooper* looper{nullptr};
    state::SceneManager* scenes{nullptr};
    audio::GraphEngine* graph{nullptr};
    bool preferLooper{false};
};

using DragDropContext = DropContext;

class DragDropRouter {
public:
    DragDropRouter() = default;

    /// File classification helpers
    [[nodiscard]] static DroppedFileType classifyFile(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static DropType classifyPath(const std::string& path) noexcept;
    [[nodiscard]] static bool isAudioTrack(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static bool isPreset(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static bool isChain(const std::filesystem::path& path) noexcept;
    [[nodiscard]] static bool isSignalChain(const std::filesystem::path& path) noexcept { return isChain(path); }

    /// Extracts and classifies file paths from a Win32 HDROP handle
    [[nodiscard]] static std::vector<ClassifiedDropItem> parseHDrop(HDROP hDrop);

    /// Routing callbacks
    using AudioTrackHandler = std::function<void(const std::filesystem::path&)>;
    using PresetHandler     = std::function<void(const std::filesystem::path&)>;
    using ChainHandler      = std::function<void(const std::filesystem::path&)>;

    void setAudioTrackHandler(AudioTrackHandler handler) { m_audioHandler = std::move(handler); }
    void setPresetHandler(PresetHandler handler)         { m_presetHandler = std::move(handler); }
    void setChainHandler(ChainHandler handler)           { m_chainHandler = std::move(handler); }
    void setSignalChainHandler(ChainHandler handler)     { m_chainHandler = std::move(handler); }

    /// Routes parsed drop items to registered handlers
    void route(const std::vector<ClassifiedDropItem>& items) const;

    /// Direct entry point from WndProc WM_DROPFILES
    void handleDropMessage(HDROP hDrop) const;

    /// Direct single-file dispatch given a DropContext
    bool handleDrop(const std::string& path, DropContext& ctx) const;

private:
    AudioTrackHandler m_audioHandler;
    PresetHandler     m_presetHandler;
    ChainHandler      m_chainHandler;
};

} // namespace praccy::ui
