#pragma once

#include <cstdint>
#include <string>

namespace praccy::midi {

enum class MidiMessageType : uint8_t {
    NoteOff = 0x80,
    NoteOn = 0x90,
    PolyAftertouch = 0xA0,
    ControlChange = 0xB0,
    ProgramChange = 0xC0,
    ChannelAftertouch = 0xD0,
    PitchBend = 0xE0,
    Unknown = 0x00
};

struct MidiMessage {
    uint8_t status{0};
    uint8_t data1{0};
    uint8_t data2{0};
    uint32_t sampleOffset{0};

    [[nodiscard]] MidiMessageType type() const noexcept {
        return static_cast<MidiMessageType>(status & 0xF0);
    }

    [[nodiscard]] uint8_t channel() const noexcept {
        return (status & 0x0F) + 1; // 1-indexed (1-16)
    }

    [[nodiscard]] bool isControlChange() const noexcept {
        return type() == MidiMessageType::ControlChange;
    }

    [[nodiscard]] bool isProgramChange() const noexcept {
        return type() == MidiMessageType::ProgramChange;
    }

    [[nodiscard]] bool isNoteOn() const noexcept {
        return type() == MidiMessageType::NoteOn && data2 > 0;
    }

    [[nodiscard]] uint8_t ccNumber() const noexcept { return data1; }
    [[nodiscard]] uint8_t ccValue() const noexcept { return data2; }
    [[nodiscard]] uint8_t programNumber() const noexcept { return data1; }
};

enum class BindingTargetType {
    SlotBypass,
    SlotDryWet,
    SlotInputGain,
    SlotOutputGain,
    BranchMute,
    BranchSolo,
    BranchGain,
    BranchPan,
    SceneSelect,
    MasterVolume
};

struct MidiBinding {
    std::string id;
    BindingTargetType targetType;
    int targetSlotIndex{-1};
    int targetBranchIndex{-1};
    int targetSceneIndex{-1};

    uint8_t midiChannel{0}; // 0 = omni
    MidiMessageType triggerType{MidiMessageType::ControlChange};
    uint8_t triggerNumber{0}; // CC number or Program number
};

} // namespace praccy::midi
