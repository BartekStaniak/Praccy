#pragma once

#include "../audio/graph_engine.h"
#include "../audio/asio_manager.h"
#include "../tools/tuner.h"
#include "../tools/metronome.h"
#include "../midi/midi_manager.h"
#include "../state/scene_manager.h"
#include "../plugins/plugin_scanner.h"
#include "../tools/audio_player.h"
#include "../tools/quick_looper.h"
#include "modals/plugin_browser_modal.h"
#include "modals/settings_modal.h"
#include "modals/practice_tools_modal.h"
#include "modals/tuner_settings_modal.h"
#include "modals/export_daw_chain_modal.h"
#include "../state/undo_manager.h"
#include "../audio/retroactive_buffer.h"
#include "../state/app_config.h"
#include <vector>
#include <string>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <array>

namespace praccy::ui {

class ThumbnailManager;

struct CardDragPayload {
    int sourceIndex{-1};
    int sourceBlockIndex{-1};
    int sourceBranchIndex{-1};
};

using DeviceMacro = audio::PluginSlot::Macro;

class RackView {
public:
    RackView(audio::GraphEngine& graph,
             audio::AsioManager& asio,
             tools::InstrumentTuner& tuner,
             tools::Metronome& metronome,
             tools::AudioPlayer& player,
             tools::QuickLooper& looper,
             midi::MidiManager& midi,
             state::SceneManager& scenes,
             plugins::PluginScanner& scanner);

    void render();
    void setDspStats(std::atomic<float>* load, std::atomic<uint32_t>* dropouts) {
        m_dspLoadPercent = load;
        m_dspDropouts = dropouts;
        m_displayedDspLoad = 0.0f;
        m_lastDspLoadUpdateTime = {};
    }
    [[nodiscard]] float displayedDspLoad() const noexcept { return m_displayedDspLoad; }
    void resetDspDropouts() {
        if (m_dspDropouts) {
            m_dspDropouts->store(0, std::memory_order_relaxed);
        }
    }

    void triggerHudToast(std::string message);

    audio::RetroactiveRiffBuffer& getRiffBuffer() noexcept { return m_riffBuffer; }
    const audio::RetroactiveRiffBuffer& getRiffBuffer() const noexcept { return m_riffBuffer; }
    void saveLastRiff();
    [[nodiscard]] static std::string formatRiffFilename(const std::string& presetName, float bpm, const std::tm& tmNow) {
        return audio::RetroactiveRiffBuffer::formatFilename(presetName, bpm, tmNow);
    }

    state::AppConfig::RackViewMode getRackViewMode() const noexcept { return m_rackViewMode; }
    void setRackViewMode(state::AppConfig::RackViewMode mode) noexcept { m_rackViewMode = mode; }

    void applyAudioSettings(double sampleRate, uint32_t bufferSize);

    void recallScene(size_t index);
    bool loadPresetChain(const std::string& name);
    const std::string& activePreset() const noexcept { return m_auditionedPreset; }
    [[nodiscard]] const std::vector<std::string>& favoritePresets() const noexcept { return m_favoritePresets; }

private:
    bool renderStatusPill(bool isRunning, const char* statusText, float pillWidth = 0.0f);
    void renderPracticeRibbon();
    void renderSignalRack();
    void renderInputCard(float cardY = -1.0f, float cardH = 224.0f);
    void renderPluginSlot(audio::PluginSlot* slot, int slotIndex, int blockIndex = -1, int branchIndex = -1, float cardHeight = 224.0f);
    void renderParallelBlock(audio::ParallelSplitMergeBlock* block, int blockIndex, float centerY);
    void renderSceneBar();
    void renderBottomBar();
    void renderMeter(const char* label, float level, float width, float height);
    void renderMeter(const char* id, float level, float holdLevel, bool isClipped, float width, float height, bool* outClicked = nullptr);
    void renderDspTweakModal();
    void renderUpdateModal();
    void renderFloatingHudToast();
    void renderPresetBrowserModal();
    void renderHotkeyLegendOverlay();

    void cycleAdjacentScenePreset(int delta);
    void dispatchKeyBindings();
    void processKeyMapCapture();
    int getBoundKeyForTarget(int slotIndex, int blockIndex, int branchIndex, int branchSlotIndex, uint32_t targetType, uint32_t paramId) const;
    void initMacrosForSlot(audio::PluginSlot* slot);
    void syncMacrosFromGraph();
    void refreshGuiParamBaseline(audio::PluginSlot* slot);
    void pollPluginGuiEdits(bool forceFlushPending = false);
    void moveSlot(int srcIdx, int srcBlockIdx, int srcBranchIdx, int dstIdx, int dstBlockIdx, int dstBranchIdx);

    audio::GraphEngine& m_graph;
    audio::AsioManager& m_asio;
    tools::InstrumentTuner& m_tuner;
    tools::Metronome& m_metronome;
    tools::AudioPlayer& m_player;
    tools::QuickLooper& m_looper;
    midi::MidiManager& m_midi;
    state::SceneManager& m_scenes;
    plugins::PluginScanner& m_scanner;

    std::unique_ptr<PluginBrowserModal> m_pluginBrowserModal;
    std::unique_ptr<SettingsModal> m_settingsModal;
    std::unique_ptr<PracticeToolsModal> m_practiceToolsModal;
    std::unique_ptr<TunerSettingsModal> m_tunerSettingsModal;
    ExportDawChainModal m_exportDawModal;

    std::atomic<float>* m_dspLoadPercent{nullptr};
    std::atomic<uint32_t>* m_dspDropouts{nullptr};
    float m_displayedDspLoad{0.0f};
    std::chrono::steady_clock::time_point m_lastDspLoadUpdateTime{};

    std::vector<std::chrono::steady_clock::time_point> m_tapTimes;
    std::chrono::steady_clock::time_point m_lastTapFlash{};
    bool m_tempoEditing{false};
    int m_tempoEditValue{120};

    std::unordered_map<uint64_t, bool> m_branchMinimized;
    std::vector<std::string> m_cachedUserPresetNames;
    bool m_userPresetsValid{false};

    bool m_showSavePresetModal{false};
    char m_presetNameBuffer[64]{0};

    bool m_showPresetBrowserModal{false};
    char m_presetSearchFilter[64]{0};
    int m_selectedCategoryTagIndex{0}; // 0: All, 1: High Gain Metal, 2: Glassy Clean, 3: Ambient FX, 4: Edge of Breakup, 5: Favorites
    bool isPresetFavorite(const std::string& name) const;
    void toggleFavoritePreset(const std::string& name);
    std::vector<std::string> m_favoritePresets;
    std::string m_auditionedPreset;

    std::string m_hudToastText;
    float m_hudToastTimer{0.0f};
    static constexpr float kHudToastDuration = 1.8f;

    audio::PluginSlot* m_dspTweakSlot{nullptr};
    audio::PluginSlot* m_expandedSlot{nullptr};
    float m_expandPulseTimer{0.0f};

    bool m_showUpdateModal{false};
    state::AppConfig m_cachedConfig;
    state::AppConfig& m_config{m_cachedConfig};
    ThumbnailManager* m_thumbnailMgr{nullptr};

    state::UndoManager m_undoManager;
    audio::RetroactiveRiffBuffer m_riffBuffer;
    std::unordered_map<audio::PluginSlot*, std::array<DeviceMacro, 4>> m_slotMacros;
    std::unordered_map<audio::PluginSlot*, std::vector<float>> m_guiParamBaseline;
    std::chrono::steady_clock::time_point m_lastGuiParamPollTime{};
    audio::PluginSlot* m_pendingGuiEditSlot{nullptr};
    std::string m_pendingGuiEditTargetKey;
    std::string m_pendingGuiEditDesc;
    bool m_guiEditWasMouseDown{false};

    struct AwaitingKeyTarget {
        bool active{false};
        int slotIndex{-1};
        int blockIndex{-1};
        int branchIndex{-1};
        int branchSlotIndex{-1};
        int macroIndex{-1};
        uint32_t targetType{0};
        uint32_t paramId{0};
        std::string targetLabel;
    };
    AwaitingKeyTarget m_awaitingKeyTarget;

    bool m_midiMapMode{false};
    bool m_keyMapMode{false};
    bool m_showHotkeyLegend{false};
    audio::PluginSlot* m_focusedSlot{nullptr};
    int m_focusedSlotIndex{-1};
    int m_focusedBlockIndex{-1};
    int m_focusedBranchIndex{-1};
    state::AppConfig::RackViewMode m_rackViewMode{state::AppConfig::RackViewMode::Modular};
};

} // namespace praccy::ui
