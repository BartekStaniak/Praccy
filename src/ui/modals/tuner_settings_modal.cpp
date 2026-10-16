#include "tuner_settings_modal.h"
#include "../design_tokens.h"
#include "../theme.h"
#include "../ui_helpers.h"
#include <imgui.h>
#include <cstdio>
#include <cmath>
#include <algorithm>

extern ImFont* g_fontUI;
extern ImFont* g_fontMono;

#include "../../state/app_config.h"

namespace praccy::ui {

TunerSettingsModal::TunerSettingsModal(tools::InstrumentTuner& tuner)
    : m_tuner(tuner)
{
    initNoteOptions();
}

TunerSettingsModal::~TunerSettingsModal() = default;

void TunerSettingsModal::open() {
    m_isOpen = true;
    m_pitchEditingText = false;
    m_lastQuickPitchStandard = m_tuner.a4Reference();
}

void TunerSettingsModal::close() {
    m_isOpen = false;
    state::AppConfig cfg;
    cfg.load();
    cfg.tunerEnabled = m_tuner.isEnabled();
    cfg.tunerA4Reference = m_tuner.a4Reference();
    const auto& tcfg = m_tuner.config();
    cfg.tunerInstrument = static_cast<int>(tcfg.instrument);
    cfg.tunerStringCount = tcfg.stringCount;
    cfg.tunerTuningMode = static_cast<int>(tcfg.mode);
    cfg.tunerTuningType = static_cast<int>(tcfg.tuningType);
    cfg.tunerRootNote = tcfg.rootNoteIndex;
    if (tcfg.mode == tools::TuningMode::Custom) {
        std::string s;
        for (size_t i = 0; i < tcfg.stringMidiNotes.size(); ++i) {
            if (i > 0) s += ",";
            s += std::to_string(tcfg.stringMidiNotes[i]);
        }
        cfg.tunerCustomStrings = s;
    }
    cfg.save();
}

void TunerSettingsModal::initNoteOptions() {
    m_noteOptions.clear();
    // Support MIDI note range 12 (C0) to 108 (C8)
    for (int m = 12; m <= 108; ++m) {
        NoteOption opt;
        opt.midiNote = m;
        opt.label = tools::TunerConfig::midiToNoteName(m);
        m_noteOptions.push_back(opt);
    }
}

void TunerSettingsModal::renderPitchStandardSection() {
    const auto& tokens = themeTokens();
    float currentA4 = m_tuner.a4Reference();

    ImGui::TextColored(tokens.text.accent.vec4, "PITCH STANDARD (MIDDLE A / A4)");
    ImGui::TextColored(tokens.text.muted.vec4, "Offsets target frequencies and pitch detection reference across all instruments.");
    ImGui::Spacing();

    // 1. Quick Pitch Standards Dropdown
    struct QuickStandard {
        const char* label;
        float freq;
    };
    static const QuickStandard kStandards[] = {
        { "440.0 Hz (Concert Pitch Standard)",  440.0f },
        { "432.0 Hz (Verdi / Scientific Pitch)", 432.0f },
        { "442.0 Hz (European Concert Standard)",442.0f },
        { "444.0 Hz (High Concert Pitch)",       444.0f },
        { "415.0 Hz (Baroque Pitch Standard)",   415.0f },
        { "435.0 Hz (French Diapason Normal)",   435.0f },
        { "446.0 Hz (Renaissance Pitch)",        446.0f }
    };

    int matchedIdx = -1;
    for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(kStandards)); ++i) {
        if (std::abs(currentA4 - kStandards[i].freq) < 0.05f) {
            matchedIdx = i;
            break;
        }
    }

    char previewBuf[64];
    if (matchedIdx >= 0) {
        std::snprintf(previewBuf, sizeof(previewBuf), "%s", kStandards[matchedIdx].label);
    } else {
        std::snprintf(previewBuf, sizeof(previewBuf), "Custom (%.1f Hz)", currentA4);
    }

    ImGui::AlignTextToFramePadding();
    ImGui::Text("Pitch Standard:");
    ImGui::SameLine(150.0f);
    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::BeginCombo("##QuickPitchStandardsCombo", previewBuf)) {
        for (int i = 0; i < static_cast<int>(IM_ARRAYSIZE(kStandards)); ++i) {
            const bool isSelected = (matchedIdx == i);
            if (ImGui::Selectable(kStandards[i].label, isSelected)) {
                m_tuner.setA4Reference(kStandards[i].freq);
                m_lastQuickPitchStandard = kStandards[i].freq;
                m_pitchEditingText = false;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Spacing();

    // 2. Middle A Slider with click-to-edit float input and double-click reset
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Middle A (A4):");
    ImGui::SameLine(150.0f);
    ImGui::SetNextItemWidth(280.0f);

    if (m_pitchEditingText) {
        ImGui::SetKeyboardFocusHere();
        float inputVal = currentA4;
        bool committed = false;
        if (ImGui::InputFloat("##PitchStandardInlineInput", &inputVal, 0.0f, 0.0f, "%.1f Hz", ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_tuner.setA4Reference(std::clamp(inputVal, 400.0f, 480.0f));
            m_pitchEditingText = false;
            committed = true;
        }
        if (!committed) {
            if (ImGui::IsItemDeactivated()) {
                m_tuner.setA4Reference(std::clamp(inputVal, 400.0f, 480.0f));
                m_pitchEditingText = false;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                m_pitchEditingText = false;
            }
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            m_tuner.setA4Reference(m_lastQuickPitchStandard);
            m_pitchEditingText = false;
        }
    } else {
        float sliderVal = currentA4;
        if (ImGui::SliderFloat("##PitchStandardSlider", &sliderVal, 400.0f, 480.0f, "%.1f Hz", ImGuiSliderFlags_AlwaysClamp)) {
            m_tuner.setA4Reference(sliderVal);
        }

        const bool isHovered = ImGui::IsItemHovered();
        const bool isDoubleClicked = isHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        const bool isDeactivated = ImGui::IsItemDeactivated();

        if (isDoubleClicked) {
            m_tuner.setA4Reference(m_lastQuickPitchStandard);
            m_pitchEditingText = false;
        } else if (isDeactivated) {
            // User clicked on slider without dragging -> switch to float text input
            if (ImGui::GetIO().MouseDragMaxDistanceSqr[0] < 9.0f) {
                m_pitchEditingText = true;
            }
        }

        if (isHovered) {
            char tt[140];
            std::snprintf(tt, sizeof(tt), "Click to type float | Drag to adjust | Double-click to reset (%.1f Hz)", m_lastQuickPitchStandard);
            ImGui::SetTooltip("%s", tt);
        }
    }
}

void TunerSettingsModal::renderInstrumentSection() {
    const auto& tokens = themeTokens();
    auto& cfg = m_tuner.config();

    ImGui::TextColored(tokens.text.accent.vec4, "INSTRUMENT & STRINGS");
    ImGui::Separator();
    ImGui::Spacing();

    // 1. Instrument Type Selector (string counts removed from names)
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Instrument Type:");
    ImGui::SameLine(150.0f);

    const char* instNames[] = {
        "Guitar",
        "Bass",
        "Electric Mandolin",
        "Electric Bowed (Violin/Cello)",
        "Electric Ukulele",
        "Electric Banjo",
        "Lap Steel"
    };
    int currentInstIdx = static_cast<int>(cfg.instrument);

    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::Combo("##InstrumentTypeCombo", &currentInstIdx, instNames, IM_ARRAYSIZE(instNames))) {
        cfg.setInstrument(static_cast<tools::InstrumentType>(currentInstIdx));
        m_tuner.setConfig(cfg);
    }

    ImGui::Spacing();

    // 2. Amount of Strings Dropdown
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Amount of Strings:");
    ImGui::SameLine(150.0f);

    std::vector<int> availableCounts;
    if (cfg.instrument == tools::InstrumentType::ElectricMandolin) {
        availableCounts = { 4, 5, 8 };
    } else if (cfg.instrument == tools::InstrumentType::LapSteel) {
        availableCounts = { 6, 8 };
    } else {
        for (int s = cfg.minStrings(); s <= cfg.maxStrings(); ++s) {
            availableCounts.push_back(s);
        }
    }

    int currentStrCountIdx = 0;
    char strCountLabels[12][24]{};
    const char* strCountCStrs[12]{};
    const size_t nCounts = std::min<size_t>(availableCounts.size(), 12);
    for (size_t i = 0; i < nCounts; ++i) {
        std::snprintf(strCountLabels[i], sizeof(strCountLabels[i]), "%d Strings", availableCounts[i]);
        strCountCStrs[i] = strCountLabels[i];
        if (availableCounts[i] == cfg.stringCount) {
            currentStrCountIdx = static_cast<int>(i);
        }
    }

    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::Combo("##StringCountCombo", &currentStrCountIdx, strCountCStrs, static_cast<int>(nCounts))) {
        cfg.setStringCount(availableCounts[currentStrCountIdx]);
        m_tuner.setConfig(cfg);
    }
}

void TunerSettingsModal::renderPresetTuningSection() {
    const auto& tokens = themeTokens();
    auto& cfg = m_tuner.config();

    ImGui::Spacing();
    ImGui::TextColored(tokens.text.secondary.vec4, "Select Root String Note and Tuning Preset:");
    ImGui::Spacing();

    // 1. Root string note selector
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Root String Note:");
    ImGui::SameLine(150.0f);

    const char* rootNotes[] = {
        "C", "C# / Db", "D", "D# / Eb", "E", "F", "F# / Gb", "G", "G# / Ab", "A", "A# / Bb", "B"
    };
    int rootIdx = cfg.rootNoteIndex;
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("##TuningRootCombo", &rootIdx, rootNotes, IM_ARRAYSIZE(rootNotes))) {
        cfg.setTuningPreset(cfg.tuningType, rootIdx);
        m_tuner.setConfig(cfg);
    }

    // 2. Tuning selector (label says "Tuning:" instead of "Tuning Type:")
    ImGui::SameLine(320.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Tuning:");
    ImGui::SameLine(390.0f);

    const char* typeNames[] = {
        "Standard",
        "Drop",
        "Double Drop",
        "Half-Step Down",
        "Full-Step Down",
        "Open D",
        "Open G",
        "Open E",
        "Open A",
        "Open C",
        "DADGAD"
    };
    int typeIdx = static_cast<int>(cfg.tuningType);
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::Combo("##TuningTypeCombo", &typeIdx, typeNames, IM_ARRAYSIZE(typeNames))) {
        cfg.setTuningPreset(static_cast<tools::TuningType>(typeIdx), cfg.rootNoteIndex);
        m_tuner.setConfig(cfg);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // 3. Calculated Target Strings Preview with live dynamic tuning highlight
    ImGui::TextColored(tokens.text.accent.vec4, "Calculated Target Strings Preview (Live Tuner Active):");
    ImGui::Spacing();

    auto strings = cfg.getStringInfoList();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Spec::tableCellPadding());
    if (ImGui::BeginTable("PresetStringsTable", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("String", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Target Note", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Frequency", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Live Tuner Feedback", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < strings.size(); ++i) {
            const auto& str = strings[i];
            const bool isHighlighted = (static_cast<int>(i) == m_lastHighlightedStringIndex);

            ImGui::TableNextRow(ImGuiTableRowFlags_None, Spec::tableCompactRowHeight());

            // Dynamic glow row background for the detected/plucked string
            if (isHighlighted) {
                ImU32 rowColor;
                if (std::abs(m_lastDetectedCents) <= 4.0f) {
                    rowColor = tokens.signal.tunerInTune.withAlpha(0.45f).u32; // In tune emerald
                } else if (m_lastDetectedCents < -4.0f) {
                    rowColor = tokens.signal.tunerFlat.withAlpha(0.45f).u32;   // Flat warm amber
                } else {
                    rowColor = tokens.signal.tunerSharp.withAlpha(0.45f).u32;  // Sharp coral red
                }
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowColor);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, rowColor);
            }

            // Column 1: String identifier (Vertically centered)
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(ImGui::GetFontSize(), Spec::tableCompactRowHeight());
            if (isHighlighted) {
                ImGui::TextColored(tokens.text.accent.vec4, "> String %d%s", str.stringNumber,
                                   (str.stringNumber == 1 ? " (High)" : (str.stringNumber == static_cast<int>(strings.size()) ? " (Low)" : "")));
            } else {
                if (str.stringNumber == 1) {
                    ImGui::Text("String 1 (High)");
                } else if (str.stringNumber == static_cast<int>(strings.size())) {
                    ImGui::Text("String %d (Low)", str.stringNumber);
                } else {
                    ImGui::Text("String %d", str.stringNumber);
                }
            }

            // Column 2: Target Note (Vertically centered)
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(ImGui::GetFontSize(), Spec::tableCompactRowHeight());
            ImGui::TextColored(tokens.signal.tunerInTune.vec4, "%s", str.noteName.c_str());

            // Column 3: Monospace target frequency (Vertically centered)
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(ImGui::GetFontSize(), Spec::tableCompactRowHeight());
            if (g_fontMono) ImGui::PushFont(g_fontMono);
            ImGui::Text("%.1f Hz", str.targetFreqHz);
            if (g_fontMono) ImGui::PopFont();

            // Column 4: Live pitch feedback and cents deviation (Vertically centered)
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(ImGui::GetFontSize(), Spec::tableCompactRowHeight());
            if (isHighlighted) {
                if (g_fontMono) ImGui::PushFont(g_fontMono);
                if (std::abs(m_lastDetectedCents) <= 4.0f) {
                    ImGui::TextColored(tokens.signal.tunerInTune.vec4, "[ IN TUNE ]  %.1f Hz (%+.1f ct)", m_lastDetectedFreq, m_lastDetectedCents);
                } else if (m_lastDetectedCents < -4.0f) {
                    ImGui::TextColored(tokens.signal.tunerFlat.vec4, "[ FLAT ]     %.1f Hz (%+.1f ct)", m_lastDetectedFreq, m_lastDetectedCents);
                } else {
                    ImGui::TextColored(tokens.signal.tunerSharp.vec4, "[ SHARP ]    %.1f Hz (%+.1f ct)", m_lastDetectedFreq, m_lastDetectedCents);
                }
                if (g_fontMono) ImGui::PopFont();
            } else {
                ImGui::TextDisabled("MIDI %d", str.midiNote);
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    ImGui::Spacing();
    if (CenteredButton("Switch to Custom Mode with These Notes", ImVec2(320.0f, Metrics::kButtonHeight))) {
        cfg.mode = tools::TuningMode::Custom;
        m_tuner.setConfig(cfg);
    }
}

void TunerSettingsModal::renderCustomTuningSection() {
    const auto& tokens = themeTokens();
    auto& cfg = m_tuner.config();

    ImGui::Spacing();
    ImGui::TextColored(tokens.text.secondary.vec4, "Select the note and octave for each string independently:");
    ImGui::Spacing();

    auto strings = cfg.getStringInfoList();
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Spec::tableCellPadding());
    if (ImGui::BeginTable("CustomStringsTable", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("String", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Note Dropdown", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Semitone Step", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Octave Step", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("Target & Live Tuning", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < strings.size(); ++i) {
            const auto& str = strings[i];
            const bool isHighlighted = (static_cast<int>(i) == m_lastHighlightedStringIndex);

            ImGui::TableNextRow(ImGuiTableRowFlags_None, Spec::tableRowHeight());

            // Dynamic glow row background
            if (isHighlighted) {
                ImU32 rowColor;
                if (std::abs(m_lastDetectedCents) <= 4.0f) {
                    rowColor = tokens.signal.tunerInTune.withAlpha(0.45f).u32;
                } else if (m_lastDetectedCents < -4.0f) {
                    rowColor = tokens.signal.tunerFlat.withAlpha(0.45f).u32;
                } else {
                    rowColor = tokens.signal.tunerSharp.withAlpha(0.45f).u32;
                }
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, rowColor);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, rowColor);
            }

            // 1. String identifier (Vertically centered)
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(ImGui::GetFontSize(), Spec::tableRowHeight());
            if (isHighlighted) {
                ImGui::TextColored(tokens.text.accent.vec4, "> String %d%s", str.stringNumber,
                                   (str.stringNumber == 1 ? " (High)" : (str.stringNumber == static_cast<int>(strings.size()) ? " (Low)" : "")));
            } else {
                if (str.stringNumber == 1) {
                    ImGui::Text("String 1 (High)");
                } else if (str.stringNumber == static_cast<int>(strings.size())) {
                    ImGui::Text("String %d (Low)", str.stringNumber);
                } else {
                    ImGui::Text("String %d", str.stringNumber);
                }
            }

            // 2. Note Dropdown combo (Vertically centered)
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(Spec::comboHeight(), Spec::tableRowHeight());
            char comboId[32];
            std::snprintf(comboId, sizeof(comboId), "##StrNoteCombo_%zu", i);
            ImGui::SetNextItemWidth(110.0f);

            if (ImGui::BeginCombo(comboId, str.noteName.c_str())) {
                for (const auto& opt : m_noteOptions) {
                    const bool isSelected = (opt.midiNote == str.midiNote);
                    if (ImGui::Selectable(opt.label.c_str(), isSelected)) {
                        cfg.setCustomStringNote(i, opt.midiNote);
                        m_tuner.setConfig(cfg);
                    }
                    if (isSelected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            // 3. Semitone +/- buttons (Vertically centered)
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(Spec::buttonHeight(), Spec::tableRowHeight());
            char minusId[32], plusId[32];
            std::snprintf(minusId, sizeof(minusId), "- 1st##Minus_%zu", i);
            std::snprintf(plusId, sizeof(plusId), "+ 1st##Plus_%zu", i);

            if (CenteredButton(minusId, ImVec2(52.0f, Spec::buttonHeight()))) {
                cfg.setCustomStringNote(i, str.midiNote - 1);
                m_tuner.setConfig(cfg);
            }
            ImGui::SameLine(0, 4.0f);
            if (CenteredButton(plusId, ImVec2(52.0f, Spec::buttonHeight()))) {
                cfg.setCustomStringNote(i, str.midiNote + 1);
                m_tuner.setConfig(cfg);
            }

            // 4. Octave +/- buttons (Vertically centered)
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(Spec::buttonHeight(), Spec::tableRowHeight());
            char octMinusId[32], octPlusId[32];
            std::snprintf(octMinusId, sizeof(octMinusId), "- Oct##OctMinus_%zu", i);
            std::snprintf(octPlusId, sizeof(octPlusId), "+ Oct##OctPlus_%zu", i);

            if (CenteredButton(octMinusId, ImVec2(52.0f, Spec::buttonHeight()))) {
                cfg.setCustomStringNote(i, str.midiNote - 12);
                m_tuner.setConfig(cfg);
            }
            ImGui::SameLine(0, 4.0f);
            if (CenteredButton(octPlusId, ImVec2(52.0f, Spec::buttonHeight()))) {
                cfg.setCustomStringNote(i, str.midiNote + 12);
                m_tuner.setConfig(cfg);
            }

            // 5. Monospace frequency readout & live feedback (Vertically centered)
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(ImGui::GetFontSize(), Spec::tableRowHeight());
            if (g_fontMono) ImGui::PushFont(g_fontMono);
            if (isHighlighted) {
                if (std::abs(m_lastDetectedCents) <= 4.0f) {
                    ImGui::TextColored(tokens.signal.tunerInTune.vec4, "%.1f Hz  [ IN TUNE %+.1f ct ]", str.targetFreqHz, m_lastDetectedCents);
                } else if (m_lastDetectedCents < -4.0f) {
                    ImGui::TextColored(tokens.signal.tunerFlat.vec4, "%.1f Hz  [ FLAT %+.1f ct ]", str.targetFreqHz, m_lastDetectedCents);
                } else {
                    ImGui::TextColored(tokens.signal.tunerSharp.vec4, "%.1f Hz  [ SHARP %+.1f ct ]", str.targetFreqHz, m_lastDetectedCents);
                }
            } else {
                ImGui::TextColored(tokens.signal.tunerInTune.vec4, "%.1f Hz", str.targetFreqHz);
            }
            if (g_fontMono) ImGui::PopFont();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();

    ImGui::Spacing();
    if (CenteredButton("Reset Strings to Preset", ImVec2(180.0f, Spec::buttonHeight()))) {
        cfg.applyPreset();
        cfg.mode = tools::TuningMode::Preset;
        m_tuner.setConfig(cfg);
    }
}

void TunerSettingsModal::render() {
    if (!m_isOpen) return;

    // Continuously sample live tuner pitch detection to highlight active string (persists even between plucks)
    tools::TunerResult liveResult = m_tuner.currentResult();
    if (liveResult.confidence && liveResult.matchedStringIndex >= 0) {
        m_lastHighlightedStringIndex = liveResult.matchedStringIndex;
        m_lastDetectedFreq = liveResult.frequencyHz;
        m_lastDetectedCents = liveResult.stringCentDeviation;
        m_lastDetectedNote = liveResult.noteName;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float initW = vp ? std::clamp(vp->Size.x * 0.65f, 680.0f, 920.0f) : 700.0f;
    const float initH = vp ? std::clamp(vp->Size.y * 0.72f, 540.0f, 760.0f) : 580.0f;

    ImGui::OpenPopup("Tuner Settings##Modal");
    ImGui::SetNextWindowSize(ImVec2(initW, initH), ImGuiCond_Appearing);
    if (vp) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(580.0f, 440.0f), ImVec2(vp->Size.x * 0.95f, vp->Size.y * 0.95f));
    }
    ImGui::SetNextWindowPos(vp ? vp->GetCenter() : ImVec2(100.0f, 100.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    const auto& tokens = themeTokens();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, tokens.surfaces.popupBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.subtle.vec4);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Spec::kModalCornerRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Spec::kModalWindowPadding, Spec::kModalWindowPadding));

    if (ImGui::BeginPopupModal("Tuner Settings##Modal", &m_isOpen, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const float footerHeight = Metrics::kFooterHeight;
        const float contentHeight = ImGui::GetContentRegionAvail().y - footerHeight;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ColorToken::transparent().vec4);
        if (ImGui::BeginChild("TunerSettingsContentRegion", ImVec2(0, contentHeight), false)) {
            // Zone 1: Pitch Standard Section (Middle A / 440 Hz)
            renderPitchStandardSection();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Zone 2: Instrument & String Count Section
            renderInstrumentSection();

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Zone 3: Tuning Configurations: Presets OR Custom (Dropdown selector)
            auto& cfg = m_tuner.config();

            ImGui::TextColored(tokens.text.accent.vec4, "TUNING CONFIGURATION");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::AlignTextToFramePadding();
            ImGui::Text("Tuning Mode:");
            ImGui::SameLine(150.0f);
            ImGui::SetNextItemWidth(280.0f);

            const char* modeNames[] = { "Presets", "Custom" };
            int modeIdx = (cfg.mode == tools::TuningMode::Custom) ? 1 : 0;
            if (ImGui::Combo("##TuningModeCombo", &modeIdx, modeNames, IM_ARRAYSIZE(modeNames))) {
                cfg.mode = (modeIdx == 1) ? tools::TuningMode::Custom : tools::TuningMode::Preset;
                m_tuner.setConfig(cfg);
            }

            if (cfg.mode == tools::TuningMode::Preset) {
                renderPresetTuningSection();
            } else {
                renderCustomTuningSection();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Separator();
        ImGui::Spacing();

        // Footer: Active tuning badge and Done button
        const auto& cfg = m_tuner.config();
        std::string summary = cfg.fullTuningTitle() + "  |  A4 = " + std::to_string(static_cast<int>(std::round(m_tuner.a4Reference()))) + " Hz";
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(tokens.text.muted.vec4, "%s", summary.c_str());

        ImGui::SameLine(ImGui::GetWindowWidth() - 110.0f);
        if (CenteredButton("Done", ImVec2(90, Metrics::buttonHeight()))) {
            close();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

} // namespace praccy::ui
