#pragma once

#include "midi_types.h"
#include <windows.h>
#include <mmsystem.h>
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <array>
#include <cstdint>

namespace praccy::midi {

struct MidiDeviceInfo {
    uint32_t id{0};
    std::string name;
};

static constexpr size_t MAX_BINDINGS_PER_CC = 4;
static constexpr size_t NUM_MIDI_CHANNELS = 17; // 0 = Omni, 1..16 = Ch 1..16
static constexpr size_t NUM_CC_NUMBERS = 128;   // CC 0..127

struct FlatCcBinding {
    uint32_t paramId{0};
    float minVal{0.0f};
    float maxVal{1.0f};
    bool active{false};
    uint8_t count{0};
    MidiBinding bindings[MAX_BINDINGS_PER_CC]{};
    MidiBinding binding{};
};

using MidiBindingCallback = std::function<void(const MidiBinding& binding, float normalizedValue)>;

class MidiManager {
public:
    MidiManager();
    ~MidiManager();

    [[nodiscard]] static std::vector<MidiDeviceInfo> enumerateInputDevices();

    bool openDevice(uint32_t deviceId);
    void closeDevice();

    void addBinding(const MidiBinding& binding);
    void addCcBinding(uint8_t channel, uint8_t cc, uint32_t paramId, float minVal = 0.0f, float maxVal = 1.0f);
    void removeBinding(const std::string& id);
    void clearBindings();

    [[nodiscard]] const FlatCcBinding& getCcBinding(uint8_t channel, uint8_t cc) const noexcept;

    void startLearning(BindingTargetType target, int slot = -1, int branch = -1, int scene = -1);
    void cancelLearning();
    [[nodiscard]] bool isLearning() const noexcept { return m_learning.load(std::memory_order_relaxed); }

    void setBindingCallback(MidiBindingCallback callback) {
        m_bindingCallback = std::move(callback);
    }

    [[nodiscard]] std::string lastActivityDescription() const;

    /// O(1) MIDI CC dispatch with strictly zero mutex locks and zero heap allocations
    void handleControlChange(uint8_t channel, uint8_t cc, uint8_t val);

    /// General MIDI message processor
    void handleMidiMessage(uint8_t status, uint8_t d1, uint8_t d2);

    /// Flat O(1) lookup table: [0..16][0..127] where index 0 = Omni, 1..16 = Ch 1..16
    FlatCcBinding m_ccTable[NUM_MIDI_CHANNELS][NUM_CC_NUMBERS]{};

private:
    static void CALLBACK midiInProc(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2);
    void rebuildCcTable();

    HMIDIIN m_handle{nullptr};
    uint32_t m_currentDeviceId{0};
    std::vector<MidiBinding> m_bindings;
    mutable std::mutex m_bindingsMutex;

    MidiBindingCallback m_bindingCallback;

    std::atomic<bool> m_learning{false};
    MidiBinding m_pendingLearnBinding;

    std::atomic<uint32_t> m_lastRawMessage{0};
    std::atomic<uint64_t> m_lastMessageTimestamp{0};
};

} // namespace praccy::midi
