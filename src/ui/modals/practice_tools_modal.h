#pragma once

#include <string>
#include "../../tools/quick_looper.h"
#include "../../tools/audio_player.h"

namespace praccy::ui {

class PracticeToolsModal {
public:
    PracticeToolsModal(tools::QuickLooper& looper, tools::AudioPlayer& player);
    ~PracticeToolsModal();

    PracticeToolsModal(const PracticeToolsModal&) = delete;
    PracticeToolsModal& operator=(const PracticeToolsModal&) = delete;

    /// Open the practice tools modal
    void open();

    /// Close the practice tools modal
    void close();

    /// Toggle open/closed state
    void toggle() {
        if (m_isOpen) close();
        else open();
    }

    /// Check if modal is currently open
    [[nodiscard]] bool isOpen() const noexcept { return m_isOpen; }

    /// Render frame routine (invoked in RackView::render())
    void render();

private:
    struct WaveformPeak {
        float minVal{-0.05f};
        float maxVal{0.05f};
    };

    void renderLooperTab();
    void renderBackingTrackTab();
    void renderLooperCircularProgressRing();
    void generateWaveform800(const std::string& wavPath);
    void renderOverviewWaveformStrip(float stripWidth, float stripHeight);

    tools::QuickLooper& m_looper;
    tools::AudioPlayer& m_player;

    bool m_isOpen{false};
    char m_audioFilePathBuffer[260]{0};

    std::vector<WaveformPeak> m_waveform800;
    std::string m_lastLoadedTrack;
    float m_loopStart{0.0f};
    float m_loopEnd{1.0f};
    bool m_isDraggingLoopStart{false};
    bool m_isDraggingLoopEnd{false};
};

} // namespace praccy::ui
