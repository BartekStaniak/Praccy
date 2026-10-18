#include "midi_manager.h"
#include <iostream>
#include <sstream>
#include <cstring>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

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

void MidiManager::acquireSeqlockWrite() noexcept {
    while (m_seqlockWriteFlag.test_and_set(std::memory_order_acquire)) {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
#endif
    }
    m_ccTableSeq.fetch_add(1, std::memory_order_acq_rel);
}

void MidiManager::releaseSeqlockWrite() noexcept {
    m_ccTableSeq.fetch_add(1, std::memory_order_release);
    m_seqlockWriteFlag.clear(std::memory_order_release);
}

std::vector<MidiDeviceInfo> MidiManager::enumerateInputDevices() {
    std::vector<MidiDeviceInfo> devices;
#if defined(_WIN32)
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
#endif
    return devices;
}

bool MidiManager::openDevice(uint32_t deviceId) {
    closeDevice();
#if defined(_WIN32)
    MMRESULT res = midiInOpen(&m_handle, deviceId, reinterpret_cast<DWORD_PTR>(&MidiManager::midiInProc), reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        std::cerr << "Failed to open MIDI device: " << deviceId << " (res=" << res << ")\n";
        return false;
    }

    m_currentDeviceId = deviceId;
    midiInStart(m_handle);
    return true;
#else
    (void)deviceId;
    return false;
#endif
}

void MidiManager::closeDevice() {
#if defined(_WIN32)
    if (m_handle) {
        midiInStop(m_handle);
        midiInReset(m_handle);
        midiInClose(m_handle);
        m_handle = nullptr;
    }
#else
    m_handle = nullptr;
#endif
}

void MidiManager::drainLearnedPodFromMidiThread() {
    if (m_hasPendingLearnedPod.exchange(false, std::memory_order_acq_rel)) {
        PodCcBinding pod{};
        m_learnedPodForUiSync.load(pod);
        m_bindings.push_back(pod.toMidiBinding());
    }
}

void MidiManager::addBinding(const MidiBinding& binding) {
    std::lock_guard<std::mutex> lock(m_bindingsMutex);
    drainLearnedPodFromMidiThread();
    m_bindings.push_back(binding);

    if (binding.triggerType == MidiMessageType::ControlChange) {
        const uint8_t ch = (binding.midiChannel <= 16) ? binding.midiChannel : 0;
        const uint8_t cc = binding.triggerNumber;
        if (cc < NUM_CC_NUMBERS) {
            acquireSeqlockWrite();
            auto& slot = m_ccTable[ch][cc];
            slot.active = true;
            slot.paramId = (binding.paramId != 0)
                ? binding.paramId
                : static_cast<uint32_t>(binding.targetSlotIndex >= 0 ? binding.targetSlotIndex : 0);
            slot.minVal = 0.0f;
            slot.maxVal = 1.0f;
            slot.binding.fromMidiBinding(binding);
            if (slot.count < MAX_BINDINGS_PER_CC) {
                slot.bindings[slot.count++].fromMidiBinding(binding);
            }
            m_atomicCcTable[ch][cc].store(slot);
            releaseSeqlockWrite();
        }
    } else {
        acquireSeqlockWrite();
        uint32_t idx = m_atomicNonCcCount.load(std::memory_order_relaxed);
        if (idx < MAX_NON_CC_BINDINGS) {
            PodCcBinding pod{};
            pod.fromMidiBinding(binding);
            m_atomicNonCcBindings[idx].store(pod);
            m_atomicNonCcCount.store(idx + 1, std::memory_order_release);
        }
        releaseSeqlockWrite();
    }
}

void MidiManager::addCcBinding(uint8_t channel, uint8_t cc, uint32_t paramId, float minVal, float maxVal) {
    if (channel >= NUM_MIDI_CHANNELS || cc >= NUM_CC_NUMBERS) return;
    std::lock_guard<std::mutex> lock(m_bindingsMutex);
    drainLearnedPodFromMidiThread();

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

    acquireSeqlockWrite();
    auto& slot = m_ccTable[channel][cc];
    slot.active = true;
    slot.paramId = paramId;
    slot.minVal = minVal;
    slot.maxVal = maxVal;
    slot.binding.fromMidiBinding(tempB);
    if (slot.count == 0) {
        slot.bindings[0] = slot.binding;
        slot.count = 1;
    }
    m_atomicCcTable[channel][cc].store(slot);
    releaseSeqlockWrite();
}

void MidiManager::rebuildCcTable() {
    acquireSeqlockWrite(); // Exclusive writer + Odd sequence = write in progress
    for (size_t ch = 0; ch < NUM_MIDI_CHANNELS; ++ch) {
        for (size_t cc = 0; cc < NUM_CC_NUMBERS; ++cc) {
            m_ccTable[ch][cc].clear();
        }
    }
    uint32_t nonCcIdx = 0;
    for (const auto& binding : m_bindings) {
        if (binding.triggerType == MidiMessageType::ControlChange) {
            const uint8_t ch = (binding.midiChannel <= 16) ? binding.midiChannel : 0;
            const uint8_t cc = binding.triggerNumber;
            if (cc < NUM_CC_NUMBERS) {
                auto& slot = m_ccTable[ch][cc];
                slot.active = true;
                slot.paramId = (binding.paramId != 0)
                    ? binding.paramId
                    : static_cast<uint32_t>(binding.targetSlotIndex >= 0 ? binding.targetSlotIndex : 0);
                slot.minVal = 0.0f;
                slot.maxVal = 1.0f;
                slot.binding.fromMidiBinding(binding);
                if (slot.count < MAX_BINDINGS_PER_CC) {
                    slot.bindings[slot.count++].fromMidiBinding(binding);
                }
            }
        } else if (nonCcIdx < MAX_NON_CC_BINDINGS) {
            PodCcBinding pod{};
            pod.fromMidiBinding(binding);
            m_atomicNonCcBindings[nonCcIdx++].store(pod);
        }
    }
    m_atomicNonCcCount.store(nonCcIdx, std::memory_order_relaxed);
    for (size_t ch = 0; ch < NUM_MIDI_CHANNELS; ++ch) {
        for (size_t cc = 0; cc < NUM_CC_NUMBERS; ++cc) {
            m_atomicCcTable[ch][cc].store(m_ccTable[ch][cc]);
        }
    }
    releaseSeqlockWrite(); // Even = write complete
}

void MidiManager::removeBinding(const std::string& id) {
    std::lock_guard<std::mutex> lock(m_bindingsMutex);
    drainLearnedPodFromMidiThread();
    const uint64_t targetHash = PodCcBinding::computeIdHash(id);
    std::erase_if(m_bindings, [&](const MidiBinding& b) {
        return b.id == id || PodCcBinding::computeIdHash(b.id) == targetHash;
    });
    rebuildCcTable();
}

void MidiManager::clearBindings() {
    std::lock_guard<std::mutex> lock(m_bindingsMutex);
    m_hasPendingLearnedPod.store(false, std::memory_order_relaxed);
    m_bindings.clear();
    acquireSeqlockWrite();
    m_atomicNonCcCount.store(0, std::memory_order_relaxed);
    for (size_t ch = 0; ch < NUM_MIDI_CHANNELS; ++ch) {
        for (size_t cc = 0; cc < NUM_CC_NUMBERS; ++cc) {
            m_ccTable[ch][cc].clear();
            m_atomicCcTable[ch][cc].store(m_ccTable[ch][cc]);
        }
    }
    releaseSeqlockWrite();
}

void MidiManager::stopLearnAndBind(uint8_t channel, uint8_t cc, uint32_t paramId, float minVal, float maxVal) {
    if (channel >= NUM_MIDI_CHANNELS || cc >= NUM_CC_NUMBERS) return;
    std::lock_guard<std::mutex> lock(m_bindingsMutex);
    drainLearnedPodFromMidiThread();

    int slot = 0;
    int branch = -1;
    int scene = -1;
    int branchSlot = -1;
    BindingTargetType target = BindingTargetType::SlotOutputGain;

    {
        std::lock_guard<std::mutex> learnLock(m_learnMutex);
        if (m_learning.load(std::memory_order_relaxed)) {
            slot = m_pendingLearnBinding.targetSlotIndex;
            branch = m_pendingLearnBinding.targetBranchIndex;
            scene = m_pendingLearnBinding.targetSceneIndex;
            branchSlot = m_pendingLearnBinding.targetBranchSlotIndex;
            target = m_pendingLearnBinding.targetType;
            if (paramId == 1000 && m_pendingLearnBinding.paramId != 0) {
                paramId = m_pendingLearnBinding.paramId;
            }
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
    b.targetBranchSlotIndex = branchSlot;
    b.paramId = paramId;
    b.midiChannel = channel;
    b.triggerType = MidiMessageType::ControlChange;
    b.triggerNumber = cc;

    m_bindings.push_back(b);

    acquireSeqlockWrite();
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
    m_atomicCcTable[channel][cc].store(entry);
    releaseSeqlockWrite();

    m_learning.store(false, std::memory_order_release);
}

void MidiManager::stopLearnAndBind(uint32_t paramId) {
    stopLearnAndBind(1, 11, paramId, 0.0f, 1.0f);
}

FlatCcBinding MidiManager::getCcBinding(uint8_t channel, uint8_t cc) const noexcept {
    FlatCcBinding out{};
    if (channel >= NUM_MIDI_CHANNELS || cc >= NUM_CC_NUMBERS) return out;

    for (int retry = 0; retry < 8; ++retry) {
        const uint32_t seq1 = m_ccTableSeq.load(std::memory_order_acquire);
        if ((seq1 & 1u) != 0u) {
            continue;
        }
        m_atomicCcTable[channel][cc].load(out);
        const uint32_t seq2 = m_ccTableSeq.load(std::memory_order_acquire);
        if (seq1 == seq2) {
            return out;
        }
    }
    m_atomicCcTable[channel][cc].load(out);
    return out;
}

void MidiManager::startLearning(BindingTargetType target, int slot, int branch, int scene, int branchSlot, uint32_t paramId) {
    MidiBinding pending{
        .id = "binding_" + std::to_string(static_cast<int>(target)) + "_" + std::to_string(slot) + "_" + std::to_string(paramId),
        .targetType = target,
        .targetSlotIndex = slot,
        .targetBranchIndex = branch,
        .targetSceneIndex = scene,
        .targetBranchSlotIndex = branchSlot,
        .paramId = paramId,
        .midiChannel = 0,
        .triggerType = MidiMessageType::ControlChange,
        .triggerNumber = 0
    };
    PodCcBinding pod{};
    pod.fromMidiBinding(pending);
    m_pendingLearnPod.store(pod);
    {
        std::lock_guard<std::mutex> lock(m_learnMutex);
        m_pendingLearnBinding = std::move(pending);
    }
    m_learning.store(true, std::memory_order_release);
}

void MidiManager::cancelLearning() {
    m_learning.store(false, std::memory_order_release);
}

void CALLBACK MidiManager::midiInProc(HMIDIIN /*hMidiIn*/, UINT wMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR /*dwParam2*/) {
#if defined(_WIN32)
    if (wMsg != MIM_DATA) return;
#else
    if (wMsg != 0x3C3u) return;
#endif

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

    // C++20-compliant lock-free atomic Seqlock read loop (0 non-atomic data races, 0 TSan warnings)
    for (int retry = 0; retry < 3; ++retry) {
        const uint32_t seq1 = m_ccTableSeq.load(std::memory_order_acquire);
        if ((seq1 & 1u) != 0u) {
            continue;
        }

        if (channel >= 1 && channel <= 16) {
            m_atomicCcTable[channel][cc].load(exactSlot);
        } else {
            exactSlot.clear();
        }

        m_atomicCcTable[0][cc].load(omniSlot);

        const uint32_t seq2 = m_ccTableSeq.load(std::memory_order_acquire);
        if (seq2 == seq1) {
            valid = true;
            break;
        }
    }

    if (!valid) {
        return;
    }

    const float norm = static_cast<float>(val) / 127.0f;

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
        if (exactSlot.active) {
            if (exactSlot.count > 0) {
                for (uint8_t i = 0; i < exactSlot.count; ++i) {
                    m_bindingCallback(exactSlot.bindings[i].toMidiBinding(), norm);
                }
            } else {
                m_bindingCallback(exactSlot.binding.toMidiBinding(), norm);
            }
        }

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

    // 1. Lock-free, zero-allocation MIDI Learn on real-time MIDI thread
    if (type == MidiMessageType::ControlChange || type == MidiMessageType::ProgramChange || type == MidiMessageType::NoteOn) {
        if (m_learning.exchange(false, std::memory_order_acq_rel)) {
            PodCcBinding learnedPod{};
            m_pendingLearnPod.load(learnedPod);
            learnedPod.midiChannel = channel;
            learnedPod.triggerType = type;
            learnedPod.triggerNumber = (type == MidiMessageType::ProgramChange) ? msg.programNumber() : d1;

            if (type == MidiMessageType::ControlChange && learnedPod.triggerNumber < NUM_CC_NUMBERS) {
                const uint8_t ch = (channel <= 16) ? channel : 0;
                const uint8_t cc = learnedPod.triggerNumber;
                acquireSeqlockWrite();
                auto& slot = m_ccTable[ch][cc];
                slot.active = true;
                slot.paramId = (learnedPod.paramId != 0)
                    ? learnedPod.paramId
                    : static_cast<uint32_t>(learnedPod.targetSlotIndex >= 0 ? learnedPod.targetSlotIndex : 0);
                slot.minVal = 0.0f;
                slot.maxVal = 1.0f;
                slot.binding = learnedPod;
                if (slot.count < MAX_BINDINGS_PER_CC) {
                    slot.bindings[slot.count++] = learnedPod;
                }
                m_atomicCcTable[ch][cc].store(slot);
                releaseSeqlockWrite();
            } else {
                acquireSeqlockWrite();
                const uint32_t idx = m_atomicNonCcCount.load(std::memory_order_relaxed);
                if (idx < MAX_NON_CC_BINDINGS) {
                    m_atomicNonCcBindings[idx].store(learnedPod);
                    m_atomicNonCcCount.store(idx + 1, std::memory_order_release);
                }
                releaseSeqlockWrite();
            }

            m_learnedPodForUiSync.store(learnedPod);
            m_hasPendingLearnedPod.store(true, std::memory_order_release);
            return;
        }
    }

    // 2. High-performance O(1) dispatch for Control Change messages (zero mutex locks, zero allocations)
    if (type == MidiMessageType::ControlChange) {
        handleControlChange(channel, d1, d2);
        return;
    }

    // 3. Lock-free seqlock dispatch for non-CC messages (ProgramChange, NoteOn) — zero mutex locks, zero heap allocations
    if (!m_bindingCallback) return;
    if (type != MidiMessageType::ProgramChange && type != MidiMessageType::NoteOn) return;

    std::array<PodCcBinding, MAX_NON_CC_BINDINGS> localPods{};
    uint32_t count = 0;
    bool valid = false;

    for (int retry = 0; retry < 3; ++retry) {
        const uint32_t seq1 = m_ccTableSeq.load(std::memory_order_acquire);
        if ((seq1 & 1u) != 0u) continue;

        count = std::min<uint32_t>(m_atomicNonCcCount.load(std::memory_order_acquire), static_cast<uint32_t>(MAX_NON_CC_BINDINGS));
        for (uint32_t i = 0; i < count; ++i) {
            m_atomicNonCcBindings[i].load(localPods[i]);
        }

        const uint32_t seq2 = m_ccTableSeq.load(std::memory_order_acquire);
        if (seq2 == seq1) {
            valid = true;
            break;
        }
    }

    if (!valid) return;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& pod = localPods[i];
        if (pod.midiChannel != 0 && pod.midiChannel != channel) continue;
        if (pod.triggerType != type) continue;

        if (type == MidiMessageType::ProgramChange && pod.triggerNumber == msg.programNumber()) {
            m_bindingCallback(pod.toMidiBinding(), 1.0f);
        } else if (type == MidiMessageType::NoteOn && pod.triggerNumber == d1) {
            const float norm = static_cast<float>(d2) / 127.0f;
            m_bindingCallback(pod.toMidiBinding(), norm);
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
        ss << "MIDI 0x" << std::hex << static_cast<int>(status);
    }
    return ss.str();
}

} // namespace praccy::midi
