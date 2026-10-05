#pragma once

#include "midi_types.h"
#include <windows.h>
#include <mmsystem.h>
#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>

namespace praccy::midi {

struct MidiDeviceInfo {
    uint32_t id{0};
    std::string name;
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
    void removeBinding(const std::string& id);
    void clearBindings();

    void startLearning(BindingTargetType target, int slot = -1, int branch = -1, int scene = -1);
    void cancelLearning();
    [[nodiscard]] bool isLearning() const noexcept { return m_learning.load(std::memory_order_relaxed); }

    void setBindingCallback(MidiBindingCallback callback) {
        m_bindingCallback = std::move(callback);
    }

    [[nodiscard]] std::string lastActivityDescription() const;

private:
    static void CALLBACK midiInProc(HMIDIIN hMidiIn, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2);
    void handleMidiMessage(uint8_t status, uint8_t d1, uint8_t d2);

    HMIDIIN m_handle{nullptr};
    uint32_t m_currentDeviceId{0};
    std::vector<MidiBinding> m_bindings;
    std::mutex m_bindingsMutex;

    MidiBindingCallback m_bindingCallback;

    std::atomic<bool> m_learning{false};
    MidiBinding m_pendingLearnBinding;

    std::atomic<uint32_t> m_lastRawMessage{0};
    std::atomic<uint64_t> m_lastMessageTimestamp{0};
};

} // namespace praccy::midi
