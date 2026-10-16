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
        clampStringCount();
        rootNoteIndex = defaultRootNoteIndex(instrument, stringCount, tuningType);
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

    [[nodiscard]] static int defaultRootNoteIndex(InstrumentType inst, int stringCount, TuningType tuning) noexcept {
        if (inst == InstrumentType::Guitar) {
            if (tuning == TuningType::Drop || tuning == TuningType::DoubleDrop) {
                switch (stringCount) {
                    case 6: return 2;  // D (Drop D)
                    case 7: return 9;  // A (Drop A)
                    case 8: return 4;  // E (Drop E)
                    case 9: return 11; // B (Drop B)
                    default: return 2;
                }
            }
            if (tuning == TuningType::HalfStepDown) {
                switch (stringCount) {
                    case 6: return 3;  // D# (Eb Standard)
                    case 7: return 10; // A# (Bb Standard)
                    case 8: return 5;  // F (F Standard / Half-step down)
                    case 9: return 0;  // C (C Standard / Half-step down)
                    default: return 3;
                }
            }
            if (tuning == TuningType::FullStepDown) {
                switch (stringCount) {
                    case 6: return 2;  // D (D Standard)
                    case 7: return 9;  // A (A Standard)
                    case 8: return 4;  // E (E Standard)
                    case 9: return 11; // B (B Standard)
                    default: return 2;
                }
            }
            if (tuning == TuningType::OpenD || tuning == TuningType::DADGAD) return 2; // D
            if (tuning == TuningType::OpenG) return 7; // G
            if (tuning == TuningType::OpenE) return 4; // E
            if (tuning == TuningType::OpenA) return 9; // A
            if (tuning == TuningType::OpenC) return 0; // C
            // Standard:
            switch (stringCount) {
                case 6: return 4;  // E (E Standard)
                case 7: return 11; // B (B Standard)
                case 8: return 6;  // F# (F# Standard)
                case 9: return 1;  // C# (C# Standard)
                default: return 4;
            }
        } else if (inst == InstrumentType::Bass) {
            if (tuning == TuningType::Drop) {
                return (stringCount == 4) ? 2 : 9; // D for 4-str, A for 5/6-str
            }
            if (tuning == TuningType::HalfStepDown) {
                return (stringCount == 4) ? 3 : 10; // Eb for 4-str, Bb for 5/6-str
            }
            if (tuning == TuningType::FullStepDown) {
                return (stringCount == 4) ? 2 : 9; // D for 4-str, A for 5/6-str
            }
            return (stringCount == 4) ? 4 : 11; // E for 4-str, B for 5/6-str
        } else if (inst == InstrumentType::ElectricMandolin) {
            return (stringCount == 5) ? 0 : 7; // C for 5-str, G for 4/8-str
        } else if (inst == InstrumentType::ElectricBowed) {
            return 7; // G for Violin (or C for Cello)
        } else if (inst == InstrumentType::ElectricUkulele) {
            return 0; // C
        } else if (inst == InstrumentType::ElectricBanjo) {
            return (stringCount == 5) ? 7 : 0; // G for 5-str, C for 4-str
        } else if (inst == InstrumentType::LapSteel) {
            return 0; // C (C6)
        }
        return 4;
    }

    [[nodiscard]] static int rootNoteBaseMidi(InstrumentType inst, int stringCount, int rootIndex) noexcept {
        const int idx = (rootIndex % 12 + 12) % 12;
        if (inst == InstrumentType::Guitar) {
            if (stringCount == 6) {
                // E2=40 centered
                static constexpr int kRoots[12] = { 36, 37, 38, 39, 40, 41, 42, 43, 44, 33, 34, 35 };
                return kRoots[idx];
            } else if (stringCount == 7) {
                // B1=35 centered
                static constexpr int kRoots[12] = { 36, 37, 38, 39, 28, 29, 30, 31, 32, 33, 34, 35 };
                return kRoots[idx];
            } else if (stringCount == 8) {
                // F#1=30 centered
                static constexpr int kRoots[12] = { 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35 };
                return kRoots[idx];
            } else { // 9 strings
                // C#1=25 centered
                static constexpr int kRoots[12] = { 24, 25, 26, 27, 28, 29, 30, 19, 20, 21, 22, 23 };
                return kRoots[idx];
            }
        } else if (inst == InstrumentType::Bass) {
            if (stringCount == 4) {
                // E1=28 centered
                static constexpr int kRoots[12] = { 24, 25, 26, 27, 28, 29, 30, 31, 32, 21, 22, 23 };
                return kRoots[idx];
            } else {
                // B0=23 centered
                static constexpr int kRoots[12] = { 24, 25, 26, 27, 28, 17, 18, 19, 20, 21, 22, 23 };
                return kRoots[idx];
            }
        } else if (inst == InstrumentType::ElectricMandolin) {
            static constexpr int kRoots[12] = { 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59 };
            return kRoots[idx];
        } else if (inst == InstrumentType::ElectricBowed) {
            if (idx == 0) return 36; // C2 for Cello
            static constexpr int kRoots[12] = { 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59 };
            return kRoots[idx];
        } else if (inst == InstrumentType::ElectricUkulele) {
            static constexpr int kRoots[12] = { 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71 };
            return kRoots[idx];
        } else if (inst == InstrumentType::ElectricBanjo) {
            static constexpr int kRoots[12] = { 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59 };
            return kRoots[idx];
        } else if (inst == InstrumentType::LapSteel) {
            static constexpr int kRoots[12] = { 48, 49, 50, 51, 52, 53, 54, 55, 56, 45, 46, 47 };
            return kRoots[idx];
        }
        return 40;
    }

    [[nodiscard]] static int standardRootMidi(InstrumentType inst, int stringCount) noexcept {
        if (inst == InstrumentType::Guitar) {
            switch (stringCount) {
                case 6: return 40;  // E2
                case 7: return 35;  // B1
                case 8: return 30;  // F#1
                case 9: return 25;  // C#1
                default: return 40;
            }
        } else if (inst == InstrumentType::Bass) {
            return (stringCount == 4) ? 28 : 23; // E1 or B0
        } else if (inst == InstrumentType::ElectricMandolin) {
            return (stringCount == 5) ? 48 : 55; // C3 or G3
        } else if (inst == InstrumentType::ElectricBowed) {
            return 55; // G3
        } else if (inst == InstrumentType::ElectricUkulele) {
            return 60; // C4
        } else if (inst == InstrumentType::ElectricBanjo) {
            return (stringCount == 5) ? 55 : 48; // G3 or C3
        } else if (inst == InstrumentType::LapSteel) {
            return (stringCount == 6) ? 48 : 45; // C3 or A2
        }
        return 40;
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
        ss << instrumentName() << " (" << stringCount << " Str) ";
        if (tuningType == TuningType::Standard) {
            ss << rootNoteName(rootNoteIndex) << " Standard";
        } else if (tuningType == TuningType::Drop) {
            ss << "Drop " << rootNoteName(rootNoteIndex);
        } else if (tuningType == TuningType::DoubleDrop) {
            ss << "Double Drop " << rootNoteName(rootNoteIndex);
        } else if (tuningType == TuningType::HalfStepDown) {
            ss << "Half-Step Down (" << rootNoteName(rootNoteIndex) << " Standard)";
        } else if (tuningType == TuningType::FullStepDown) {
            ss << "Full-Step Down (" << rootNoteName(rootNoteIndex) << " Standard)";
        } else {
            ss << tuningTypeName();
        }
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
            rootNoteIndex = defaultRootNoteIndex(instrument, stringCount, tuningType);
            applyPreset();
        }
    }

    void setStringCount(int count) {
        if (stringCount != count) {
            stringCount = count;
            clampStringCount();
            rootNoteIndex = defaultRootNoteIndex(instrument, stringCount, tuningType);
            applyPreset();
        }
    }

    void setTuningPreset(TuningType type, int rootIndex) {
        mode = TuningMode::Preset;
        tuningType = type;
        rootNoteIndex = (rootIndex % 12 + 12) % 12;
        applyPreset();
    }

    void setTuningPreset(TuningType type) {
        mode = TuningMode::Preset;
        tuningType = type;
        rootNoteIndex = defaultRootNoteIndex(instrument, stringCount, type);
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
        const int baseRoot = rootNoteBaseMidi(instrument, stringCount, rootNoteIndex);
        const int stdRoot = standardRootMidi(instrument, stringCount);

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
            case TuningType::Standard: {
                const int offset = baseRoot - stdRoot;
                for (int& n : notes) n += offset;
                break;
            }

            case TuningType::Drop: {
                // Lowest string is baseRoot, string 2 is 7 semitones above, upper strings follow standard intervals
                notes[0] = baseRoot;
                notes[1] = baseRoot + 7;
                for (size_t i = 2; i < notes.size(); ++i) {
                    notes[i] = notes[i - 1] + (standardLowToHigh[i] - standardLowToHigh[i - 1]);
                }
                break;
            }

            case TuningType::DoubleDrop: {
                notes[0] = baseRoot;
                notes[1] = baseRoot + 7;
                for (size_t i = 2; i < notes.size(); ++i) {
                    notes[i] = notes[i - 1] + (standardLowToHigh[i] - standardLowToHigh[i - 1]);
                }
                notes.back() -= 2;
                break;
            }

            case TuningType::HalfStepDown: {
                for (int& n : notes) n -= 1;
                break;
            }

            case TuningType::FullStepDown: {
                for (int& n : notes) n -= 2;
                break;
            }

            case TuningType::OpenD: {
                // Reference Open D: D2, A2, D3, F#3, A3, D4 (offset from D2=38)
                const int rootOffset = rootNoteBaseMidi(InstrumentType::Guitar, 6, rootNoteIndex) - 38;
                if (stringCount == 6) {
                    notes = { 38, 45, 50, 54, 57, 62 };
                } else if (stringCount == 7) {
                    notes = { 33, 38, 45, 50, 54, 57, 62 }; // Low A1
                } else if (stringCount == 8) {
                    notes = { 26, 33, 38, 45, 50, 54, 57, 62 }; // Low D1
                } else {
                    notes = { 21, 26, 33, 38, 45, 50, 54, 57, 62 }; // Low A0
                }
                for (int& n : notes) n += rootOffset;
                break;
            }

            case TuningType::OpenG: {
                const int rootOffset = rootNoteBaseMidi(InstrumentType::Guitar, 6, rootNoteIndex) - 43;
                if (stringCount == 6) {
                    notes = { 38, 43, 50, 55, 59, 62 };
                } else if (stringCount == 7) {
                    notes = { 31, 38, 43, 50, 55, 59, 62 }; // Low G1
                } else if (stringCount == 8) {
                    notes = { 26, 31, 38, 43, 50, 55, 59, 62 };
                } else {
                    notes = { 19, 26, 31, 38, 43, 50, 55, 59, 62 };
                }
                for (int& n : notes) n += rootOffset;
                break;
            }

            case TuningType::OpenE: {
                const int rootOffset = rootNoteBaseMidi(InstrumentType::Guitar, 6, rootNoteIndex) - 40;
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
                const int rootOffset = rootNoteBaseMidi(InstrumentType::Guitar, 6, rootNoteIndex) - 33;
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
                const int rootOffset = rootNoteBaseMidi(InstrumentType::Guitar, 6, rootNoteIndex) - 36;
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
                const int rootOffset = rootNoteBaseMidi(InstrumentType::Guitar, 6, rootNoteIndex) - 38;
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
        const int baseRoot = rootNoteBaseMidi(instrument, stringCount, rootNoteIndex);
        const int stdRoot = standardRootMidi(instrument, stringCount);

        std::vector<int> standardLowToHigh;
        if (stringCount == 4) {
            standardLowToHigh = { 28, 33, 38, 43 }; // E1, A1, D2, G2
        } else if (stringCount == 5) {
            standardLowToHigh = { 23, 28, 33, 38, 43 }; // B0, E1, A1, D2, G2
        } else { // 6-string bass
            standardLowToHigh = { 23, 28, 33, 38, 43, 48 }; // B0, E1, A1, D2, G2, C3
        }

        std::vector<int> notes = standardLowToHigh;

        switch (tuningType) {
            case TuningType::Standard: {
                const int offset = baseRoot - stdRoot;
                for (int& n : notes) n += offset;
                break;
            }

            case TuningType::Drop: {
                notes[0] = baseRoot;
                notes[1] = baseRoot + 7;
                for (size_t i = 2; i < notes.size(); ++i) {
                    notes[i] = notes[i - 1] + (standardLowToHigh[i] - standardLowToHigh[i - 1]);
                }
                break;
            }

            case TuningType::HalfStepDown: {
                for (int& n : notes) n -= 1;
                break;
            }

            case TuningType::FullStepDown: {
                for (int& n : notes) n -= 2;
                break;
            }

            default: {
                const int offset = baseRoot - stdRoot;
                for (int& n : notes) n += offset;
                break;
            }
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
            if (rootNoteIndex == 0) { // C: Cello (C2, G2, D3, A3)
                lowToHigh = { 36, 43, 50, 57 };
            } else { // Violin (G3, D4, A4, E5)
                const int baseRoot = rootNoteBaseMidi(instrument, stringCount, rootNoteIndex);
                lowToHigh = { baseRoot, baseRoot + 7, baseRoot + 14, baseRoot + 21 };
            }
        } else {
            // 5-string Violin/Viola (C3, G3, D4, A4, E5)
            const int baseRoot = rootNoteBaseMidi(instrument, stringCount, rootNoteIndex);
            lowToHigh = { baseRoot, baseRoot + 7, baseRoot + 14, baseRoot + 21, baseRoot + 28 };
        }
        for (size_t i = 0; i < lowToHigh.size(); ++i) {
            stringMidiNotes[i] = lowToHigh[lowToHigh.size() - 1 - i];
        }
    }

    void applyUkulelePreset() {
        if (tuningType == TuningType::Drop || rootNoteIndex == 7) {
            // Low-G tuning: G3 (55), C4 (60), E4 (64), A4 (69) -> String 1..4: A4, E4, C4, G3
            stringMidiNotes = { 69, 64, 60, 55 };
        } else if (rootNoteIndex == 2) {
            // D-tuning: A4 (69), D4 (62), F#4 (66), B4 (71) -> String 1..4: B4, F#4, D4, A4
            stringMidiNotes = { 71, 66, 62, 69 };
        } else {
            // Standard High-G: G4 (67), C4 (60), E4 (64), A4 (69) -> String 1..4: A4, E4, C4, G4
            stringMidiNotes = { 69, 64, 60, 67 };
        }
    }

    void applyBanjoPreset() {
        if (stringCount == 5) {
            if (tuningType == TuningType::Drop || rootNoteIndex == 0) {
                // Drop C: String 1=D4, 2=B3, 3=G3, 4=C3, 5=G4
                stringMidiNotes = { 62, 59, 55, 48, 67 };
            } else {
                // 5-string Open G: String 1=D4, 2=B3, 3=G3, 4=D3, 5=G4 (drone)
                stringMidiNotes = { 62, 59, 55, 50, 67 };
            }
        } else {
            if (rootNoteIndex == 7) {
                // Irish Tenor Banjo (GDAE): String 1=E4, 2=A3, 3=D3, 4=G2
                stringMidiNotes = { 64, 57, 50, 43 };
            } else {
                // Standard Tenor Banjo (CGDA): String 1=A4, 2=D4, 3=G3, 4=C3
                stringMidiNotes = { 69, 62, 55, 48 };
            }
        }
    }

    void applyLapSteelPreset() {
        if (stringCount == 6) {
            if (tuningType == TuningType::OpenD) {
                // Open D: D4, A3, F#3, D3, A2, D2
                stringMidiNotes = { 62, 57, 54, 50, 45, 38 };
            } else if (tuningType == TuningType::OpenG) {
                // Open G: D4, B3, G3, D3, B2, G2
                stringMidiNotes = { 62, 59, 55, 50, 47, 43 };
            } else if (tuningType == TuningType::OpenE) {
                // Open E: E4, B3, G#3, E3, B2, E2
                stringMidiNotes = { 64, 59, 56, 52, 47, 40 };
            } else {
                // C6: String 1=E4, 2=C4, 3=A3, 4=G3, 5=E3, 6=C3
                stringMidiNotes = { 64, 60, 57, 55, 52, 48 };
            }
        } else {
            // 8-string C6/A7: String 1=G4, 2=E4, 3=C4, 4=A3, 5=G3, 6=E3, 7=C3, 8=A2
            stringMidiNotes = { 67, 64, 60, 57, 55, 52, 48, 45 };
        }
    }
};

} // namespace praccy::tools
