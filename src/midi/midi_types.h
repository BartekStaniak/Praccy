#ifndef PRACCY_MIDI_TYPES_H
#define PRACCY_MIDI_TYPES_H

#if !defined(__INCLUDE_LEVEL__) || __INCLUDE_LEVEL__ > 0
#pragma once
#endif

#include <cstdint>
#include <string>
#include <functional>
#include <cstring>
#include <cstdio>
#include <type_traits>

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
        return static_cast<uint8_t>((status & 0x0F) + 1); // 1-indexed (1-16)
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
    MasterVolume,
    PluginParameter = 10
};

struct MidiBinding {
    std::string id;
    BindingTargetType targetType{BindingTargetType::SlotBypass};
    int targetSlotIndex{-1};
    int targetBranchIndex{-1};
    int targetSceneIndex{-1};
    int targetBranchSlotIndex{-1};
    uint32_t paramId{0};

    uint8_t midiChannel{0}; // 0 = omni
    MidiMessageType triggerType{MidiMessageType::ControlChange};
    uint8_t triggerNumber{0}; // CC number or Program number
};

static constexpr size_t MAX_BINDINGS_PER_CC = 4;
static constexpr size_t NUM_MIDI_CHANNELS = 17; // 0 = Omni, 1..16 = Ch 1..16
static constexpr size_t NUM_CC_NUMBERS = 128;   // CC 0..127

struct PodCcBinding {
    char id[16]{};
    uint64_t idHash{0};
    BindingTargetType targetType{BindingTargetType::SlotBypass};
    int targetSlotIndex{-1};
    int targetBranchIndex{-1};
    int targetSceneIndex{-1};
    int targetBranchSlotIndex{-1};
    uint32_t paramId{0};
    uint8_t midiChannel{0};
    MidiMessageType triggerType{MidiMessageType::ControlChange};
    uint8_t triggerNumber{0};

    [[nodiscard]] static constexpr uint64_t computeIdHash(std::string_view str) noexcept {
        uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : str) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    [[nodiscard]] bool matchesId(std::string_view queryId) const noexcept {
        return idHash != 0 && idHash == computeIdHash(queryId);
    }

    [[nodiscard]] MidiBinding toMidiBinding() const {
        char ssoId[16]{};
        const size_t len = std::strlen(id);
        if (len > 0 && len <= 15) {
            std::memcpy(ssoId, id, len + 1);
        } else {
            std::snprintf(ssoId, sizeof(ssoId), "c%X_%X_%d_%u",
                          static_cast<unsigned>(midiChannel & 0x1Fu),
                          static_cast<unsigned>(triggerNumber & 0x7Fu),
                          targetSlotIndex,
                          static_cast<unsigned>(targetType));
        }
        return MidiBinding{
            .id = ssoId,
            .targetType = targetType,
            .targetSlotIndex = targetSlotIndex,
            .targetBranchIndex = targetBranchIndex,
            .targetSceneIndex = targetSceneIndex,
            .targetBranchSlotIndex = targetBranchSlotIndex,
            .paramId = paramId,
            .midiChannel = midiChannel,
            .triggerType = triggerType,
            .triggerNumber = triggerNumber
        };
    }

    void fromMidiBinding(const MidiBinding& b) noexcept {
        std::memset(id, 0, sizeof(id));
        idHash = computeIdHash(b.id);
        if (!b.id.empty() && b.id.length() <= 15) {
            std::memcpy(id, b.id.data(), b.id.length());
        } else {
            std::snprintf(id, sizeof(id), "c%X_%X_%d_%u",
                          static_cast<unsigned>(b.midiChannel & 0x1Fu),
                          static_cast<unsigned>(b.triggerNumber & 0x7Fu),
                          b.targetSlotIndex,
                          static_cast<unsigned>(b.targetType));
        }
        targetType = b.targetType;
        targetSlotIndex = b.targetSlotIndex;
        targetBranchIndex = b.targetBranchIndex;
        targetSceneIndex = b.targetSceneIndex;
        targetBranchSlotIndex = b.targetBranchSlotIndex;
        paramId = b.paramId;
        midiChannel = b.midiChannel;
        triggerType = b.triggerType;
        triggerNumber = b.triggerNumber;
    }
};

struct FlatCcBinding {
    uint32_t paramId{0};
    float minVal{0.0f};
    float maxVal{1.0f};
    bool active{false};
    uint8_t count{0};
    PodCcBinding bindings[MAX_BINDINGS_PER_CC]{};
    PodCcBinding binding{};

    void clear() noexcept {
        paramId = 0;
        minVal = 0.0f;
        maxVal = 1.0f;
        active = false;
        count = 0;
        auto clearPod = [](PodCcBinding& b) noexcept {
            std::memset(b.id, 0, sizeof(b.id));
            b.idHash = 0;
            b.targetType = BindingTargetType::SlotBypass;
            b.targetSlotIndex = -1;
            b.targetBranchIndex = -1;
            b.targetSceneIndex = -1;
            b.targetBranchSlotIndex = -1;
            b.paramId = 0;
            b.midiChannel = 0;
            b.triggerType = MidiMessageType::ControlChange;
            b.triggerNumber = 0;
        };
        clearPod(binding);
        for (size_t i = 0; i < MAX_BINDINGS_PER_CC; ++i) {
            clearPod(bindings[i]);
        }
    }
};
static_assert(std::is_trivially_copyable_v<FlatCcBinding>, "FlatCcBinding must be 100% trivially copyable");

using MidiBindingCallback = std::function<void(const MidiBinding& binding, float normalizedValue)>;
using FlatBindingCallback = std::function<void(const FlatCcBinding& binding, float mappedValue)>;

} // namespace praccy::midi

#endif // PRACCY_MIDI_TYPES_H
