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
    void renderLooperTab();
    void renderBackingTrackTab();
    void renderLooperCircularProgressRing();

    tools::QuickLooper& m_looper;
    tools::AudioPlayer& m_player;

    bool m_isOpen{false};
    char m_audioFilePathBuffer[260]{0};
};

} // namespace praccy::ui
