#include "drag_drop_router.h"
#include "../tools/audio_player.h"
#include "../tools/quick_looper.h"
#include "../state/scene_manager.h"
#include "../state/daw_interop/universal_daw_interop_manager.h"
#include "../plugins/plugin_scanner.h"
#include "../audio/graph_engine.h"

#include <algorithm>
#include <cctype>

namespace praccy::ui {

DroppedFileType DragDropRouter::classifyFile(const std::filesystem::path& path) noexcept {
    if (!path.has_extension()) {
        return DroppedFileType::Unknown;
    }

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(::tolower(c));
    });

    // Audio tracks (strictly .wav - WavReader only supports RIFF WAVE PCM/Float)
    if (ext == ".wav") {
        return DroppedFileType::AudioTrack;
    }

    // Presets
    if (ext == ".pracpreset" || ext == ".ini" || ext == ".preset" ||
        ext == ".vstpreset" || ext == ".clappreset") {
        return DroppedFileType::Preset;
    }

    // Signal chains
    if (ext == ".praccy" || ext == ".pracchain" || ext == ".pracchaim" || ext == ".chain" || ext == ".rack" ||
        ext == ".rfxchain" || ext == ".adg" || ext == ".dawproject") {
        return DroppedFileType::SignalChain;
    }

    return DroppedFileType::Unknown;
}

DropType DragDropRouter::classifyPath(const std::string& path) noexcept {
    return classifyFile(std::filesystem::path(path));
}

bool DragDropRouter::isAudioTrack(const std::filesystem::path& path) noexcept {
    return classifyFile(path) == DroppedFileType::AudioTrack;
}

bool DragDropRouter::isPreset(const std::filesystem::path& path) noexcept {
    return classifyFile(path) == DroppedFileType::Preset;
}

bool DragDropRouter::isChain(const std::filesystem::path& path) noexcept {
    return classifyFile(path) == DroppedFileType::SignalChain;
}

std::vector<ClassifiedDropItem> DragDropRouter::parseHDrop(HDROP hDrop) {
    std::vector<ClassifiedDropItem> items;
    if (!hDrop) return items;

    const UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
    items.reserve(fileCount);

    for (UINT i = 0; i < fileCount; ++i) {
        const UINT pathLen = DragQueryFileW(hDrop, i, nullptr, 0);
        if (pathLen > 0) {
            std::wstring filePathW(pathLen + 1, L'\0');
            DragQueryFileW(hDrop, i, filePathW.data(), pathLen + 1);
            filePathW.resize(pathLen);

            std::filesystem::path p(filePathW);
            DroppedFileType type = classifyFile(p);
            items.push_back(ClassifiedDropItem{
                .path = std::move(p),
                .type = type
            });
        }
    }

    return items;
}

void DragDropRouter::route(const std::vector<ClassifiedDropItem>& items) const {
    for (const auto& item : items) {
        switch (item.type) {
            case DroppedFileType::AudioTrack:
                if (m_audioHandler) {
                    m_audioHandler(item.path);
                }
                break;
            case DroppedFileType::Preset:
                if (m_presetHandler) {
                    m_presetHandler(item.path);
                }
                break;
            case DroppedFileType::SignalChain:
                if (m_chainHandler) {
                    m_chainHandler(item.path);
                }
                break;
            case DroppedFileType::Unknown:
            default:
                break;
        }
    }
}

void DragDropRouter::handleDropMessage(HDROP hDrop) const {
    auto items = parseHDrop(hDrop);
    route(items);
}

bool DragDropRouter::handleDrop(const std::string& path, DropContext& ctx) const {
    const auto type = classifyPath(path);
    switch (type) {
        case DroppedFileType::AudioTrack:
            if (ctx.preferLooper) {
                if (ctx.looper) {
                    ctx.looper->loadWavFile(path);
                }
            } else {
                if (ctx.player) {
                    ctx.player->loadWavFile(path);
                }
            }
            return true;
        case DroppedFileType::Preset:
            if (ctx.scenes) {
                return ctx.scenes->loadFromFile(path);
            }
            return false;
        case DroppedFileType::SignalChain:
            if (ctx.scenes && ctx.graph) {
                std::filesystem::path p(path);
                auto* dawAdapter = state::UniversalDawInteropManager::instance().findAdapter(p);
                if (dawAdapter) {
                    plugins::PluginScanner scanner;
                    scanner.scanSync();
                    return state::UniversalDawInteropManager::instance().importAndApply(p, *ctx.graph, *ctx.scenes, scanner);
                } else {
                    ctx.scenes->loadPresetChain(p.stem().string(), *ctx.graph);
                    return true;
                }
            }
            return false;
        case DroppedFileType::Unknown:
        default:
            return false;
    }
}

} // namespace praccy::ui
