#include "graph_engine.h"
#include "../plugins/plugin_base.h"
#include <cmath>
#include <algorithm>

namespace praccy::audio {

// ==========================================
// PluginSlot Implementation
// ==========================================

PluginSlot::PluginSlot(std::unique_ptr<AudioNode> innerNode)
    : m_innerNode(std::move(innerNode)) {
    if (m_innerNode) {
        m_pluginPtr = dynamic_cast<plugins::IPluginInstance*>(m_innerNode.get());
        refreshCachedParameterRanges();
    }
    m_rtInnerNode.store(m_innerNode.get(), std::memory_order_release);
    m_rtPluginPtr.store(m_pluginPtr, std::memory_order_release);
}

void PluginSlot::replaceInnerNode(std::unique_ptr<AudioNode> newInner, std::unique_ptr<AudioNode>& outOld) noexcept {
    AudioNode* nextNode = newInner.get();
    auto* nextPlugin = dynamic_cast<plugins::IPluginInstance*>(nextNode);

    // Fence in-flight PluginSlot::process() execution so (m_rtPluginPtr, m_rtInnerNode) never tear
    // and outOld is guaranteed quiescent before returning to caller.
    m_slotSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
    while (m_slotActiveCallbacks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }

    m_rtPluginPtr.store(nextPlugin, std::memory_order_release);
    m_rtInnerNode.store(nextNode, std::memory_order_release);

    m_pluginPtr = nextPlugin;
    refreshCachedParameterRanges();

    outOld = std::move(m_innerNode);
    m_innerNode = std::move(newInner);

    m_slotSuspendDepth.fetch_sub(1, std::memory_order_release);
}

void PluginSlot::refreshCachedParameterRanges() noexcept {
    // Zero m_numParamRanges first with seq_cst barrier so concurrent MIDI readers in
    // setParameterNormalized() never read partially overwritten m_paramRanges entries.
    m_numParamRanges.store(0, std::memory_order_seq_cst);

    if (!m_pluginPtr) {
        return;
    }
    const size_t total = std::min(m_pluginPtr->numParameters(), MAX_CACHED_PARAM_RANGES);
    for (size_t i = 0; i < total; ++i) {
        auto desc = m_pluginPtr->getParameterDesc(i);
        m_paramRanges[i].id.store(desc.id, std::memory_order_relaxed);
        m_paramRanges[i].minVal.store(desc.minValue, std::memory_order_relaxed);
        m_paramRanges[i].maxVal.store(desc.maxValue, std::memory_order_release);
    }
    m_numParamRanges.store(total, std::memory_order_release);
}

void PluginSlot::setParameterNormalized(uint32_t paramId, float normalizedValue) noexcept {
    const float clampedNorm = std::clamp(normalizedValue, 0.0f, 1.0f);
    float scaledVal = clampedNorm;

    const size_t count = m_numParamRanges.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        if (m_paramRanges[i].id.load(std::memory_order_relaxed) == paramId) {
            const float minV = m_paramRanges[i].minVal.load(std::memory_order_relaxed);
            const float maxV = m_paramRanges[i].maxVal.load(std::memory_order_acquire);
            if (maxV > minV) {
                scaledVal = minV + clampedNorm * (maxV - minV);
            }
            break;
        }
    }
    m_automationRegister.setParameter(paramId, scaledVal);
}

void PluginSlot::prepare(double sampleRate, uint32_t maxBlockSize) {
    m_slotSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
    while (m_slotActiveCallbacks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;
    AudioNode* inner = m_rtInnerNode.load(std::memory_order_relaxed);
    if (inner) {
        inner->prepare(sampleRate, maxBlockSize);
        refreshCachedParameterRanges();
    }
    m_dryBuffer.resize(2, maxBlockSize);
    m_processBuffer.resize(2, maxBlockSize);
    m_inputScaledBuffer.resize(2, maxBlockSize);
    // 10ms smooth ramp at current sample rate
    m_bypassRamp.reset(static_cast<uint32_t>(sampleRate * 0.010));
    m_slotSuspendDepth.fetch_sub(1, std::memory_order_release);
}

void PluginSlot::reset() {
    m_slotSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
    while (m_slotActiveCallbacks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    AudioNode* inner = m_rtInnerNode.load(std::memory_order_relaxed);
    if (inner) {
        inner->reset();
    }
    m_automationRegister.reset();
    m_meter.reset();
    m_slotSuspendDepth.fetch_sub(1, std::memory_order_release);
}

const std::string& PluginSlot::name() const noexcept {
    static const std::string emptyName = "Empty Slot";
    AudioNode* inner = m_rtInnerNode.load(std::memory_order_acquire);
    if (inner) return inner->name();
    return m_innerNode ? m_innerNode->name() : emptyName;
}

void PluginSlot::setBypassed(bool bypassed) noexcept {
    AudioNode::setBypassed(bypassed);
    m_bypassRamp.startTransition(!bypassed);
}

bool PluginSlot::isBypassed() const noexcept {
    return AudioNode::isBypassed();
}

void PluginSlot::process(AudioProcessContext& ctx) {
    const uint32_t numSamples = ctx.numSamples;
    if (numSamples == 0 || m_slotSuspendDepth.load(std::memory_order_acquire) != 0) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    m_slotActiveCallbacks.fetch_add(1, std::memory_order_seq_cst);
    struct SlotActiveGuard {
        std::atomic<uint32_t>& counter;
        ~SlotActiveGuard() noexcept {
            counter.fetch_sub(1, std::memory_order_release);
        }
    } slotGuard{m_slotActiveCallbacks};

    if (m_slotSuspendDepth.load(std::memory_order_seq_cst) != 0) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    AudioNode* inner = m_rtInnerNode.load(std::memory_order_acquire);
    if (!inner) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    // 1. Drain pending parameter automation wait-free at block boundary
    plugins::IPluginInstance* plug = m_rtPluginPtr.load(std::memory_order_acquire);
    m_automationRegister.drainDirty([&](uint32_t paramId, float value) {
        if (plug) {
            plug->setParameterFromAudioThread(paramId, value);
        }
    });

    const bool currentlyBypassed = isBypassed();
    const bool transitioning = m_bypassRamp.isTransitioning();

    // If fully bypassed, faulted, or not transitioning, pure pass-through
    if ((currentlyBypassed && !transitioning) || inner->isFaulted()) {
        ctx.output.copyFrom(ctx.input);
        m_meter.process(ctx.output.channel(0), ctx.output.numChannels() > 1 ? ctx.output.channel(1) : nullptr, numSamples, ctx.sampleRate);
        return;
    }

    // Save dry signal and prepare distinct, non-aliasing input/output buffers for plugin
    auto dryView = m_dryBuffer.view(numSamples);
    dryView.copyFrom(ctx.input);
    const float inGain = DspUtils::dbToGain(m_inputGainDb.load(std::memory_order_relaxed));

    AudioBufferView innerInput = dryView;
    if (std::abs(inGain - 1.0f) > 1e-6f) {
        auto scaledView = m_inputScaledBuffer.view(numSamples);
        scaledView.copyFrom(ctx.input);
        scaledView.applyGain(inGain);
        innerInput = scaledView;
    }

    auto procView = m_processBuffer.view(numSamples);
    procView.clear();

    // Process through inner plugin node with non-aliasing input (innerInput) and output (procView), preserving transport context
    AudioProcessContext innerCtx = ctx;
    innerCtx.input = innerInput;
    innerCtx.output = procView;
    innerCtx.sampleRate = ctx.sampleRate;
    innerCtx.numSamples = numSamples;
    inner->process(innerCtx);
    DspUtils::ScopedDenormalGuard::reassert();

    // Scrub NaN/Inf and clamp runaway spikes from third-party plugins
    for (uint32_t ch = 0; ch < procView.numChannels(); ++ch) {
        DspUtils::sanitizeChannel(procView.channel(ch), numSamples);
    }

    // If plugin faulted during execution, safely bypass with dry signal
    if (inner->isFaulted()) {
        ctx.output.copyFrom(dryView);
        m_meter.process(ctx.output.channel(0), ctx.output.numChannels() > 1 ? ctx.output.channel(1) : nullptr, numSamples, ctx.sampleRate);
        return;
    }

    // Apply output trim
    const float outGain = DspUtils::dbToGain(m_outputGainDb.load(std::memory_order_relaxed));
    procView.applyGain(outGain);

    // Apply Dry/Wet mix
    const float mix = m_dryWet.load(std::memory_order_relaxed);
    float dryGain = 1.0f, wetGain = 0.0f;
    DspUtils::calculateEqualPowerCrossfade(mix, dryGain, wetGain);

    const uint32_t numCh = ctx.output.numChannels();
    for (uint32_t s = 0; s < numSamples; ++s) {
        float rampOld = 0.0f, rampNew = 0.0f;
        if (transitioning) {
            m_bypassRamp.getNextGains(rampOld, rampNew);
        }

        for (uint32_t ch = 0; ch < numCh; ++ch) {
            float* out = ctx.output.channel(ch);
            const float* dry = dryView.channel(ch);
            const float* wet = procView.channel(ch);

            float blended = (dry[s] * dryGain) + (wet[s] * wetGain);
            if (transitioning) {
                // When activating: rampNew is active plugin, rampOld is dry bypass
                blended = (dry[s] * rampOld) + (blended * rampNew);
            }
            out[s] = blended;
        }
    }

    m_meter.process(ctx.output.channel(0), numCh > 1 ? ctx.output.channel(1) : nullptr, numSamples, ctx.sampleRate);
}

// ==========================================
// ParallelBranch Implementation
// ==========================================

ParallelBranch::ParallelBranch(std::string name)
    : m_name(std::move(name)) {
    m_uiSlots.reserve(MAX_BRANCH_SLOTS);
    m_activeSlots.reserve(MAX_BRANCH_SLOTS);
    m_deferredReclaim.reserve(DEFERRED_RECLAIM_CAPACITY);
}

ParallelBranch::~ParallelBranch() {
    m_branchSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
    while (m_branchActiveCallbacks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    m_isAudioRunning.store(false, std::memory_order_release);

    // 1. Drain any remaining items from command queue
    SlotCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        cmd.slot.reset();
    }

    // 2. Drain reclamation queue
    collectReclaimedSlots();

    // 3. Clear active slots and deferred reclaim
    m_activeSlots.clear();
    {
        std::lock_guard<std::mutex> lock(m_slotMutex);
        m_uiSlots.clear();
        syncRtSlotsLocked();
    }
    for (auto& s : m_deferredReclaim) {
        s.reset();
    }
    m_deferredReclaim.clear();
    m_branchSuspendDepth.fetch_sub(1, std::memory_order_release);
}

void ParallelBranch::syncRtSlotsLocked() noexcept {
    const size_t count = std::min(m_uiSlots.size(), MAX_BRANCH_SLOTS);
    for (size_t i = 0; i < count; ++i) {
        m_rtSlots[i].store(m_uiSlots[i], std::memory_order_release);
    }
    for (size_t i = count; i < MAX_BRANCH_SLOTS; ++i) {
        m_rtSlots[i].store(nullptr, std::memory_order_release);
    }
    m_rtNumSlots.store(count, std::memory_order_release);
}

size_t ParallelBranch::numSlots() const noexcept {
    std::lock_guard<std::mutex> lock(m_slotMutex);
    return m_uiSlots.size();
}

PluginSlot* ParallelBranch::getSlot(size_t index) noexcept {
    std::lock_guard<std::mutex> lock(m_slotMutex);
    return (index < m_uiSlots.size()) ? m_uiSlots[index] : nullptr;
}

void ParallelBranch::addSlotToUiShadow(PluginSlot* slot) {
    std::lock_guard<std::mutex> lock(m_slotMutex);
    m_uiSlots.push_back(slot);
    syncRtSlotsLocked();
}

void ParallelBranch::prepare(double sampleRate, uint32_t maxBlockSize) {
    m_branchSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
    while (m_branchActiveCallbacks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    m_isAudioRunning.store(false, std::memory_order_release);
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_branchBuffer.resize(2, maxBlockSize);
    m_slotTemp.resize(2, maxBlockSize);
    m_activeSlots.reserve(MAX_BRANCH_SLOTS);
    m_deferredReclaim.reserve(DEFERRED_RECLAIM_CAPACITY);

    // Pre-allocate PDC stereo circular delay buffers strictly in prepare() (16,384 samples)
    m_delayBufferL.assign(PDC_BUFFER_SIZE, 0.0f);
    m_delayBufferR.assign(PDC_BUFFER_SIZE, 0.0f);
    m_delayWritePos = 0;
    m_currentDelay = 0;
    m_targetDelay = 0;
    m_oldDelay = 0;
    m_crossfadePos = 0;
    m_isCrossfading = false;
    m_branchLatency.store(0, std::memory_order_relaxed);

    // Drain any pending commands before preparing
    drainSlotCommandsIntoActive();

    for (auto& slot : m_activeSlots) {
        if (slot) slot->prepare(sampleRate, maxBlockSize);
    }
    m_branchSuspendDepth.fetch_sub(1, std::memory_order_release);
}

void ParallelBranch::reset() {
    m_branchSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
    while (m_branchActiveCallbacks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    m_isAudioRunning.store(false, std::memory_order_release);
    std::fill(m_delayBufferL.begin(), m_delayBufferL.end(), 0.0f);
    std::fill(m_delayBufferR.begin(), m_delayBufferR.end(), 0.0f);
    m_delayWritePos = 0;
    m_currentDelay = 0;
    m_targetDelay = 0;
    m_oldDelay = 0;
    m_crossfadePos = 0;
    m_isCrossfading = false;
    m_branchLatency.store(0, std::memory_order_relaxed);
    m_meter.reset();

    for (auto& slot : m_activeSlots) {
        if (slot) slot->reset();
    }
    m_branchSuspendDepth.fetch_sub(1, std::memory_order_release);
}

void ParallelBranch::setTargetDelay(uint32_t target) noexcept {
    target = std::min<uint32_t>(target, static_cast<uint32_t>(PDC_BUFFER_SIZE));
    if (target != m_currentDelay && !m_isCrossfading) {
        m_oldDelay = m_currentDelay;
        m_targetDelay = target;
        m_crossfadePos = 0;
        m_isCrossfading = true;
    } else if (target != m_targetDelay && m_isCrossfading) {
        m_targetDelay = target;
    } else if (target == m_currentDelay) {
        m_targetDelay = target;
        m_isCrossfading = false;
    }
}

uint32_t ParallelBranch::totalLatencySamples() const noexcept {
    uint32_t total = 0;
    for (const auto& slot : m_activeSlots) {
        if (slot) total += slot->latencySamples();
    }
    return total;
}

void ParallelBranch::addSlot(std::unique_ptr<PluginSlot> slot) {
    if (!slot) return;
    collectReclaimedSlots();

    // Guarantee that slot scratch buffers are allocated on UI thread before entering realtime queue
    slot->prepare(m_sampleRate, m_maxBlockSize);

    std::lock_guard<std::mutex> lock(m_slotMutex);
    PluginSlot* rawPtr = slot.get();
    m_uiSlots.push_back(rawPtr);
    syncRtSlotsLocked();

    SlotCommand cmd = SlotCommand::makeAdd(std::move(slot));
    if (!m_commandQueue.try_enqueue(std::move(cmd))) {
        m_uiSlots.pop_back();
        syncRtSlotsLocked();
    }
}

void ParallelBranch::insertSlot(size_t index, std::unique_ptr<PluginSlot> slot) {
    if (!slot) return;
    collectReclaimedSlots();

    slot->prepare(m_sampleRate, m_maxBlockSize);

    std::lock_guard<std::mutex> lock(m_slotMutex);
    PluginSlot* rawPtr = slot.get();
    index = std::min(index, m_uiSlots.size());
    m_uiSlots.insert(m_uiSlots.begin() + index, rawPtr);
    syncRtSlotsLocked();

    SlotCommand cmd = SlotCommand::makeInsert(index, std::move(slot));
    if (!m_commandQueue.try_enqueue(std::move(cmd))) {
        m_uiSlots.erase(m_uiSlots.begin() + index);
        syncRtSlotsLocked();
    }
}

void ParallelBranch::removeSlot(size_t index) {
    collectReclaimedSlots();
    PluginSlot* targetPtr = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_slotMutex);
        if (index >= m_uiSlots.size()) return;
        targetPtr = m_uiSlots[index];
        m_uiSlots.erase(m_uiSlots.begin() + index);
        syncRtSlotsLocked();
    }

    SlotCommand cmd = SlotCommand::makeRemove(index, targetPtr);
    m_commandQueue.try_enqueue(std::move(cmd));
}

void ParallelBranch::reorderSlots(size_t fromIndex, size_t toIndex) {
    collectReclaimedSlots();
    {
        std::lock_guard<std::mutex> lock(m_slotMutex);
        if (fromIndex >= m_uiSlots.size() || toIndex >= m_uiSlots.size() || fromIndex == toIndex) return;
        auto* ptr = m_uiSlots[fromIndex];
        m_uiSlots.erase(m_uiSlots.begin() + fromIndex);
        m_uiSlots.insert(m_uiSlots.begin() + toIndex, ptr);
        syncRtSlotsLocked();
    }

    SlotCommand cmd = SlotCommand::makeReorder(fromIndex, toIndex);
    m_commandQueue.try_enqueue(std::move(cmd));
}

std::unique_ptr<PluginSlot> ParallelBranch::takeSlot(size_t index) {
    if (m_isAudioRunning.load(std::memory_order_acquire)) {
        return nullptr;
    }
    m_branchSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
    while (m_branchActiveCallbacks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    struct BranchFenceGuard {
        std::atomic<uint32_t>& depth;
        ~BranchFenceGuard() noexcept { depth.fetch_sub(1, std::memory_order_release); }
    } fenceGuard{m_branchSuspendDepth};

    collectReclaimedSlots();
    std::lock_guard<std::mutex> lock(m_slotMutex);

    // 1. If command queue has unconsumed commands, drain them into active slots
    drainSlotCommandsIntoActive();

    // 2. Identify target slot from UI shadow
    PluginSlot* targetPtr = nullptr;
    if (index < m_uiSlots.size()) {
        targetPtr = m_uiSlots[index];
        m_uiSlots.erase(m_uiSlots.begin() + index);
        syncRtSlotsLocked();
    }

    // 3. Extract and return from active slots
    if (targetPtr) {
        auto it = std::find_if(m_activeSlots.begin(), m_activeSlots.end(),
            [&](const auto& s) { return s.get() == targetPtr; });
        if (it != m_activeSlots.end()) {
            auto ptr = std::move(*it);
            m_activeSlots.erase(it);
            return ptr;
        }
    }

    if (index < m_activeSlots.size()) {
        auto ptr = std::move(m_activeSlots[index]);
        m_activeSlots.erase(m_activeSlots.begin() + index);
        return ptr;
    }

    return nullptr;
}

void ParallelBranch::collectReclaimedSlots() noexcept {
    std::unique_ptr<PluginSlot> reclaimed;
    while (m_reclaimQueue.try_dequeue(reclaimed)) {
        reclaimed.reset();
    }
}

void ParallelBranch::stashForReclamation(std::unique_ptr<PluginSlot> slot) noexcept {
    if (!slot) return;
    if (m_reclaimQueue.try_enqueue(std::move(slot))) {
        return;
    }
    if (m_deferredReclaim.size() < m_deferredReclaim.capacity()) {
        m_deferredReclaim.push_back(std::move(slot));
    }
}

void ParallelBranch::flushStashedReclamations() noexcept {
    if (!m_deferredReclaim.empty()) {
        size_t writeIdx = 0;
        for (size_t i = 0; i < m_deferredReclaim.size(); ++i) {
            if (!m_reclaimQueue.try_enqueue(std::move(m_deferredReclaim[i]))) {
                if (writeIdx != i) {
                    m_deferredReclaim[writeIdx] = std::move(m_deferredReclaim[i]);
                }
                ++writeIdx;
            }
        }
        m_deferredReclaim.resize(writeIdx);
    }
}

void ParallelBranch::drainSlotCommandsIntoActive() noexcept {
    flushStashedReclamations();

    SlotCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        switch (cmd.type) {
            case SlotCommandType::Add: {
                if (cmd.slot) {
                    if (m_activeSlots.size() < MAX_BRANCH_SLOTS) {
                        m_activeSlots.push_back(std::move(cmd.slot));
                    } else {
                        if (!m_reclaimQueue.try_enqueue(std::move(cmd.slot))) {
                            stashForReclamation(std::move(cmd.slot));
                        }
                    }
                }
                break;
            }
            case SlotCommandType::Insert: {
                if (cmd.slot) {
                    if (m_activeSlots.size() < MAX_BRANCH_SLOTS) {
                        size_t idx = std::min(cmd.index, m_activeSlots.size());
                        m_activeSlots.insert(m_activeSlots.begin() + static_cast<ptrdiff_t>(idx), std::move(cmd.slot));
                    } else {
                        if (!m_reclaimQueue.try_enqueue(std::move(cmd.slot))) {
                            stashForReclamation(std::move(cmd.slot));
                        }
                    }
                }
                break;
            }
            case SlotCommandType::Reorder: {
                if (cmd.index < m_activeSlots.size() && cmd.targetIndex < m_activeSlots.size() && cmd.index != cmd.targetIndex) {
                    auto s = std::move(m_activeSlots[cmd.index]);
                    m_activeSlots.erase(m_activeSlots.begin() + static_cast<ptrdiff_t>(cmd.index));
                    m_activeSlots.insert(m_activeSlots.begin() + static_cast<ptrdiff_t>(cmd.targetIndex), std::move(s));
                }
                break;
            }
            case SlotCommandType::Remove: {
                auto it = m_activeSlots.end();
                if (cmd.targetSlot != nullptr) {
                    it = std::find_if(m_activeSlots.begin(), m_activeSlots.end(),
                        [&](const auto& s) { return s.get() == cmd.targetSlot; });
                } else if (cmd.index < m_activeSlots.size()) {
                    it = m_activeSlots.begin() + static_cast<ptrdiff_t>(cmd.index);
                }

                if (it != m_activeSlots.end()) {
                    auto removed = std::move(*it);
                    m_activeSlots.erase(it);
                    if (!m_reclaimQueue.try_enqueue(std::move(removed))) {
                        stashForReclamation(std::move(removed));
                    }
                }
                break;
            }
            case SlotCommandType::Clear: {
                for (auto& s : m_activeSlots) {
                    if (s) {
                        if (!m_reclaimQueue.try_enqueue(std::move(s))) {
                            stashForReclamation(std::move(s));
                        }
                    }
                }
                m_activeSlots.clear();
                break;
            }
        }
    }
}

void ParallelBranch::process(AudioProcessContext& ctx) {
    const uint32_t numSamples = ctx.numSamples;
    if (numSamples == 0 || m_branchSuspendDepth.load(std::memory_order_acquire) != 0) {
        return;
    }

    m_branchActiveCallbacks.fetch_add(1, std::memory_order_seq_cst);
    struct BranchActiveGuard {
        std::atomic<uint32_t>& counter;
        ~BranchActiveGuard() noexcept {
            counter.fetch_sub(1, std::memory_order_release);
        }
    } branchGuard{m_branchActiveCallbacks};

    if (m_branchSuspendDepth.load(std::memory_order_seq_cst) != 0) {
        return;
    }

    m_isAudioRunning.store(true, std::memory_order_release);

    // 1. Drain pending commands from UI thread (Wait-Free, 0 locks)
    drainSlotCommandsIntoActive();

    // 2. Audio Processing through active slots with zero-copy ping-pong buffering
    AudioBufferView curIn = m_branchBuffer.view(numSamples);
    curIn.copyFrom(ctx.input);
    AudioBufferView curOut = m_slotTemp.view(numSamples);

    for (auto& slot : m_activeSlots) {
        if (!slot) continue;
        AudioProcessContext slotCtx = ctx;
        slotCtx.input = curIn;
        slotCtx.output = curOut;
        slotCtx.sampleRate = ctx.sampleRate;
        slotCtx.numSamples = numSamples;
        slot->process(slotCtx);
        std::swap(curIn, curOut);
    }

    auto branchView = m_branchBuffer.view(numSamples);
    if (curIn.channel(0) != branchView.channel(0)) {
        branchView.copyFrom(curIn);
    }

    // 4. Dynamic phase-coherent Parallel Delay Compensation (PDC)
    float* bL = branchView.channel(0);
    float* bR = branchView.numChannels() > 1 ? branchView.channel(1) : bL;

    for (uint32_t s = 0; s < numSamples; ++s) {
        const float inSampL = bL[s];
        const float inSampR = bR[s];

        float outSampL = inSampL;
        float outSampR = inSampR;

        if (m_isCrossfading) {
            const float t = static_cast<float>(m_crossfadePos) / 64.0f;
            const float angle = t * DspUtils::HALF_PI;
            const float gOld = std::cos(angle);
            const float gNew = std::sin(angle);

            float oldL = inSampL, oldR = inSampR;
            if (m_oldDelay > 0) {
                const size_t rOld = (m_delayWritePos + PDC_BUFFER_SIZE - m_oldDelay) & PDC_BUFFER_MASK;
                oldL = m_delayBufferL[rOld];
                oldR = m_delayBufferR[rOld];
            }

            float newL = inSampL, newR = inSampR;
            if (m_targetDelay > 0) {
                const size_t rNew = (m_delayWritePos + PDC_BUFFER_SIZE - m_targetDelay) & PDC_BUFFER_MASK;
                newL = m_delayBufferL[rNew];
                newR = m_delayBufferR[rNew];
            }

            outSampL = (oldL * gOld) + (newL * gNew);
            outSampR = (oldR * gOld) + (newR * gNew);

            m_crossfadePos++;
            if (m_crossfadePos >= 64) {
                m_isCrossfading = false;
                m_currentDelay = m_targetDelay;
            }
        } else if (m_currentDelay > 0) {
            const size_t rPos = (m_delayWritePos + PDC_BUFFER_SIZE - m_currentDelay) & PDC_BUFFER_MASK;
            outSampL = m_delayBufferL[rPos];
            outSampR = m_delayBufferR[rPos];
        }

        // Store sample into circular buffer (read-before-write)
        m_delayBufferL[m_delayWritePos] = inSampL;
        m_delayBufferR[m_delayWritePos] = inSampR;
        m_delayWritePos = (m_delayWritePos + 1) & PDC_BUFFER_MASK;

        bL[s] = outSampL;
        bR[s] = outSampR;
    }

    m_meter.process(branchView.channel(0), branchView.numChannels() > 1 ? branchView.channel(1) : nullptr, numSamples, ctx.sampleRate);
}

// ==========================================
// ParallelSplitMergeBlock Implementation
// ==========================================

ParallelSplitMergeBlock::ParallelSplitMergeBlock(std::string name)
    : m_name(std::move(name)) {
    m_uiBranches.reserve(MAX_BRANCHES);
    m_activeBranches.reserve(MAX_BRANCHES);
    m_deferredReclaimBranches.reserve(DEFERRED_BRANCH_RECLAIM_CAPACITY);
}

ParallelSplitMergeBlock::~ParallelSplitMergeBlock() {
    m_blockSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
    while (m_blockActiveCallbacks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    m_isAudioRunning.store(false, std::memory_order_release);
    BranchCommand cmd;
    while (m_branchCmdQueue.try_dequeue(cmd)) {
        cmd.branch.reset();
    }
    collectReclaimedBranches();
    m_activeBranches.clear();
    {
        std::lock_guard<std::mutex> lock(m_branchMutex);
        m_uiBranches.clear();
        syncRtBranchesLocked();
    }
    for (auto& b : m_deferredReclaimBranches) {
        b.reset();
    }
    m_deferredReclaimBranches.clear();
    m_blockSuspendDepth.fetch_sub(1, std::memory_order_release);
}

void ParallelSplitMergeBlock::syncRtBranchesLocked() noexcept {
    const size_t count = std::min(m_uiBranches.size(), MAX_BRANCHES);
    for (size_t i = 0; i < count; ++i) {
        m_rtBranches[i].store(m_uiBranches[i], std::memory_order_release);
    }
    for (size_t i = count; i < MAX_BRANCHES; ++i) {
        m_rtBranches[i].store(nullptr, std::memory_order_release);
    }
    m_rtNumBranches.store(count, std::memory_order_release);
}

void ParallelSplitMergeBlock::setAudioRunning(bool running) noexcept {
    m_isAudioRunning.store(running, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_branchMutex);
    for (auto* branch : m_uiBranches) {
        if (branch) branch->setAudioRunning(running);
    }
}

void ParallelSplitMergeBlock::prepare(double sampleRate, uint32_t maxBlockSize) {
    m_blockSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
    while (m_blockActiveCallbacks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    m_isAudioRunning.store(false, std::memory_order_release);
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_mixBuffer.resize(2, maxBlockSize);
    m_deferredReclaimBranches.reserve(DEFERRED_BRANCH_RECLAIM_CAPACITY);
    drainCommandQueueIntoActive();
    for (auto& branch : m_activeBranches) {
        if (branch) branch->prepare(sampleRate, maxBlockSize);
    }
    m_blockSuspendDepth.fetch_sub(1, std::memory_order_release);
}

void ParallelSplitMergeBlock::reset() {
    m_blockSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
    while (m_blockActiveCallbacks.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    m_isAudioRunning.store(false, std::memory_order_release);
    drainCommandQueueIntoActive();
    for (auto& branch : m_activeBranches) {
        if (branch) branch->reset();
    }
    m_blockLatency.store(0, std::memory_order_relaxed);
    m_blockSuspendDepth.fetch_sub(1, std::memory_order_release);
}

uint32_t ParallelSplitMergeBlock::latencySamples() const noexcept {
    if (m_isAudioRunning.load(std::memory_order_acquire)) {
        return m_blockLatency.load(std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(m_branchMutex);
    uint32_t maxLat = 0;
    for (const auto* branch : m_uiBranches) {
        if (branch && !branch->isMuted()) {
            maxLat = std::max(maxLat, branch->totalLatencySamples());
        }
    }
    return maxLat;
}

ParallelBranch* ParallelSplitMergeBlock::addBranch(const std::string& branchName) {
    collectReclaimedBranches();
    auto branch = std::make_unique<ParallelBranch>(branchName);
    branch->prepare(m_sampleRate, m_maxBlockSize);
    ParallelBranch* rawPtr = branch.get();

    {
        std::lock_guard<std::mutex> lock(m_branchMutex);
        m_uiBranches.push_back(rawPtr);
        syncRtBranchesLocked();
    }

    BranchCommand cmd = BranchCommand::makeAdd(std::move(branch));
    if (!m_branchCmdQueue.try_enqueue(std::move(cmd))) {
        std::lock_guard<std::mutex> lock(m_branchMutex);
        m_uiBranches.pop_back();
        syncRtBranchesLocked();
        return nullptr;
    }

    if (!m_isAudioRunning.load(std::memory_order_acquire)) {
        m_blockSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
        while (m_blockActiveCallbacks.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        drainCommandQueueIntoActive();
        m_blockSuspendDepth.fetch_sub(1, std::memory_order_release);
    }
    return rawPtr;
}

void ParallelSplitMergeBlock::removeBranch(size_t index) {
    collectReclaimedBranches();
    ParallelBranch* target = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_branchMutex);
        if (index >= m_uiBranches.size()) return;
        target = m_uiBranches[index];
        m_uiBranches.erase(m_uiBranches.begin() + index);
        syncRtBranchesLocked();
    }

    BranchCommand cmd = BranchCommand::makeRemove(index, target);
    m_branchCmdQueue.try_enqueue(std::move(cmd));

    if (!m_isAudioRunning.load(std::memory_order_acquire)) {
        m_blockSuspendDepth.fetch_add(1, std::memory_order_seq_cst);
        while (m_blockActiveCallbacks.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        drainCommandQueueIntoActive();
        m_blockSuspendDepth.fetch_sub(1, std::memory_order_release);
    }
}

size_t ParallelSplitMergeBlock::numBranches() const noexcept {
    std::lock_guard<std::mutex> lock(m_branchMutex);
    return m_uiBranches.size();
}

ParallelBranch* ParallelSplitMergeBlock::getBranch(size_t index) noexcept {
    std::lock_guard<std::mutex> lock(m_branchMutex);
    if (index < m_uiBranches.size()) return m_uiBranches[index];
    return nullptr;
}

void ParallelSplitMergeBlock::collectReclaimedBranches() noexcept {
    std::unique_ptr<ParallelBranch> reclaimed;
    while (m_branchReclaimQueue.try_dequeue(reclaimed)) {
        reclaimed.reset();
    }
}

void ParallelSplitMergeBlock::collectReclaimedSlots() noexcept {
    collectReclaimedBranches();
    std::lock_guard<std::mutex> lock(m_branchMutex);
    for (auto* branch : m_uiBranches) {
        if (branch) branch->collectReclaimedSlots();
    }
}

void ParallelSplitMergeBlock::stashBranchForReclamation(std::unique_ptr<ParallelBranch> branch) noexcept {
    if (!branch) return;
    if (m_branchReclaimQueue.try_enqueue(std::move(branch))) {
        return;
    }
    if (m_deferredReclaimBranches.size() < m_deferredReclaimBranches.capacity()) {
        m_deferredReclaimBranches.push_back(std::move(branch));
    }
}

void ParallelSplitMergeBlock::drainCommandQueueIntoActive() {
    BranchCommand cmd;
    while (m_branchCmdQueue.try_dequeue(cmd)) {
        switch (cmd.type) {
            case BranchCommandType::Add: {
                if (cmd.branch) {
                    m_activeBranches.push_back(std::move(cmd.branch));
                }
                break;
            }
            case BranchCommandType::Remove: {
                auto it = m_activeBranches.end();
                if (cmd.targetBranch != nullptr) {
                    it = std::find_if(m_activeBranches.begin(), m_activeBranches.end(),
                        [&](const auto& b) { return b.get() == cmd.targetBranch; });
                } else if (cmd.index < m_activeBranches.size()) {
                    it = m_activeBranches.begin() + cmd.index;
                }
                if (it != m_activeBranches.end()) {
                    auto removed = std::move(*it);
                    m_activeBranches.erase(it);
                    if (!m_branchReclaimQueue.try_enqueue(std::move(removed))) {
                        stashBranchForReclamation(std::move(removed));
                    }
                }
                break;
            }
            case BranchCommandType::Clear: {
                for (auto& b : m_activeBranches) {
                    if (b) {
                        if (!m_branchReclaimQueue.try_enqueue(std::move(b))) {
                            stashBranchForReclamation(std::move(b));
                        }
                    }
                }
                m_activeBranches.clear();
                break;
            }
        }
    }
}

void ParallelSplitMergeBlock::process(AudioProcessContext& ctx) {
    const uint32_t numSamples = ctx.numSamples;
    if (numSamples == 0 || m_blockSuspendDepth.load(std::memory_order_acquire) != 0) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    m_blockActiveCallbacks.fetch_add(1, std::memory_order_seq_cst);
    struct BlockActiveGuard {
        std::atomic<uint32_t>& counter;
        ~BlockActiveGuard() noexcept {
            counter.fetch_sub(1, std::memory_order_release);
        }
    } blockGuard{m_blockActiveCallbacks};

    if (m_blockSuspendDepth.load(std::memory_order_seq_cst) != 0) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    m_isAudioRunning.store(true, std::memory_order_release);
    drainCommandQueueIntoActive();

    // Flush any deferred reclaimed branches
    if (!m_deferredReclaimBranches.empty()) {
        size_t writeIdx = 0;
        for (size_t i = 0; i < m_deferredReclaimBranches.size(); ++i) {
            if (!m_branchReclaimQueue.try_enqueue(std::move(m_deferredReclaimBranches[i]))) {
                if (writeIdx != i) {
                    m_deferredReclaimBranches[writeIdx] = std::move(m_deferredReclaimBranches[i]);
                }
                ++writeIdx;
            }
        }
        m_deferredReclaimBranches.resize(writeIdx);
    }

    if (m_activeBranches.empty()) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    // 1. Calculate dynamic PDC branch latencies
    uint32_t maxBranchLatency = 0;
    for (const auto& branch : m_activeBranches) {
        if (branch) {
            const uint32_t lat = branch->totalLatencySamples();
            branch->setBranchLatency(lat);
            if (!branch->isMuted()) {
                maxBranchLatency = std::max(maxBranchLatency, lat);
            }
        }
    }
    m_blockLatency.store(maxBranchLatency, std::memory_order_relaxed);

    // 2. Set target delay for each branch: Delta L = max(L) - L_i (up to 16,384 samples)
    for (auto& branch : m_activeBranches) {
        if (branch) {
            const uint32_t lat = branch->branchLatency();
            const uint32_t deltaL = (maxBranchLatency >= lat) ? (maxBranchLatency - lat) : 0;
            branch->setTargetDelay(deltaL);
        }
    }

    auto mixView = m_mixBuffer.view(numSamples);
    mixView.clear();

    // Check if any branch is soloed
    bool hasSolo = false;
    for (const auto& branch : m_activeBranches) {
        if (branch && branch->isSolo()) {
            hasSolo = true;
            break;
        }
    }

    const float blendVal = m_blend.load(std::memory_order_relaxed);

    // Process and sum active branches
    for (size_t bIdx = 0; bIdx < m_activeBranches.size(); ++bIdx) {
        auto& branch = m_activeBranches[bIdx];
        if (!branch) continue;
        if (branch->isMuted()) continue;
        if (hasSolo && !branch->isSolo()) continue;

        branch->process(ctx);
        auto branchOut = branch->buffer().view(numSamples);

        float blendWeight = 1.0f;
        if (m_activeBranches.size() == 2) {
            if (bIdx == 0) {
                blendWeight = (blendVal <= 0.0f) ? 1.0f : (1.0f - blendVal);
            } else if (bIdx == 1) {
                blendWeight = (blendVal >= 0.0f) ? 1.0f : (1.0f + blendVal);
            }
        }

        const float gain = DspUtils::dbToGain(branch->gainDb()) * blendWeight * (branch->isPhaseInvert() ? -1.0f : 1.0f);
        float panL = 1.0f, panR = 1.0f;
        DspUtils::calculateStereoPan(branch->pan(), panL, panR);

        float* outL = mixView.channel(0);
        float* outR = mixView.numChannels() > 1 ? mixView.channel(1) : outL;
        const float* srcL = branchOut.channel(0);
        const float* srcR = branchOut.numChannels() > 1 ? branchOut.channel(1) : srcL;

        const float gL = gain * panL;
        const float gR = gain * panR;

        for (uint32_t s = 0; s < numSamples; ++s) {
            outL[s] += srcL[s] * gL;
            outR[s] += srcR[s] * gR;
        }
    }

    ctx.output.copyFrom(mixView);
}

// ==========================================
// NoiseGate Implementation
// ==========================================

void NoiseGate::prepare(double sampleRate) {
    // Attack ~2ms, Release ~50ms
    m_attackCoeff = std::exp(-1.0f / (static_cast<float>(sampleRate) * 0.002f));
    m_releaseCoeff = std::exp(-1.0f / (static_cast<float>(sampleRate) * 0.050f));
    m_envelope = 0.0f;
    m_currentGain = 1.0f;
}

void NoiseGate::process(AudioBufferView& buffer) noexcept {
    if (!m_enabled.load(std::memory_order_relaxed)) return;

    const float threshDb = m_thresholdDb.load(std::memory_order_relaxed);
    const float threshold = DspUtils::dbToGain(threshDb);
    const uint32_t numSamples = buffer.numSamples();
    const uint32_t numCh = buffer.numChannels();

    float* ch0 = buffer.channel(0);
    float* ch1 = numCh > 1 ? buffer.channel(1) : nullptr;

    for (uint32_t s = 0; s < numSamples; ++s) {
        float peak = std::abs(ch0[s]);
        if (ch1) peak = std::max(peak, std::abs(ch1[s]));

        // Envelope follower
        if (peak > m_envelope) {
            m_envelope = peak + m_attackCoeff * (m_envelope - peak);
        } else {
            m_envelope = peak + m_releaseCoeff * (m_envelope - peak);
        }

        // Downward expander / gate
        float targetGain = (m_envelope >= threshold) ? 1.0f : 0.0f;
        const float smoothAlpha = std::clamp(1.0f - m_attackCoeff, 0.001f, 0.1f);
        m_currentGain += smoothAlpha * (targetGain - m_currentGain);

        ch0[s] *= m_currentGain;
        if (ch1) ch1[s] *= m_currentGain;
    }
}

// ==========================================
// GraphEngine Implementation
// ==========================================

GraphEngine::GraphEngine() {
    m_uiNodes.reserve(MAX_GRAPH_NODES);
    m_activeNodes.reserve(MAX_GRAPH_NODES);
    m_fadingOutNodes.reserve(MAX_GRAPH_NODES);
    m_deferredReclaim.reserve(DEFERRED_RECLAIM_CAPACITY);
}

GraphEngine::~GraphEngine() {
    ScopedAudioFence fence(this);
    m_isAudioRunning.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_uiMutex);
        m_uiNodes.clear();
        syncRtNodesLocked();
    }

    // 1. Drain pending commands
    GraphCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        cmd.node.reset();
        cmd.chainPayload.reset();
    }

    // 2. Drain stashed chains
    for (size_t i = 0; i < m_stashedChainCount; ++i) {
        m_stashedChains[i].reset();
    }
    m_stashedChainCount = 0;

    // 3. Drain reclamation queues
    std::unique_ptr<AudioNode> reclaimed;
    while (m_reclaimQueue.try_dequeue(reclaimed)) {
        reclaimed.reset();
    }
    std::unique_ptr<NodeChain> reclaimedChain;
    while (m_chainReclaimQueue.try_dequeue(reclaimedChain)) {
        reclaimedChain.reset();
    }

    // 4. Clear active, fading out, and deferred reclaim nodes
    m_activeNodes.clear();
    m_fadingOutNodes.clear();
    for (auto& n : m_deferredReclaim) {
        n.reset();
    }
    m_deferredReclaim.clear();
}

void GraphEngine::syncRtNodesLocked() noexcept {
    const size_t count = std::min(m_uiNodes.size(), MAX_GRAPH_NODES);
    for (size_t i = 0; i < count; ++i) {
        m_rtNodes[i].store(m_uiNodes[i], std::memory_order_release);
    }
    for (size_t i = count; i < MAX_GRAPH_NODES; ++i) {
        m_rtNodes[i].store(nullptr, std::memory_order_release);
    }
    m_rtNumNodes.store(count, std::memory_order_release);
}

void GraphEngine::setAudioRunning(bool running) noexcept {
    m_isAudioRunning.store(running, std::memory_order_release);
    std::lock_guard<std::mutex> lock(m_uiMutex);
    for (auto* node : m_uiNodes) {
        if (auto* split = dynamic_cast<ParallelSplitMergeBlock*>(node)) {
            split->setAudioRunning(running);
        }
    }
}

void GraphEngine::stashForReclamation(std::unique_ptr<AudioNode> node) noexcept {
    if (!node) return;
    if (m_reclaimQueue.try_enqueue(std::move(node))) {
        return;
    }
    if (m_deferredReclaim.size() < m_deferredReclaim.capacity()) {
        m_deferredReclaim.push_back(std::move(node));
    }
}

void GraphEngine::stashChainForReclamation(std::unique_ptr<NodeChain> chain) noexcept {
    if (!chain) return;
    if (m_chainReclaimQueue.try_enqueue(std::move(chain))) {
        return;
    }
    if (m_stashedChainCount < m_stashedChains.size()) {
        m_stashedChains[m_stashedChainCount++] = std::move(chain);
        return;
    }
    // Fail-safe: transfer any remaining nodes in chain into m_deferredReclaim so ~AudioNode never runs on audio thread
    for (auto& n : chain->nodes) {
        if (n) {
            stashForReclamation(std::move(n));
        }
    }
}

void GraphEngine::flushStashedReclamations() noexcept {
    if (!m_deferredReclaim.empty()) {
        size_t writeIdx = 0;
        for (size_t i = 0; i < m_deferredReclaim.size(); ++i) {
            if (!m_reclaimQueue.try_enqueue(std::move(m_deferredReclaim[i]))) {
                if (writeIdx != i) {
                    m_deferredReclaim[writeIdx] = std::move(m_deferredReclaim[i]);
                }
                ++writeIdx;
            }
        }
        m_deferredReclaim.resize(writeIdx);
    }
    while (m_stashedChainCount > 0) {
        if (m_chainReclaimQueue.try_enqueue(std::move(m_stashedChains[m_stashedChainCount - 1]))) {
            --m_stashedChainCount;
        } else {
            break;
        }
    }
}

void GraphEngine::drainCommandQueueIntoActive() {
    GraphCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        switch (cmd.type) {
            case GraphCommandType::Add: {
                if (cmd.node) {
                    if (m_activeNodes.size() < MAX_GRAPH_NODES) {
                        m_activeNodes.push_back(std::move(cmd.node));
                    } else {
                        stashForReclamation(std::move(cmd.node));
                    }
                }
                break;
            }
            case GraphCommandType::Insert: {
                if (cmd.node) {
                    if (m_activeNodes.size() < MAX_GRAPH_NODES) {
                        if (cmd.indexA <= m_activeNodes.size()) {
                            m_activeNodes.insert(m_activeNodes.begin() + cmd.indexA, std::move(cmd.node));
                        } else {
                            m_activeNodes.push_back(std::move(cmd.node));
                        }
                    } else {
                        stashForReclamation(std::move(cmd.node));
                    }
                }
                break;
            }
            case GraphCommandType::Remove: {
                auto it = m_activeNodes.end();
                if (cmd.targetNode != nullptr) {
                    it = std::find_if(m_activeNodes.begin(), m_activeNodes.end(),
                        [&](const auto& n) { return n.get() == cmd.targetNode; });
                } else if (cmd.indexA < m_activeNodes.size()) {
                    it = m_activeNodes.begin() + cmd.indexA;
                }
                if (it != m_activeNodes.end()) {
                    m_activeNodes.erase(it);
                }
                break;
            }
            case GraphCommandType::Swap: {
                if (cmd.indexA < m_activeNodes.size() && cmd.indexB < m_activeNodes.size()) {
                    std::swap(m_activeNodes[cmd.indexA], m_activeNodes[cmd.indexB]);
                }
                break;
            }
            case GraphCommandType::Reorder: {
                if (cmd.indexA < m_activeNodes.size() && cmd.indexB < m_activeNodes.size() && cmd.indexA != cmd.indexB) {
                    auto node = std::move(m_activeNodes[cmd.indexA]);
                    m_activeNodes.erase(m_activeNodes.begin() + cmd.indexA);
                    m_activeNodes.insert(m_activeNodes.begin() + cmd.indexB, std::move(node));
                }
                break;
            }
            case GraphCommandType::Clear: {
                m_activeNodes.clear();
                break;
            }
            case GraphCommandType::CrossfadeTo: {
                if (cmd.chainPayload) {
                    m_activeNodes.clear();
                    for (auto& n : cmd.chainPayload->nodes) {
                        if (n) {
                            if (m_activeNodes.size() < MAX_GRAPH_NODES) {
                                m_activeNodes.push_back(std::move(n));
                            } else {
                                stashForReclamation(std::move(n));
                            }
                        }
                    }
                }
                break;
            }
            case GraphCommandType::SplitToParallel: {
                if (cmd.indexA < m_activeNodes.size() && cmd.node) {
                    auto originalNode = std::move(m_activeNodes[cmd.indexA]);
                    auto* parallelBlock = dynamic_cast<ParallelSplitMergeBlock*>(cmd.node.get());
                    if (parallelBlock) {
                        parallelBlock->flushBranchCommandsIntoActive();
                    }
                    if (parallelBlock && !parallelBlock->activeBranches().empty() && parallelBlock->activeBranches()[0]) {
                        auto* branchA = parallelBlock->activeBranches()[0].get();
                        branchA->drainSlotCommandsIntoActive();
                        auto* slotA = dynamic_cast<PluginSlot*>(originalNode.get());
                        if (slotA && branchA && branchA->activeSlots().size() < ParallelBranch::MAX_BRANCH_SLOTS) {
                            originalNode.release();
                            branchA->activeSlots().push_back(std::unique_ptr<PluginSlot>(slotA));
                        } else {
                            stashForReclamation(std::move(originalNode));
                        }
                    } else {
                        stashForReclamation(std::move(originalNode));
                    }
                    m_activeNodes[cmd.indexA] = std::move(cmd.node);
                }
                break;
            }
            case GraphCommandType::DissolveParallel: {
                if (cmd.indexA < m_activeNodes.size()) {
                    auto blockNode = std::move(m_activeNodes[cmd.indexA]);
                    m_activeNodes.erase(m_activeNodes.begin() + static_cast<ptrdiff_t>(cmd.indexA));
                    auto* block = dynamic_cast<ParallelSplitMergeBlock*>(blockNode.get());
                    if (block) {
                        block->flushBranchCommandsIntoActive();
                    }
                    if (block && cmd.indexB < block->activeBranches().size() && block->activeBranches()[cmd.indexB]) {
                        auto* br = block->activeBranches()[cmd.indexB].get();
                        if (br) {
                            br->drainSlotCommandsIntoActive();
                            auto& actSlots = br->activeSlots();
                            size_t insertPos = cmd.indexA;
                            for (auto& s : actSlots) {
                                if (!s) continue;
                                if (m_activeNodes.size() < MAX_GRAPH_NODES) {
                                    m_activeNodes.insert(m_activeNodes.begin() + static_cast<ptrdiff_t>(insertPos), std::move(s));
                                    ++insertPos;
                                } else {
                                    if (!m_reclaimQueue.try_enqueue(std::move(s))) {
                                        stashForReclamation(std::move(s));
                                    }
                                }
                            }
                            actSlots.clear();
                        }
                    }
                    stashForReclamation(std::move(blockNode));
                }
                break;
            }
        }
    }
}

void GraphEngine::prepare(double sampleRate, uint32_t maxBlockSize) {
    ScopedAudioFence fence(this);
    m_isAudioRunning.store(false, std::memory_order_release);
    m_cancelSceneCrossfade.store(false, std::memory_order_relaxed);
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;

    m_noiseGate.prepare(sampleRate);
    m_mainProcessingBuffer.resize(2, maxBlockSize);
    m_scratchBuffer.resize(2, maxBlockSize);
    m_activeBuffer.resize(2, maxBlockSize);
    m_activeNodes.reserve(MAX_GRAPH_NODES);
    m_fadingOutNodes.reserve(MAX_GRAPH_NODES);
    m_deferredReclaim.reserve(DEFERRED_RECLAIM_CAPACITY);
    const uint32_t rampSamples = static_cast<uint32_t>(sampleRate * 0.010);
    m_sceneCrossfadeRamp.reset(std::max(1u, rampSamples));
    m_microCrossfadeRamp.reset(64);
    m_outgoingTailBuffer.resize(2, maxBlockSize);
    m_inplaceCrossfadeRamp.reset(std::max(1u, rampSamples));
    m_pendingCrossfadeSamples.store(0, std::memory_order_relaxed);
    m_hasProcessedAudio = false;
    m_lastBlockSampleL = 0.0f;
    m_lastBlockSampleR = 0.0f;
    m_isSceneCrossfading.store(false, std::memory_order_relaxed);

    drainCommandQueueIntoActive();
    for (auto& node : m_activeNodes) {
        if (node) node->prepare(sampleRate, maxBlockSize);
    }
}

void GraphEngine::reset() {
    ScopedAudioFence fence(this);
    m_isAudioRunning.store(false, std::memory_order_release);
    m_cancelSceneCrossfade.store(false, std::memory_order_relaxed);
    m_hasProcessedAudio = false;
    m_lastBlockSampleL = 0.0f;
    m_lastBlockSampleR = 0.0f;
    m_isSceneCrossfading.store(false, std::memory_order_relaxed);
    m_pendingCrossfadeSamples.store(0, std::memory_order_relaxed);
    m_inplaceCrossfadeRamp.reset(std::max(1u, static_cast<uint32_t>(m_sampleRate * 0.010)));
    m_inputMeter.reset();
    m_outputMeter.reset();
    m_activeNodes.reserve(MAX_GRAPH_NODES);
    m_fadingOutNodes.clear();
    m_fadingOutNodes.reserve(MAX_GRAPH_NODES);
    m_deferredReclaim.reserve(DEFERRED_RECLAIM_CAPACITY);

    drainCommandQueueIntoActive();
    for (auto& node : m_activeNodes) {
        if (node) node->reset();
    }
}

void GraphEngine::addSerialNode(std::unique_ptr<AudioNode> node) {
    if (!node) return;
    processReclamation();

    node->prepare(m_sampleRate, m_maxBlockSize);

    std::lock_guard<std::mutex> lock(m_uiMutex);
    AudioNode* rawPtr = node.get();
    m_uiNodes.push_back(rawPtr);
    syncRtNodesLocked();

    GraphCommand cmd = GraphCommand::makeAdd(std::move(node));
    if (!m_commandQueue.try_enqueue(std::move(cmd))) {
        m_uiNodes.pop_back();
        syncRtNodesLocked();
    }
}

void GraphEngine::insertSerialNode(size_t index, std::unique_ptr<AudioNode> node) {
    if (!node) return;
    processReclamation();

    node->prepare(m_sampleRate, m_maxBlockSize);

    std::lock_guard<std::mutex> lock(m_uiMutex);
    AudioNode* rawPtr = node.get();
    if (index >= m_uiNodes.size()) {
        m_uiNodes.push_back(rawPtr);
        syncRtNodesLocked();
        GraphCommand cmd = GraphCommand::makeAdd(std::move(node));
        if (!m_commandQueue.try_enqueue(std::move(cmd))) {
            m_uiNodes.pop_back();
            syncRtNodesLocked();
        }
    } else {
        m_uiNodes.insert(m_uiNodes.begin() + index, rawPtr);
        syncRtNodesLocked();
        GraphCommand cmd = GraphCommand::makeInsert(index, std::move(node));
        if (!m_commandQueue.try_enqueue(std::move(cmd))) {
            m_uiNodes.erase(m_uiNodes.begin() + index);
            syncRtNodesLocked();
        }
    }
}

void GraphEngine::removeSerialNode(size_t index) {
    processReclamation();
    std::lock_guard<std::mutex> lock(m_uiMutex);
    if (index >= m_uiNodes.size()) return;

    AudioNode* target = m_uiNodes[index];
    m_uiNodes.erase(m_uiNodes.begin() + index);
    syncRtNodesLocked();

    GraphCommand cmd = GraphCommand::makeRemove(index, target);
    m_commandQueue.try_enqueue(std::move(cmd));
}

std::unique_ptr<AudioNode> GraphEngine::takeSerialNode(size_t index) {
    if (m_isAudioRunning.load(std::memory_order_acquire)) {
        return nullptr;
    }
    ScopedAudioFence fence(this);
    processReclamation();
    std::lock_guard<std::mutex> lock(m_uiMutex);

    drainCommandQueueIntoActive();

    AudioNode* targetPtr = nullptr;
    if (index < m_uiNodes.size()) {
        targetPtr = m_uiNodes[index];
        m_uiNodes.erase(m_uiNodes.begin() + index);
        syncRtNodesLocked();
    }

    if (targetPtr) {
        auto it = std::find_if(m_activeNodes.begin(), m_activeNodes.end(),
            [&](const auto& n) { return n.get() == targetPtr; });
        if (it != m_activeNodes.end()) {
            auto ptr = std::move(*it);
            m_activeNodes.erase(it);
            return ptr;
        }
    }

    if (index < m_activeNodes.size()) {
        auto ptr = std::move(m_activeNodes[index]);
        m_activeNodes.erase(m_activeNodes.begin() + index);
        return ptr;
    }

    return nullptr;
}

void GraphEngine::swapNodes(size_t indexA, size_t indexB) {
    if (indexA == indexB) return;
    std::lock_guard<std::mutex> lock(m_uiMutex);
    if (indexA < m_uiNodes.size() && indexB < m_uiNodes.size()) {
        std::swap(m_uiNodes[indexA], m_uiNodes[indexB]);
        syncRtNodesLocked();
        GraphCommand cmd = GraphCommand::makeSwap(indexA, indexB);
        m_commandQueue.try_enqueue(std::move(cmd));
    }
}

void GraphEngine::reorderNodes(size_t fromIndex, size_t toIndex) {
    if (fromIndex == toIndex) return;
    std::lock_guard<std::mutex> lock(m_uiMutex);
    if (fromIndex < m_uiNodes.size() && toIndex < m_uiNodes.size()) {
        auto* ptr = m_uiNodes[fromIndex];
        m_uiNodes.erase(m_uiNodes.begin() + fromIndex);
        m_uiNodes.insert(m_uiNodes.begin() + toIndex, ptr);
        syncRtNodesLocked();
        GraphCommand cmd = GraphCommand::makeReorder(fromIndex, toIndex);
        m_commandQueue.try_enqueue(std::move(cmd));
    }
}

void GraphEngine::splitSerialNodeIntoParallel(size_t index) {
    processReclamation();
    std::lock_guard<std::mutex> lock(m_uiMutex);
    if (index >= m_uiNodes.size()) return;

    const bool audioStopped = !m_isAudioRunning.load(std::memory_order_acquire);
    if (audioStopped) {
        ScopedAudioFence fence(this);
        drainCommandQueueIntoActive();
    }

    auto parallelBlock = std::make_unique<ParallelSplitMergeBlock>("Parallel Split/Merge");
    parallelBlock->prepare(m_sampleRate, m_maxBlockSize);

    auto* branchA = parallelBlock->addBranch("Branch A");
    branchA->prepare(m_sampleRate, m_maxBlockSize);
    branchA->setPan(-0.5f);

    auto* branchB = parallelBlock->addBranch("Branch B");
    branchB->prepare(m_sampleRate, m_maxBlockSize);
    branchB->setPan(+0.5f);

    auto* origNode = m_uiNodes[index];
    auto* origSlot = dynamic_cast<PluginSlot*>(origNode);
    if (origSlot) {
        branchA->addSlotToUiShadow(origSlot);
    }
    m_uiNodes[index] = parallelBlock.get();
    syncRtNodesLocked();
    GraphCommand cmd = GraphCommand::makeSplitToParallel(index, std::move(parallelBlock));
    m_commandQueue.try_enqueue(std::move(cmd));

    if (!m_isAudioRunning.load(std::memory_order_acquire)) {
        ScopedAudioFence fence(this);
        drainCommandQueueIntoActive();
    }
}

void GraphEngine::dissolveParallelBlock(size_t blockIndex, int branchToKeep) {
    processReclamation();
    std::lock_guard<std::mutex> lock(m_uiMutex);
    if (blockIndex >= m_uiNodes.size()) return;

    if (!m_isAudioRunning.load(std::memory_order_acquire)) {
        ScopedAudioFence fence(this);
        drainCommandQueueIntoActive();
    }

    auto* block = dynamic_cast<ParallelSplitMergeBlock*>(m_uiNodes[blockIndex]);
    if (!block) return;
    std::vector<AudioNode*> preservedUiSlots;
    if (branchToKeep >= 0 && static_cast<size_t>(branchToKeep) < block->numBranches()) {
        auto* br = block->getBranch(branchToKeep);
        if (br) {
            for (size_t s = 0; s < br->numSlots(); ++s) {
                preservedUiSlots.push_back(br->getSlot(s));
            }
        }
    }
    m_uiNodes.erase(m_uiNodes.begin() + blockIndex);
    m_uiNodes.insert(m_uiNodes.begin() + blockIndex, preservedUiSlots.begin(), preservedUiSlots.end());
    syncRtNodesLocked();
    GraphCommand cmd = GraphCommand::makeDissolveParallel(blockIndex, branchToKeep);
    m_commandQueue.try_enqueue(std::move(cmd));

    if (!m_isAudioRunning.load(std::memory_order_acquire)) {
        ScopedAudioFence fence(this);
        drainCommandQueueIntoActive();
    }
}

void GraphEngine::clearNodes() {
    processReclamation();
    std::lock_guard<std::mutex> lock(m_uiMutex);
    m_uiNodes.clear();
    syncRtNodesLocked();
    m_cancelSceneCrossfade.store(true, std::memory_order_release);

    GraphCommand cmd = GraphCommand::makeClear();
    m_commandQueue.try_enqueue(std::move(cmd));

    if (!m_isAudioRunning.load(std::memory_order_acquire)) {
        ScopedAudioFence fence(this);
        m_sceneCrossfadeRamp.reset(1);
        m_isSceneCrossfading.store(false, std::memory_order_release);
        drainCommandQueueIntoActive();
    }
}

void GraphEngine::crossfadeToNodes(std::vector<std::unique_ptr<AudioNode>> newNodes) {
    processReclamation();

    auto chain = std::make_unique<NodeChain>();
    chain->nodes.reserve(newNodes.size());

    std::vector<AudioNode*> rawPointers;
    rawPointers.reserve(newNodes.size());
    for (auto& node : newNodes) {
        if (node) {
            node->prepare(m_sampleRate, m_maxBlockSize);
            rawPointers.push_back(node.get());
            chain->nodes.push_back(std::move(node));
        }
    }

    std::lock_guard<std::mutex> lock(m_uiMutex);
    m_uiNodes = std::move(rawPointers);
    syncRtNodesLocked();

    GraphCommand cmd = GraphCommand::makeCrossfadeTo(std::move(chain));
    m_commandQueue.try_enqueue(std::move(cmd));
}

void GraphEngine::processReclamation() noexcept {
    // 1. Drain retired nodes from GraphEngine's SPSC queue
    std::unique_ptr<AudioNode> reclaimed;
    while (m_reclaimQueue.try_dequeue(reclaimed)) {
        reclaimed.reset();
    }

    // 2. Drain retired node chains
    std::unique_ptr<NodeChain> reclaimedChain;
    while (m_chainReclaimQueue.try_dequeue(reclaimedChain)) {
        reclaimedChain.reset();
    }

    // 3. Collect reclaimed slots from all parallel branches
    std::lock_guard<std::mutex> lock(m_uiMutex);
    for (auto* node : m_uiNodes) {
        if (node) {
            if (auto* splitBlock = dynamic_cast<ParallelSplitMergeBlock*>(node)) {
                splitBlock->collectReclaimedSlots();
            }
        }
    }
}

size_t GraphEngine::numNodes() const noexcept {
    std::lock_guard<std::mutex> lock(m_uiMutex);
    return m_uiNodes.size();
}

AudioNode* GraphEngine::getNode(size_t index) noexcept {
    std::lock_guard<std::mutex> lock(m_uiMutex);
    if (index < m_uiNodes.size()) return m_uiNodes[index];
    return nullptr;
}

bool GraphEngine::dispatchParameterThreadSafe(int slotIdx, int branchIdx, int branchSlotIdx,
                                              uint32_t paramId, float normalizedValue) noexcept {
    const size_t nodeCount = m_rtNumNodes.load(std::memory_order_acquire);
    if (slotIdx < 0 || static_cast<size_t>(slotIdx) >= nodeCount || static_cast<size_t>(slotIdx) >= MAX_GRAPH_NODES) {
        return false;
    }

    AudioNode* node = m_rtNodes[static_cast<size_t>(slotIdx)].load(std::memory_order_acquire);
    if (!node) {
        return false;
    }

    PluginSlot* slot = nullptr;
    if (branchIdx >= 0) {
        if (node->type() != NodeType::ParallelSplitMerge) {
            return false;
        }
        auto* block = static_cast<ParallelSplitMergeBlock*>(node);
        ParallelBranch* branch = block->getBranchLockFree(static_cast<size_t>(branchIdx));
        if (!branch || branchSlotIdx < 0) {
            return false;
        }
        slot = branch->getSlotLockFree(static_cast<size_t>(branchSlotIdx));
    } else {
        if (node->type() != NodeType::Plugin) {
            return false;
        }
        slot = static_cast<PluginSlot*>(node);
    }

    if (!slot) {
        return false;
    }

    slot->setParameterNormalized(paramId, normalizedValue);
    return true;
}

bool GraphEngine::dispatchBindingThreadSafe(praccy::midi::BindingTargetType type,
                                            int slotIdx, int branchIdx, int branchSlotIdx,
                                            uint32_t paramId, float value) noexcept {
    const size_t nodeCount = m_rtNumNodes.load(std::memory_order_acquire);
    if (slotIdx < 0 || static_cast<size_t>(slotIdx) >= nodeCount || static_cast<size_t>(slotIdx) >= MAX_GRAPH_NODES) {
        return false;
    }

    AudioNode* node = m_rtNodes[static_cast<size_t>(slotIdx)].load(std::memory_order_acquire);
    if (!node) {
        return false;
    }

    // Branch-level controls (Mute, Solo, Gain, Pan)
    if (branchIdx >= 0 && branchSlotIdx < 0) {
        if (node->type() != NodeType::ParallelSplitMerge) {
            return false;
        }
        auto* block = static_cast<ParallelSplitMergeBlock*>(node);
        ParallelBranch* branch = block->getBranchLockFree(static_cast<size_t>(branchIdx));
        if (!branch) return false;

        switch (type) {
            case praccy::midi::BindingTargetType::BranchMute:
                branch->setMuted(value > 0.5f);
                return true;
            case praccy::midi::BindingTargetType::BranchSolo:
                branch->setSolo(value > 0.5f);
                return true;
            case praccy::midi::BindingTargetType::BranchGain:
                branch->setGainDb(-36.0f + (value * 42.0f));
                return true;
            case praccy::midi::BindingTargetType::BranchPan:
                branch->setPan((value * 2.0f) - 1.0f);
                return true;
            default:
                break;
        }
    }

    PluginSlot* slot = nullptr;
    if (branchIdx >= 0) {
        if (node->type() != NodeType::ParallelSplitMerge) {
            return false;
        }
        auto* block = static_cast<ParallelSplitMergeBlock*>(node);
        ParallelBranch* branch = block->getBranchLockFree(static_cast<size_t>(branchIdx));
        if (!branch || branchSlotIdx < 0) {
            return false;
        }
        slot = branch->getSlotLockFree(static_cast<size_t>(branchSlotIdx));
    } else {
        if (node->type() != NodeType::Plugin) {
            return false;
        }
        slot = static_cast<PluginSlot*>(node);
    }

    if (!slot) {
        return false;
    }

    switch (type) {
        case praccy::midi::BindingTargetType::SlotBypass:
            slot->setBypassed(value > 0.5f);
            return true;
        case praccy::midi::BindingTargetType::SlotDryWet:
            slot->setDryWet(value);
            return true;
        case praccy::midi::BindingTargetType::SlotInputGain:
            slot->setInputGainDb(-24.0f + (value * 36.0f));
            return true;
        case praccy::midi::BindingTargetType::SlotOutputGain:
            slot->setOutputGainDb(-24.0f + (value * 36.0f));
            return true;
        case praccy::midi::BindingTargetType::PluginParameter:
            slot->setParameterNormalized(paramId, value);
            return true;
        default:
            break;
    }
    return false;
}

void GraphEngine::process(const AudioBufferView& hardwareIn, AudioBufferView& hardwareOut) {
    AudioProcessContext defaultTransport{};
    defaultTransport.sampleRate = m_sampleRate;
    defaultTransport.numSamples = hardwareIn.numSamples();
    process(hardwareIn, hardwareOut, defaultTransport);
}

void GraphEngine::process(const AudioBufferView& hardwareIn, AudioBufferView& hardwareOut, const AudioProcessContext& transportCtx) {
    DspUtils::ScopedDenormalGuard denormalGuard;
    const uint32_t numSamples = hardwareIn.numSamples();
    if (numSamples == 0) return;

    // Two-way Dekker fence with executeWithAudioFence() / ScopedAudioFence
    if (m_suspendDepth.load(std::memory_order_acquire) != 0) {
        hardwareOut.copyFrom(hardwareIn);
        return;
    }

    m_activeCallbacks.fetch_add(1, std::memory_order_seq_cst);
    struct ActiveCallbackGuard {
        std::atomic<uint32_t>& counter;
        ~ActiveCallbackGuard() noexcept {
            counter.fetch_sub(1, std::memory_order_release);
        }
    } callbackGuard{m_activeCallbacks};

    if (m_suspendDepth.load(std::memory_order_seq_cst) != 0) {
        hardwareOut.copyFrom(hardwareIn);
        return;
    }

    m_isAudioRunning.store(true, std::memory_order_release);

    if (m_cancelSceneCrossfade.exchange(false, std::memory_order_acq_rel)) {
        m_sceneCrossfadeRamp.reset(1);
        m_isSceneCrossfading.store(false, std::memory_order_release);
        for (auto& node : m_fadingOutNodes) {
            if (node) {
                if (!m_reclaimQueue.try_enqueue(std::move(node))) {
                    stashForReclamation(std::move(node));
                }
            }
        }
        m_fadingOutNodes.clear();
    }

    // Check for triggered in-place 10ms crossfade (Wait-Free, Lock-Free)
    uint32_t pendingCrossfade = m_pendingCrossfadeSamples.exchange(0, std::memory_order_acq_rel);
    if (pendingCrossfade > 0) {
        m_inplaceCrossfadeRamp.reset(pendingCrossfade);
        m_inplaceCrossfadeRamp.startTransition(true);
    }

    // 1. Drain pending structural commands from UI thread (Wait-Free, Lock-Free)
    GraphCommand cmd;
    bool hadStructuralMutation = false;
    while (m_commandQueue.try_dequeue(cmd)) {
        switch (cmd.type) {
            case GraphCommandType::Add: {
                if (cmd.node) {
                    if (m_activeNodes.size() < MAX_GRAPH_NODES) {
                        m_activeNodes.push_back(std::move(cmd.node));
                    } else {
                        if (!m_reclaimQueue.try_enqueue(std::move(cmd.node))) {
                            stashForReclamation(std::move(cmd.node));
                        }
                    }
                }
                break;
            }
            case GraphCommandType::Insert: {
                if (cmd.node) {
                    if (m_activeNodes.size() < MAX_GRAPH_NODES) {
                        if (cmd.indexA <= m_activeNodes.size()) {
                            m_activeNodes.insert(m_activeNodes.begin() + cmd.indexA, std::move(cmd.node));
                        } else {
                            m_activeNodes.push_back(std::move(cmd.node));
                        }
                    } else {
                        if (!m_reclaimQueue.try_enqueue(std::move(cmd.node))) {
                            stashForReclamation(std::move(cmd.node));
                        }
                    }
                }
                break;
            }
            case GraphCommandType::Remove: {
                auto it = m_activeNodes.end();
                if (cmd.targetNode != nullptr) {
                    it = std::find_if(m_activeNodes.begin(), m_activeNodes.end(),
                        [&](const auto& n) { return n.get() == cmd.targetNode; });
                } else if (cmd.indexA < m_activeNodes.size()) {
                    it = m_activeNodes.begin() + cmd.indexA;
                }
                if (it != m_activeNodes.end()) {
                    auto removed = std::move(*it);
                    m_activeNodes.erase(it);
                    if (!m_reclaimQueue.try_enqueue(std::move(removed))) {
                        stashForReclamation(std::move(removed));
                    }
                    if (m_hasProcessedAudio) {
                        hadStructuralMutation = true;
                    }
                }
                break;
            }
            case GraphCommandType::Swap: {
                if (cmd.indexA < m_activeNodes.size() && cmd.indexB < m_activeNodes.size()) {
                    std::swap(m_activeNodes[cmd.indexA], m_activeNodes[cmd.indexB]);
                    if (m_hasProcessedAudio) {
                        hadStructuralMutation = true;
                    }
                }
                break;
            }
            case GraphCommandType::Reorder: {
                if (cmd.indexA < m_activeNodes.size() && cmd.indexB < m_activeNodes.size() && cmd.indexA != cmd.indexB) {
                    auto node = std::move(m_activeNodes[cmd.indexA]);
                    m_activeNodes.erase(m_activeNodes.begin() + cmd.indexA);
                    m_activeNodes.insert(m_activeNodes.begin() + cmd.indexB, std::move(node));
                    if (m_hasProcessedAudio) {
                        hadStructuralMutation = true;
                    }
                }
                break;
            }
            case GraphCommandType::Clear: {
                for (auto& n : m_activeNodes) {
                    if (n) {
                        if (!m_reclaimQueue.try_enqueue(std::move(n))) {
                            stashForReclamation(std::move(n));
                        }
                    }
                }
                m_activeNodes.clear();
                if (m_hasProcessedAudio) {
                    hadStructuralMutation = true;
                }
                break;
            }
            case GraphCommandType::CrossfadeTo: {
                for (auto& n : m_fadingOutNodes) {
                    if (n) {
                        if (!m_reclaimQueue.try_enqueue(std::move(n))) {
                            stashForReclamation(std::move(n));
                        }
                    }
                }
                m_fadingOutNodes.clear();

                for (auto& n : m_activeNodes) {
                    if (n) {
                        m_fadingOutNodes.push_back(std::move(n));
                    }
                }
                m_activeNodes.clear();

                if (cmd.chainPayload) {
                    for (auto& n : cmd.chainPayload->nodes) {
                        if (n) {
                            if (m_activeNodes.size() < MAX_GRAPH_NODES) {
                                m_activeNodes.push_back(std::move(n));
                            } else {
                                if (!m_reclaimQueue.try_enqueue(std::move(n))) {
                                    stashForReclamation(std::move(n));
                                }
                            }
                        }
                    }
                    cmd.chainPayload->nodes.clear();
                    if (!m_chainReclaimQueue.try_enqueue(std::move(cmd.chainPayload))) {
                        stashChainForReclamation(std::move(cmd.chainPayload));
                    }
                }

                const uint32_t rampSamples = static_cast<uint32_t>(m_sampleRate * 0.010);
                m_crossfadeRemainingSamples = std::max(1u, rampSamples);
                m_sceneCrossfadeRamp.reset(m_crossfadeRemainingSamples);
                m_sceneCrossfadeRamp.startTransition(true);
                m_isSceneCrossfading.store(true, std::memory_order_release);
                break;
            }
            case GraphCommandType::SplitToParallel: {
                if (cmd.indexA < m_activeNodes.size() && cmd.node) {
                    auto originalNode = std::move(m_activeNodes[cmd.indexA]);
                    auto* parallelBlock = dynamic_cast<ParallelSplitMergeBlock*>(cmd.node.get());
                    if (parallelBlock) {
                        parallelBlock->flushBranchCommandsIntoActive();
                    }
                    if (parallelBlock && !parallelBlock->activeBranches().empty() && parallelBlock->activeBranches()[0]) {
                        auto* branchA = parallelBlock->activeBranches()[0].get();
                        branchA->drainSlotCommandsIntoActive();
                        auto* slotA = dynamic_cast<PluginSlot*>(originalNode.get());
                        if (slotA && branchA && branchA->activeSlots().size() < ParallelBranch::MAX_BRANCH_SLOTS) {
                            originalNode.release();
                            branchA->activeSlots().push_back(std::unique_ptr<PluginSlot>(slotA));
                        } else {
                            if (!m_reclaimQueue.try_enqueue(std::move(originalNode))) {
                                stashForReclamation(std::move(originalNode));
                            }
                        }
                    } else {
                        if (!m_reclaimQueue.try_enqueue(std::move(originalNode))) {
                            stashForReclamation(std::move(originalNode));
                        }
                    }
                    m_activeNodes[cmd.indexA] = std::move(cmd.node);
                    if (m_hasProcessedAudio) hadStructuralMutation = true;
                }
                break;
            }
            case GraphCommandType::DissolveParallel: {
                if (cmd.indexA < m_activeNodes.size()) {
                    auto blockNode = std::move(m_activeNodes[cmd.indexA]);
                    m_activeNodes.erase(m_activeNodes.begin() + static_cast<ptrdiff_t>(cmd.indexA));
                    auto* block = dynamic_cast<ParallelSplitMergeBlock*>(blockNode.get());
                    if (block) {
                        block->flushBranchCommandsIntoActive();
                    }
                    if (block && cmd.indexB < block->activeBranches().size() && block->activeBranches()[cmd.indexB]) {
                        auto* br = block->activeBranches()[cmd.indexB].get();
                        if (br) {
                            br->drainSlotCommandsIntoActive();
                            auto& actSlots = br->activeSlots();
                            size_t insertPos = cmd.indexA;
                            for (auto& s : actSlots) {
                                if (!s) continue;
                                if (m_activeNodes.size() < MAX_GRAPH_NODES) {
                                    m_activeNodes.insert(m_activeNodes.begin() + static_cast<ptrdiff_t>(insertPos), std::move(s));
                                    ++insertPos;
                                } else {
                                    if (!m_reclaimQueue.try_enqueue(std::move(s))) {
                                        stashForReclamation(std::move(s));
                                    }
                                }
                            }
                            actSlots.clear();
                        }
                    }
                    if (!m_reclaimQueue.try_enqueue(std::move(blockNode))) {
                        stashForReclamation(std::move(blockNode));
                    }
                    if (m_hasProcessedAudio) hadStructuralMutation = true;
                }
                break;
            }
        }
    }

    // 2. Attempt to flush any previously stashed items
    flushStashedReclamations();

    // 3. Initiate 64-sample equal-power micro-crossfade on structural mutation
    if (hadStructuralMutation && !m_sceneCrossfadeRamp.isTransitioning()) {
        m_microCrossfadeRamp.reset(64);
        m_microCrossfadeRamp.startTransition(true);
    }

    auto mainView = m_mainProcessingBuffer.view(numSamples);
    const uint32_t inCh = hardwareIn.numChannels();
    float* mainL = mainView.channel(0);
    float* mainR = (mainView.numChannels() > 1) ? mainView.channel(1) : mainL;

    const InputRoutingConfig inCfg = inputRouting();
    if (inCfg.mode == InputRoutingMode::MonoLeft && inCh > 0) {
        const float* src = hardwareIn.channel(0);
        std::memcpy(mainL, src, numSamples * sizeof(float));
        std::memcpy(mainR, src, numSamples * sizeof(float));
    } else if (inCfg.mode == InputRoutingMode::MonoRight && inCh > 1) {
        const float* src = hardwareIn.channel(1);
        std::memcpy(mainL, src, numSamples * sizeof(float));
        std::memcpy(mainR, src, numSamples * sizeof(float));
    } else if (inCfg.mode == InputRoutingMode::MonoChannel && inCh > 0) {
        const uint32_t ch = std::min(static_cast<uint32_t>(inCfg.channelLeft), inCh - 1);
        const float* src = hardwareIn.channel(ch);
        std::memcpy(mainL, src, numSamples * sizeof(float));
        std::memcpy(mainR, src, numSamples * sizeof(float));
    } else if (inCfg.mode == InputRoutingMode::Stereo) {
        const float* srcL = inCh > 0 ? hardwareIn.channel(0) : nullptr;
        const float* srcR = inCh > 1 ? hardwareIn.channel(1) : srcL;
        if (srcL) std::memcpy(mainL, srcL, numSamples * sizeof(float));
        else std::memset(mainL, 0, numSamples * sizeof(float));
        if (srcR) std::memcpy(mainR, srcR, numSamples * sizeof(float));
        else std::memset(mainR, 0, numSamples * sizeof(float));
    } else if (inCfg.mode == InputRoutingMode::StereoCustom && inCh > 0) {
        const uint32_t chL = std::min(static_cast<uint32_t>(inCfg.channelLeft), inCh - 1);
        const uint32_t chR = std::min(static_cast<uint32_t>(inCfg.channelRight), inCh - 1);
        std::memcpy(mainL, hardwareIn.channel(chL), numSamples * sizeof(float));
        std::memcpy(mainR, hardwareIn.channel(chR), numSamples * sizeof(float));
    } else {
        mainView.copyFrom(hardwareIn);
    }

    // Track input levels
    m_inputMeter.process(mainL, mainR, numSamples, m_sampleRate);

    // Apply Master Input Gain
    const float inGain = DspUtils::dbToGain(m_inputGainDb.load(std::memory_order_relaxed));
    mainView.applyGain(inGain);

    // Apply Input Noise Gate
    m_noiseGate.process(mainView);

    // Execute serial rack chain (Completely Lock-Free, 0 Mutex Locks)
    if (m_sceneCrossfadeRamp.isTransitioning()) {
        // 1. Process Outgoing (Fading Out) Chain into m_outgoingTailBuffer with zero-copy ping-pong
        auto fadingView = m_outgoingTailBuffer.view(numSamples);
        fadingView.copyFrom(mainView);

        AudioBufferView curInFading = fadingView;
        AudioBufferView curOutFading = m_scratchBuffer.view(numSamples);
        for (auto& node : m_fadingOutNodes) {
            if (!node) continue;
            AudioProcessContext ctx = transportCtx;
            ctx.input = curInFading;
            ctx.output = curOutFading;
            ctx.sampleRate = m_sampleRate;
            ctx.numSamples = numSamples;
            node->process(ctx);
            std::swap(curInFading, curOutFading);
        }
        if (curInFading.channel(0) != fadingView.channel(0)) {
            fadingView.copyFrom(curInFading);
        }

        // 2. Process Incoming (Active) Chain into m_activeBuffer with zero-copy ping-pong
        auto actView = m_activeBuffer.view(numSamples);
        actView.copyFrom(mainView);

        AudioBufferView curInAct = actView;
        AudioBufferView curOutAct = m_scratchBuffer.view(numSamples);
        for (auto& node : m_activeNodes) {
            if (!node) continue;
            AudioProcessContext ctx = transportCtx;
            ctx.input = curInAct;
            ctx.output = curOutAct;
            ctx.sampleRate = m_sampleRate;
            ctx.numSamples = numSamples;
            node->process(ctx);
            std::swap(curInAct, curOutAct);
        }
        if (curInAct.channel(0) != actView.channel(0)) {
            actView.copyFrom(curInAct);
        }

        // 3. Sample-by-Sample EqualPower Crossfade
        const uint32_t numCh = mainView.numChannels();
        for (uint32_t s = 0; s < numSamples; ++s) {
            float gainOut = 0.0f, gainIn = 0.0f;
            m_sceneCrossfadeRamp.getNextGains(gainOut, gainIn);
            for (uint32_t ch = 0; ch < numCh; ++ch) {
                float* out = mainView.channel(ch);
                const float oldS = fadingView.channel(ch)[s];
                const float newS = actView.channel(ch)[s];
                out[s] = (oldS * gainOut) + (newS * gainIn);
            }
        }

        if (m_crossfadeRemainingSamples > numSamples) {
            m_crossfadeRemainingSamples -= numSamples;
        } else {
            m_crossfadeRemainingSamples = 0;
        }

        if (!m_sceneCrossfadeRamp.isTransitioning()) {
            m_isSceneCrossfading.store(false, std::memory_order_release);
            for (auto& node : m_fadingOutNodes) {
                if (node) {
                    if (!m_reclaimQueue.try_enqueue(std::move(node))) {
                        stashForReclamation(std::move(node));
                    }
                }
            }
            m_fadingOutNodes.clear();
        }
    } else {
        // Steady-state: Process active nodes with zero-copy ping-pong buffering
        AudioBufferView curIn = mainView;
        AudioBufferView curOut = m_scratchBuffer.view(numSamples);
        for (auto& node : m_activeNodes) {
            if (!node) continue;
            AudioProcessContext ctx = transportCtx;
            ctx.input = curIn;
            ctx.output = curOut;
            ctx.sampleRate = m_sampleRate;
            ctx.numSamples = numSamples;
            node->process(ctx);
            std::swap(curIn, curOut);
        }
        if (curIn.channel(0) != mainView.channel(0)) {
            mainView.copyFrom(curIn);
        }

        // 10ms smooth crossfade for in-place preset / snapshot switches without repeating frozen DC sample
        if (m_inplaceCrossfadeRamp.isTransitioning()) {
            const uint32_t numCh = mainView.numChannels();
            const uint32_t tailLimit = std::min(numSamples, m_outgoingTailBuffer.maxSamples());
            auto tailView = m_outgoingTailBuffer.view(tailLimit);
            for (uint32_t s = 0; s < numSamples; ++s) {
                float gainOld = 0.0f, gainNew = 0.0f;
                m_inplaceCrossfadeRamp.getNextGains(gainOld, gainNew);
                for (uint32_t ch = 0; ch < numCh; ++ch) {
                    float* out = mainView.channel(ch);
                    const float oldS = (s < tailLimit && ch < tailView.numChannels()) ? tailView.channel(ch)[s] : 0.0f;
                    out[s] = (oldS * gainOld) + (out[s] * gainNew);
                }
            }
        } else if (m_microCrossfadeRamp.isTransitioning()) {
            const uint32_t numCh = mainView.numChannels();
            for (uint32_t s = 0; s < numSamples; ++s) {
                float gainOld = 0.0f, gainNew = 0.0f;
                m_microCrossfadeRamp.getNextGains(gainOld, gainNew);
                for (uint32_t ch = 0; ch < numCh; ++ch) {
                    float* out = mainView.channel(ch);
                    const float prevSamp = (ch == 0) ? m_lastBlockSampleL : m_lastBlockSampleR;
                    out[s] = (prevSamp * gainOld) + (out[s] * gainNew);
                }
            }
        }
    }

    // Apply Master Volume
    const float masterVol = DspUtils::dbToGain(m_masterVolumeDb.load(std::memory_order_relaxed));
    mainView.applyGain(masterVol);

    // Master Safety Limiter: 0.95 soft-knee tanh saturator using DspUtils::fastTanh to prevent harsh clipping
    for (uint32_t ch = 0; ch < mainView.numChannels(); ++ch) {
        float* data = mainView.channel(ch);
        for (uint32_t s = 0; s < numSamples; ++s) {
            float x = data[s];
            if (!std::isfinite(x)) {
                x = 0.0f;
            } else if (x > 0.95f) {
                x = 0.95f + 0.05f * DspUtils::fastTanh((x - 0.95f) / 0.05f);
            } else if (x < -0.95f) {
                x = -0.95f + 0.05f * DspUtils::fastTanh((x + 0.95f) / 0.05f);
            }
            data[s] = x;
        }
    }

    hardwareOut.copyFrom(mainView);

    // Store outgoing audio tail for smooth in-place crossfading on tone / preset switches.
    // Do not overwrite the latched outgoing tail while an in-place crossfade is actively ramping.
    if (!m_inplaceCrossfadeRamp.isTransitioning() && numSamples > 0 && numSamples <= m_outgoingTailBuffer.maxSamples()) {
        auto tailView = m_outgoingTailBuffer.view(numSamples);
        tailView.copyFrom(mainView);
    }

    // Store last samples for micro-crossfading on structural mutations
    m_lastBlockSampleL = hardwareOut.channel(0)[numSamples - 1];
    m_lastBlockSampleR = (hardwareOut.numChannels() > 1) ? hardwareOut.channel(1)[numSamples - 1] : m_lastBlockSampleL;
    m_hasProcessedAudio = true;

    // Track output levels
    m_outputMeter.process(hardwareOut.channel(0), hardwareOut.numChannels() > 1 ? hardwareOut.channel(1) : nullptr, numSamples, m_sampleRate);
}

} // namespace praccy::audio
