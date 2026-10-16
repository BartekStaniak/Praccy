#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <sstream>

namespace praccy::tools {

enum class InstrumentType : int {
    Guitar = 0,
    Bass = 1,
    ElectricMandolin = 2,
    ElectricBowed = 3,       // Violin / Viola / Cello
    ElectricUkulele = 4,
    ElectricBanjo = 5,
    LapSteel = 6,
    Count
};

enum class TuningMode : int {
    Preset = 0,  // Root string + type
    Custom = 1   // Fully custom per-string note selection
};

enum class TuningType : int {
    Standard = 0,
    Drop = 1,
    DoubleDrop = 2,
    HalfStepDown = 3,
    FullStepDown = 4,
    OpenD = 5,
    OpenG = 6,
    OpenE = 7,
    OpenA = 8,
    OpenC = 9,
    DADGAD = 10,
    Count
};

struct StringNoteInfo {
    int stringNumber{1};        // 1 to N (1 = highest pitch)
    int midiNote{64};           // MIDI note number (e.g. 64 = E4)
    std::string noteName;       // e.g. "E4"
    float targetFreqHz{329.6f}; // Calculated based on current pitch standard A4
};

class TunerConfig {
public:
    InstrumentType instrument{InstrumentType::Guitar};
    int stringCount{6};
    TuningMode mode{TuningMode::Preset};
    TuningType tuningType{TuningType::Standard};
    int rootNoteIndex{4};          // 0: C, 1: C#, 2: D, 3: D#, 4: E, 5: F, 6: F#, 7: G, 8: G#, 9: A, 10: A#, 11: B
    float a4ReferenceHz{440.0f};   // Middle A reference pitch standard

    // String MIDI notes ordered from String 1 (highest pitch) to String N (lowest pitch)
    std::vector<int> stringMidiNotes{ 64, 59, 55, 50, 45, 40 }; // E4, B3, G3, D3, A2, E2

    TunerConfig() {
        applyPreset();
    }

    [[nodiscard]] static const char* rootNoteName(int index) noexcept {
        static constexpr const char* kNames[] = {
            "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
        };
        return kNames[(index % 12 + 12) % 12];
    }

    [[nodiscard]] static std::string midiToNoteName(int midiNote) {
        static constexpr const char* kNames[] = {
            "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
        };
        const int noteIdx = (midiNote % 12 + 12) % 12;
        const int octave = (midiNote / 12) - 1;
        return std::string(kNames[noteIdx]) + std::to_string(octave);
    }

    [[nodiscard]] static float midiToFrequency(int midiNote, float a4Ref = 440.0f) noexcept {
        return a4Ref * std::pow(2.0f, (static_cast<float>(midiNote) - 69.0f) / 12.0f);
    }

    [[nodiscard]] int minStrings() const noexcept {
        switch (instrument) {
            case InstrumentType::Guitar: return 6;
            case InstrumentType::Bass: return 4;
            case InstrumentType::ElectricMandolin: return 4;
            case InstrumentType::ElectricBowed: return 4;
            case InstrumentType::ElectricUkulele: return 4;
            case InstrumentType::ElectricBanjo: return 4;
            case InstrumentType::LapSteel: return 6;
            default: return 4;
        }
    }

    [[nodiscard]] int maxStrings() const noexcept {
        switch (instrument) {
            case InstrumentType::Guitar: return 9;
            case InstrumentType::Bass: return 6;
            case InstrumentType::ElectricMandolin: return 8;
            case InstrumentType::ElectricBowed: return 5;
            case InstrumentType::ElectricUkulele: return 4;
            case InstrumentType::ElectricBanjo: return 5;
            case InstrumentType::LapSteel: return 8;
            default: return 9;
        }
    }

    void clampStringCount() {
        stringCount = std::clamp(stringCount, minStrings(), maxStrings());
    }

    [[nodiscard]] const char* instrumentName() const noexcept {
        switch (instrument) {
            case InstrumentType::Guitar: return "Guitar";
            case InstrumentType::Bass: return "Bass";
            case InstrumentType::ElectricMandolin: return "Electric Mandolin";
            case InstrumentType::ElectricBowed: return "Electric Bowed (Violin/Cello)";
            case InstrumentType::ElectricUkulele: return "Electric Ukulele";
            case InstrumentType::ElectricBanjo: return "Electric Banjo";
            case InstrumentType::LapSteel: return "Lap Steel";
            default: return "Custom Instrument";
        }
    }

    [[nodiscard]] const char* tuningTypeName() const noexcept {
        switch (tuningType) {
            case TuningType::Standard: return "Standard";
            case TuningType::Drop: return "Drop";
            case TuningType::DoubleDrop: return "Double Drop";
            case TuningType::HalfStepDown: return "Half-Step Down";
            case TuningType::FullStepDown: return "Full-Step Down";
            case TuningType::OpenD: return "Open D";
            case TuningType::OpenG: return "Open G";
            case TuningType::OpenE: return "Open E";
            case TuningType::OpenA: return "Open A";
            case TuningType::OpenC: return "Open C";
            case TuningType::DADGAD: return "DADGAD";
            default: return "Custom";
        }
    }

    [[nodiscard]] std::string fullTuningTitle() const {
        if (mode == TuningMode::Custom) {
            std::ostringstream ss;
            ss << instrumentName() << " (" << stringCount << " Str) Custom";
            return ss.str();
        }
        std::ostringstream ss;
        ss << instrumentName() << " (" << stringCount << " Str) " << rootNoteName(rootNoteIndex) << " " << tuningTypeName();
        return ss.str();
    }

    [[nodiscard]] std::vector<StringNoteInfo> getStringInfoList() const {
        std::vector<StringNoteInfo> list;
        list.reserve(stringMidiNotes.size());
        for (size_t i = 0; i < stringMidiNotes.size(); ++i) {
            StringNoteInfo info;
            info.stringNumber = static_cast<int>(i) + 1; // 1 = highest string
            info.midiNote = stringMidiNotes[i];
            info.noteName = midiToNoteName(info.midiNote);
            info.targetFreqHz = midiToFrequency(info.midiNote, a4ReferenceHz);
            list.push_back(info);
        }
        return list;
    }

    void setInstrument(InstrumentType newInst) {
        if (instrument != newInst) {
            instrument = newInst;
            clampStringCount();
            applyPreset();
        }
    }

    void setStringCount(int count) {
        stringCount = count;
        clampStringCount();
        applyPreset();
    }

    void setTuningPreset(TuningType type, int rootIndex) {
        mode = TuningMode::Preset;
        tuningType = type;
        rootNoteIndex = (rootIndex % 12 + 12) % 12;
        applyPreset();
    }

    void setCustomStringNote(size_t stringIdx, int midiNote) {
        if (stringIdx < stringMidiNotes.size()) {
            stringMidiNotes[stringIdx] = std::clamp(midiNote, 12, 127);
            mode = TuningMode::Custom;
        }
    }

    void setA4Reference(float freqHz) {
        a4ReferenceHz = std::clamp(freqHz, 300.0f, 600.0f);
    }

    void applyPreset() {
        clampStringCount();
        stringMidiNotes.resize(static_cast<size_t>(stringCount));

        if (instrument == InstrumentType::Guitar) {
            applyGuitarPreset();
        } else if (instrument == InstrumentType::Bass) {
            applyBassPreset();
        } else if (instrument == InstrumentType::ElectricMandolin) {
            applyMandolinPreset();
        } else if (instrument == InstrumentType::ElectricBowed) {
            applyBowedPreset();
        } else if (instrument == InstrumentType::ElectricUkulele) {
            applyUkulelePreset();
        } else if (instrument == InstrumentType::ElectricBanjo) {
            applyBanjoPreset();
        } else if (instrument == InstrumentType::LapSteel) {
            applyLapSteelPreset();
        }
    }

private:
    void applyGuitarPreset() {
        // Base lowest pitch root MIDI for guitar standard:
        // C2=36, C#2=37, D2=38, D#2=39, E2=40, F2=41, F#2=42, G2=43, G#2=44, A1=33, A#1=34, B1=35
        static constexpr int kRootBaseMidi[12] = {
            36, 37, 38, 39, 40, 41, 42, 43, 44, 33, 34, 35
        };
        const int baseRoot = kRootBaseMidi[rootNoteIndex % 12];
        const int offsetFromE = baseRoot - 40; // E2 = 40

        std::vector<int> standardLowToHigh;
        if (stringCount == 6) {
            standardLowToHigh = { 40, 45, 50, 55, 59, 64 }; // E2, A2, D3, G3, B3, E4
        } else if (stringCount == 7) {
            standardLowToHigh = { 35, 40, 45, 50, 55, 59, 64 }; // B1, E2, A2, D3, G3, B3, E4
        } else if (stringCount == 8) {
            standardLowToHigh = { 30, 35, 40, 45, 50, 55, 59, 64 }; // F#1, B1, E2, A2, D3, G3, B3, E4
        } else { // 9 strings
            standardLowToHigh = { 25, 30, 35, 40, 45, 50, 55, 59, 64 }; // C#1, F#1, B1, E2, A2, D3, G3, B3, E4
        }

        std::vector<int> notes = standardLowToHigh;

        switch (tuningType) {
            case TuningType::Standard:
                for (int& n : notes) n += offsetFromE;
                break;

            case TuningType::Drop: {
                // In Drop tuning, root is the dropped lowest string
                const int dropUpperOffset = (baseRoot + 2) - 40;
                notes[0] = baseRoot;
                for (size_t i = 1; i < notes.size(); ++i) {
                    notes[i] = standardLowToHigh[i] + dropUpperOffset;
                }
                break;
            }

            case TuningType::DoubleDrop: {
                // In Double Drop, both lowest and highest strings are dropped 2 semitones
                const int dropUpperOffset = (baseRoot + 2) - 40;
                notes[0] = baseRoot;
                for (size_t i = 1; i < notes.size() - 1; ++i) {
                    notes[i] = standardLowToHigh[i] + dropUpperOffset;
                }
                notes.back() = baseRoot + 24; // e.g. D4 for D2 root
                break;
            }

            case TuningType::HalfStepDown:
                for (int& n : notes) n = n - 1;
                break;

            case TuningType::FullStepDown:
                for (int& n : notes) n = n - 2;
                break;

            case TuningType::OpenD: {
                // D2, A2, D3, F#3, A3, D4 (offset from D2=38)
                const int rootOffset = baseRoot - 38;
                if (stringCount == 6) {
                    notes = { 38, 45, 50, 54, 57, 62 };
                } else if (stringCount == 7) {
                    notes = { 33, 38, 45, 50, 54, 57, 62 }; // Low A1
                } else if (stringCount == 8) {
                    notes = { 26, 33, 38, 45, 50, 54, 57, 62 }; // Low D1
                } else {
                    notes = { 21, 26, 33, 38, 45, 50, 54, 57, 62 };
                }
                for (int& n : notes) n += rootOffset;
                break;
            }

            case TuningType::OpenG: {
                // D2, G2, D3, G3, B3, D4 (offset from D2=38)
                const int rootOffset = baseRoot - 38;
                if (stringCount == 6) {
                    notes = { 38, 43, 50, 55, 59, 62 };
                } else if (stringCount == 7) {
                    notes = { 31, 38, 43, 50, 55, 59, 62 }; // Low G1
                } else if (stringCount == 8) {
                    notes = { 26, 31, 38, 43, 50, 55, 59, 62 };
                } else {
                    notes = { 21, 26, 31, 38, 43, 50, 55, 59, 62 };
                }
                for (int& n : notes) n += rootOffset;
                break;
            }

            case TuningType::OpenE: {
                // E2, B2, E3, G#3, B3, E4 (offset from E2=40)
                const int rootOffset = baseRoot - 40;
                if (stringCount == 6) {
                    notes = { 40, 47, 52, 56, 59, 64 };
                } else if (stringCount == 7) {
                    notes = { 35, 40, 47, 52, 56, 59, 64 };
                } else if (stringCount == 8) {
                    notes = { 28, 35, 40, 47, 52, 56, 59, 64 };
                } else {
                    notes = { 23, 28, 35, 40, 47, 52, 56, 59, 64 };
                }
                for (int& n : notes) n += rootOffset;
                break;
            }

            case TuningType::OpenA: {
                // E2, A2, E3, A3, C#4, E4 (offset from E2=40)
                const int rootOffset = baseRoot - 40;
                if (stringCount == 6) {
                    notes = { 40, 45, 52, 57, 61, 64 };
                } else if (stringCount == 7) {
                    notes = { 33, 40, 45, 52, 57, 61, 64 };
                } else if (stringCount == 8) {
                    notes = { 28, 33, 40, 45, 52, 57, 61, 64 };
                } else {
                    notes = { 21, 28, 33, 40, 45, 52, 57, 61, 64 };
                }
                for (int& n : notes) n += rootOffset;
                break;
            }

            case TuningType::OpenC: {
                // C2, G2, C3, G3, C4, E4 (offset from C2=36)
                const int rootOffset = baseRoot - 36;
                if (stringCount == 6) {
                    notes = { 36, 43, 48, 55, 60, 64 };
                } else if (stringCount == 7) {
                    notes = { 31, 36, 43, 48, 55, 60, 64 };
                } else if (stringCount == 8) {
                    notes = { 24, 31, 36, 43, 48, 55, 60, 64 };
                } else {
                    notes = { 19, 24, 31, 36, 43, 48, 55, 60, 64 };
                }
                for (int& n : notes) n += rootOffset;
                break;
            }

            case TuningType::DADGAD: {
                // D2, A2, D3, G3, A3, D4 (offset from D2=38)
                const int rootOffset = baseRoot - 38;
                if (stringCount == 6) {
                    notes = { 38, 45, 50, 55, 57, 62 };
                } else if (stringCount == 7) {
                    notes = { 33, 38, 45, 50, 55, 57, 62 };
                } else if (stringCount == 8) {
                    notes = { 26, 33, 38, 45, 50, 55, 57, 62 };
                } else {
                    notes = { 21, 26, 33, 38, 45, 50, 55, 57, 62 };
                }
                for (int& n : notes) n += rootOffset;
                break;
            }

            default:
                break;
        }

        // Store ordered String 1 (highest) to String N (lowest)
        for (size_t i = 0; i < notes.size(); ++i) {
            stringMidiNotes[i] = notes[notes.size() - 1 - i];
        }
    }

    void applyBassPreset() {
        static constexpr int kRootBaseMidi[12] = {
            24, 25, 26, 27, 28, 29, 30, 31, 32, 21, 22, 23
        };
        const int baseRoot = kRootBaseMidi[rootNoteIndex % 12];
        const int offsetFromE = baseRoot - 28; // E1 = 28

        std::vector<int> lowToHigh;
        if (stringCount == 4) {
            lowToHigh = { 28, 33, 38, 43 }; // E1, A1, D2, G2
        } else if (stringCount == 5) {
            lowToHigh = { 23, 28, 33, 38, 43 }; // B0, E1, A1, D2, G2
        } else { // 6-string bass
            lowToHigh = { 23, 28, 33, 38, 43, 48 }; // B0, E1, A1, D2, G2, C3
        }

        std::vector<int> notes = lowToHigh;
        if (tuningType == TuningType::Drop) {
            const int dropUpperOffset = (baseRoot + 2) - 28;
            notes[0] = baseRoot;
            for (size_t i = 1; i < notes.size(); ++i) {
                notes[i] = lowToHigh[i] + dropUpperOffset;
            }
        } else if (tuningType == TuningType::HalfStepDown) {
            for (int& n : notes) n -= 1;
        } else if (tuningType == TuningType::FullStepDown) {
            for (int& n : notes) n -= 2;
        } else {
            for (int& n : notes) n += offsetFromE;
        }

        for (size_t i = 0; i < notes.size(); ++i) {
            stringMidiNotes[i] = notes[notes.size() - 1 - i];
        }
    }

    void applyMandolinPreset() {
        std::vector<int> lowToHigh;
        if (stringCount == 4) {
            lowToHigh = { 55, 62, 69, 76 }; // G3, D4, A4, E5
        } else if (stringCount == 5) {
            lowToHigh = { 48, 55, 62, 69, 76 }; // C3, G3, D4, A4, E5
        } else { // 8-string (4 courses of unison pairs)
            lowToHigh = { 55, 55, 62, 62, 69, 69, 76, 76 };
        }
        for (size_t i = 0; i < lowToHigh.size(); ++i) {
            stringMidiNotes[i] = lowToHigh[lowToHigh.size() - 1 - i];
        }
    }

    void applyBowedPreset() {
        std::vector<int> lowToHigh;
        if (stringCount == 4) {
            lowToHigh = { 55, 62, 69, 76 }; // Violin: G3, D4, A4, E5
        } else {
            lowToHigh = { 48, 55, 62, 69, 76 }; // 5-string Violin/Viola: C3, G3, D4, A4, E5
        }
        for (size_t i = 0; i < lowToHigh.size(); ++i) {
            stringMidiNotes[i] = lowToHigh[lowToHigh.size() - 1 - i];
        }
    }

    void applyUkulelePreset() {
        // Standard High-G: G4 (67), C4 (60), E4 (64), A4 (69)
        // Ordered String 1 to 4: A4 (69), E4 (64), C4 (60), G4 (67)
        stringMidiNotes = { 69, 64, 60, 67 };
    }

    void applyBanjoPreset() {
        if (stringCount == 5) {
            // 5-string Open G: String 1=D4, 2=B3, 3=G3, 4=D3, 5=G4 (drone)
            stringMidiNotes = { 62, 59, 55, 50, 67 };
        } else {
            // 4-string Tenor: String 1=A4, 2=D4, 3=G3, 4=C3
            stringMidiNotes = { 69, 62, 55, 48 };
        }
    }

    void applyLapSteelPreset() {
        if (stringCount == 6) {
            // C6: String 1=E4, 2=C4, 3=A3, 4=G3, 5=E3, 6=C3
            stringMidiNotes = { 64, 60, 57, 55, 52, 48 };
        } else {
            // 8-string C6/A7: String 1=G4, 2=E4, 3=C4, 4=A3, 5=G3, 6=E3, 7=C3, 8=A2
            stringMidiNotes = { 67, 64, 60, 57, 55, 52, 48, 45 };
        }
    }
};

} // namespace praccy::tools
