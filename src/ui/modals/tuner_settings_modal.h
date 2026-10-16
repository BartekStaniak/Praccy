#pragma once

#include <string>
#include <vector>
#include "../../tools/tuner.h"
#include "../../tools/tuner_settings.h"

namespace praccy::ui {

class TunerSettingsModal {
public:
    explicit TunerSettingsModal(tools::InstrumentTuner& tuner);
    ~TunerSettingsModal();

    TunerSettingsModal(const TunerSettingsModal&) = delete;
    TunerSettingsModal& operator=(const TunerSettingsModal&) = delete;

    /// Open the tuner settings modal dialog
    void open();

    /// Close the tuner settings modal dialog
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
    void renderPitchStandardSection();
    void renderInstrumentSection();
    void renderPresetTuningSection();
    void renderCustomTuningSection();

    tools::InstrumentTuner& m_tuner;
    bool m_isOpen{false};

    // Pitch standard interaction state
    float m_lastQuickPitchStandard{440.0f};
    bool m_pitchEditingText{false};

    // Live tuning target strings preview state (retains last highlighted string between plucks)
    int m_lastHighlightedStringIndex{-1};
    float m_lastDetectedFreq{0.0f};
    float m_lastDetectedCents{0.0f};
    std::string m_lastDetectedNote;

    // Cached note list for dropdowns
    struct NoteOption {
        int midiNote{60};
        std::string label;
    };
    std::vector<NoteOption> m_noteOptions;
    void initNoteOptions();
};

} // namespace praccy::ui
