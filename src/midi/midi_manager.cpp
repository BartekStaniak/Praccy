#include "midi_manager.h"
#include <iostream>
#include <sstream>
#include <cstring>

namespace praccy::midi {

MidiManager::MidiManager() {
    for (auto& row : m_ccTable) {
        for (auto& slot : row) {
            slot = FlatCcBinding{};
        }
    }
}

MidiManager::~MidiManager() {
    closeDevice();
}

std::vector<MidiDeviceInfo> MidiManager::enumerateInputDevices() {
    std::vector<MidiDeviceInfo> devices;
    const UINT numDevs = midiInGetNumDevs();

    for (UINT i = 0; i < numDevs; ++i) {
        MIDIINCAPSA caps;
        if (midiInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            MidiDeviceInfo info;
            info.id = i;
            info.name = caps.szPname;
            devices.push_back(std::move(info));
        }
    }
    return devices;
}

bool MidiManager::openDevice(uint32_t deviceId) {
    closeDevice();

    MMRESULT res = midiInOpen(&m_handle, deviceId, reinterpret_cast<DWORD_PTR>(&MidiManager::midiInProc), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        std::cerr << "Failed to open MIDI device: " << deviceId << " (res=" << res << ")\n";
        return false;
    }

    m_currentDeviceId = deviceId;
    midiInStart(m_handle);
    return true;
}

void MidiManager::closeDevice() {
    if (m_handle) {
        midiInStop(m_handle);
        midiInReset(m_handle);
        midiInClose(m_handle);
        m_handle = nullptr;
    }
}

void MidiManager::addBinding(const MidiBinding& binding) {
    std::lock_guard<std::mutex> lock(m_bindingsMutex);
    m_bindings.push_back(binding);

    if (binding.triggerType == MidiMessageType::ControlChange) {
        const uint8_t ch = (binding.midiChannel <= 16) ? binding.midiChannel : 0;
        const uint8_t cc = binding.triggerNumber;
        if (cc < NUM_CC_NUMBERS) {
            m_ccTableSeq.fetch_add(1, std::memory_order_acquire);
            auto& slot = m_ccTable[ch][cc];
            slot.active = true;
            slot.paramId = static_cast<uint32_t>(binding.targetSlotIndex >= 0 ? binding.targetSlotIndex : 0);
            slot.minVal = 0.0f;
            slot.maxVal = 1.0f;
            slot.binding.fromMidiBinding(binding);
            if (slot.count < MAX_BINDINGS_PER_CC) {
                slot.bindings[slot.count++].fromMidiBinding(binding);
            }
            m_ccTableSeq.fetch_add(1, std::memory_order_release);
        }
    }
}

void MidiManager::addCcBinding(uint8_t channel, uint8_t cc, uint32_t paramId, float minVal, float maxVal) {
    if (channel >= NUM_MIDI_CHANNELS || cc >= NUM_CC_NUMBERS) return;
    std::lock_guard<std::mutex> lock(m_bindingsMutex);

    m_ccTableSeq.fetch_add(1, std::memory_order_acquire);
    auto& slot = m_ccTable[channel][cc];
    slot.active = true;
    slot.paramId = paramId;
    slot.minVal = minVal;
    slot.maxVal = maxVal;
    MidiBinding tempB;
    tempB.id = "cc_" + std::to_string(channel) + "_" + std::to_string(cc);
    tempB.targetType = BindingTargetType::SlotOutputGain;
    tempB.targetSlotIndex = -1;
    tempB.targetBranchIndex = -1;
    tempB.targetSceneIndex = -1;
    tempB.targetBranchSlotIndex = -1;
    tempB.paramId = paramId;
    tempB.midiChannel = channel;
    tempB.triggerType = MidiMessageType::ControlChange;
    tempB.triggerNumber = cc;
    slot.binding.fromMidiBinding(tempB);
    if (slot.count == 0) {
        slot.bindings[0] = slot.binding;
        slot.count = 1;
    }
    m_ccTableSeq.fetch_add(1, std::memory_order_release);
}

void MidiManager::rebuildCcTable() {
    m_ccTableSeq.fetch_add(1, std::memory_order_acquire); // Odd = write in progress
    for (auto& row : m_ccTable) {
        for (auto& slot : row) {
            slot = FlatCcBinding{};
        }
    }
    for (const auto& binding : m_bindings) {
        if (binding.triggerType == MidiMessageType::ControlChange) {
            const uint8_t ch = (binding.midiChannel <= 16) ? binding.midiChannel : 0;
            const uint8_t cc = binding.triggerNumber;
            if (cc < NUM_CC_NUMBERS) {
                auto& slot = m_ccTable[ch][cc];
                slot.active = true;
                slot.paramId = static_cast<uint32_t>(binding.targetSlotIndex >= 0 ? binding.targetSlotIndex : 0);
                slot.minVal = 0.0f;
                slot.maxVal = 1.0f;
                slot.binding.fromMidiBinding(binding);
                if (slot.count < MAX_BINDINGS_PER_CC) {
                    slot.bindings[slot.count++].fromMidiBinding(binding);
                }
            }
        }
    }
    m_ccTableSeq.fetch_add(1, std::memory_order_release); // Even = write complete
}

void MidiManager::removeBinding(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_bindingsMutex);
    std::erase_if(m_bindings, [&](const MidiBinding& b) { return b.id == id; });
    rebuildCcTable();
}

void MidiManager::clearBindings() {
    std::lock_guard<std::mutex> lock(m_bindingsMutex);
    m_bindings.clear();
    m_ccTableSeq.fetch_add(1, std::memory_order_acquire);
    for (auto& row : m_ccTable) {
        for (auto& slot : row) {
            slot = FlatCcBinding{};
        }
    }
    m_ccTableSeq.fetch_add(1, std::memory_order_release);
}

void MidiManager::stopLearnAndBind(uint8_t channel, uint8_t cc, uint32_t paramId, float minVal, float maxVal) {
    if (channel >= NUM_MIDI_CHANNELS || cc >= NUM_CC_NUMBERS) return;
    std::lock_guard<std::mutex> lock(m_bindingsMutex);

    int slot = 0;
    int branch = -1;
    int scene = -1;
    BindingTargetType target = BindingTargetType::SlotOutputGain;

    {
        std::lock_guard<std::mutex> learnLock(m_learnMutex);
        if (m_learning.load(std::memory_order_relaxed)) {
            slot = m_pendingLearnBinding.targetSlotIndex;
            branch = m_pendingLearnBinding.targetBranchIndex;
            scene = m_pendingLearnBinding.targetSceneIndex;
            target = m_pendingLearnBinding.targetType;
        }
    }

    std::string idStr = "cc_" + std::to_string(channel) + "_" + std::to_string(cc)
                      + "_s" + std::to_string(slot)
                      + "_b" + std::to_string(branch)
                      + "_sl" + std::to_string(scene)
                      + "_p" + std::to_string(paramId);

    MidiBinding b;
    b.id = idStr;
    b.targetType = target;
    b.targetSlotIndex = slot;
    b.targetBranchIndex = branch;
    b.targetSceneIndex = scene;
    b.targetBranchSlotIndex = -1;
    b.paramId = paramId;
    b.midiChannel = channel;
    b.triggerType = MidiMessageType::ControlChange;
    b.triggerNumber = cc;

    m_bindings.push_back(b);

    m_ccTableSeq.fetch_add(1, std::memory_order_acquire);
    auto& entry = m_ccTable[channel][cc];
    entry.active = true;
    entry.paramId = paramId;
    entry.minVal = minVal;
    entry.maxVal = maxVal;
    entry.binding.fromMidiBinding(b);
    if (entry.count < MAX_BINDINGS_PER_CC) {
        entry.bindings[entry.count++].fromMidiBinding(b);
    } else {
        entry.bindings[0].fromMidiBinding(b);
    }
    m_ccTableSeq.fetch_add(1, std::memory_order_release);

    m_learning.store(false, std::memory_order_release);
}

void MidiManager::stopLearnAndBind(uint32_t paramId) {
    stopLearnAndBind(1, 11, paramId, 0.0f, 1.0f);
}

const FlatCcBinding& MidiManager::getCcBinding(uint8_t channel, uint8_t cc) const noexcept {
    static const FlatCcBinding s_empty{};
    if (channel >= NUM_MIDI_CHANNELS || cc >= NUM_CC_NUMBERS) return s_empty;
    return m_ccTable[channel][cc];
}

void MidiManager::startLearning(BindingTargetType target, int slot, int branch, int scene) {
    {
        std::lock_guard<std::mutex> lock(m_learnMutex);
        m_pendingLearnBinding = MidiBinding{
            .id = "binding_" + std::to_string(static_cast<int>(target)) + "_" + std::to_string(slot),
            .targetType = target,
            .targetSlotIndex = slot,
            .targetBranchIndex = branch,
            .targetSceneIndex = scene,
            .targetBranchSlotIndex = -1,
            .paramId = 0,
            .midiChannel = 0,
            .triggerType = MidiMessageType::ControlChange,
            .triggerNumber = 0
        };
    }
    m_learning.store(true, std::memory_order_release);
}

void MidiManager::cancelLearning() {
    m_learning.store(false);
}

void CALLBACK MidiManager::midiInProc(HMIDIIN /*hMidiIn*/, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR /*dwParam2*/) {
    if (wMsg != MIM_DATA) return;

    auto* manager = reinterpret_cast<MidiManager*>(dwInstance);
    if (!manager) return;

    const uint8_t status = static_cast<uint8_t>(dwParam1 & 0xFF);
    const uint8_t d1 = static_cast<uint8_t>((dwParam1 >> 8) & 0xFF);
    const uint8_t d2 = static_cast<uint8_t>((dwParam1 >> 16) & 0xFF);

    manager->handleMidiMessage(status, d1, d2);
}

void MidiManager::handleControlChange(uint8_t channel, uint8_t cc, uint8_t val) {
    if (cc >= NUM_CC_NUMBERS) return;

    FlatCcBinding exactSlot{};
    FlatCcBinding omniSlot{};
    bool valid = false;

    // Complete lock-free seqlock read loop bounded to 2 retries (up to 3 attempts total)
    for (int retry = 0; retry < 3; ++retry) {
        const uint32_t seq1 = m_ccTableSeq.load(std::memory_order_acquire);
        if ((seq1 & 1u) != 0u) {
            continue;
        }

        if (channel >= 1 && channel <= 16) {
            exactSlot = m_ccTable[channel][cc];
        } else {
            exactSlot = FlatCcBinding{};
        }

        omniSlot = m_ccTable[0][cc];

        std::atomic_thread_fence(std::memory_order_acquire);

        if (m_ccTableSeq.load(std::memory_order_relaxed) == seq1) {
            valid = true;
            break;
        }
    }

    if (!valid) {
        return;
    }

    const float norm = static_cast<float>(val) / 127.0f;

    // If flat binding callback is set, invoke directly with zero heap allocations
    if (m_flatBindingCallback) {
        if (exactSlot.active) {
            const float mapped = exactSlot.minVal + norm * (exactSlot.maxVal - exactSlot.minVal);
            m_flatBindingCallback(exactSlot, mapped);
        }
        if (omniSlot.active) {
            const float mapped = omniSlot.minVal + norm * (omniSlot.maxVal - omniSlot.minVal);
            m_flatBindingCallback(omniSlot, mapped);
        }
    } else if (m_bindingCallback) {
        // Channel-specific lookup
        if (exactSlot.active) {
            if (exactSlot.count > 0) {
                for (uint8_t i = 0; i < exactSlot.count; ++i) {
                    m_bindingCallback(exactSlot.bindings[i].toMidiBinding(), norm);
                }
            } else {
                m_bindingCallback(exactSlot.binding.toMidiBinding(), norm);
            }
        }

        // Omni channel lookup
        if (omniSlot.active) {
            if (omniSlot.count > 0) {
                for (uint8_t i = 0; i < omniSlot.count; ++i) {
                    m_bindingCallback(omniSlot.bindings[i].toMidiBinding(), norm);
                }
            } else {
                m_bindingCallback(omniSlot.binding.toMidiBinding(), norm);
            }
        }
    }
}

void MidiManager::handleMidiMessage(uint8_t status, uint8_t d1, uint8_t d2) {
    const uint32_t raw = (static_cast<uint32_t>(status) << 16) | (static_cast<uint32_t>(d1) << 8) | d2;
    m_lastRawMessage.store(raw, std::memory_order_relaxed);

    MidiMessage msg{
        .status = status,
        .data1 = d1,
        .data2 = d2,
        .sampleOffset = 0
    };

    const uint8_t channel = msg.channel();
    const auto type = msg.type();

    // Handle MIDI Learn
    if (m_learning.load(std::memory_order_acquire)) {
        if (type == MidiMessageType::ControlChange || type == MidiMessageType::ProgramChange || type == MidiMessageType::NoteOn) {
            MidiBinding learned;
            {
                std::lock_guard<std::mutex> lock(m_learnMutex);
                m_pendingLearnBinding.midiChannel = channel;
                m_pendingLearnBinding.triggerType = type;
                m_pendingLearnBinding.triggerNumber = (type == MidiMessageType::ProgramChange) ? msg.programNumber() : d1;
                learned = m_pendingLearnBinding;
            }
            addBinding(learned);
            m_learning.store(false, std::memory_order_release);
            return;
        }
    }

    // High-performance O(1) dispatch for Control Change messages (zero mutex locks, zero allocations)
    if (type == MidiMessageType::ControlChange) {
        handleControlChange(channel, d1, d2);
        return;
    }

    // Fallback linear dispatch for non-CC messages (ProgramChange, NoteOn)
    std::lock_guard<std::mutex> lock(m_bindingsMutex);
    for (const auto& binding : m_bindings) {
        if (binding.midiChannel != 0 && binding.midiChannel != channel) continue;
        if (binding.triggerType != type) continue;

        if (type == MidiMessageType::ProgramChange && binding.triggerNumber == msg.programNumber()) {
            if (m_bindingCallback) m_bindingCallback(binding, 1.0f);
        } else if (type == MidiMessageType::NoteOn && binding.triggerNumber == d1) {
            const float norm = static_cast<float>(d2) / 127.0f;
            if (m_bindingCallback) m_bindingCallback(binding, norm);
        }
    }
}

std::string MidiManager::lastActivityDescription() const {
    const uint32_t raw = m_lastRawMessage.load(std::memory_order_relaxed);
    if (raw == 0) return "No MIDI Activity";

    const uint8_t status = static_cast<uint8_t>((raw >> 16) & 0xFF);
    const uint8_t d1 = static_cast<uint8_t>((raw >> 8) & 0xFF);
    const uint8_t d2 = static_cast<uint8_t>(raw & 0xFF);

    const auto type = static_cast<MidiMessageType>(status & 0xF0);
    const uint8_t ch = static_cast<uint8_t>((status & 0x0F) + 1);

    std::ostringstream ss;
    if (type == MidiMessageType::ControlChange) {
        ss << "Ch " << static_cast<int>(ch) << " CC#" << static_cast<int>(d1) << " = " << static_cast<int>(d2);
    } else if (type == MidiMessageType::ProgramChange) {
        ss << "Ch " << static_cast<int>(ch) << " PC#" << static_cast<int>(d1);
    } else if (type == MidiMessageType::NoteOn) {
        ss << "Ch " << static_cast<int>(ch) << " Note " << static_cast<int>(d1) << " Vel " << static_cast<int>(d2);
    } else {
        ss << "Ch " << static_cast<int>(ch) << " Status 0x" << std::hex << static_cast<int>(status);
    }
    return ss.str();
}

} // namespace praccy::midi
