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
    }
}

void PluginSlot::prepare(double sampleRate, uint32_t maxBlockSize) {
    if (m_innerNode) {
        m_innerNode->prepare(sampleRate, maxBlockSize);
    }
    m_dryBuffer.resize(2, maxBlockSize);
    m_processBuffer.resize(2, maxBlockSize);
    // 10ms smooth ramp at current sample rate
    m_bypassRamp.reset(static_cast<uint32_t>(sampleRate * 0.010));
}

void PluginSlot::reset() {
    if (m_innerNode) {
        m_innerNode->reset();
    }
    m_automationRegister.reset();
}

const std::string& PluginSlot::name() const noexcept {
    static const std::string emptyName = "Empty Slot";
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
    if (!m_innerNode || numSamples == 0) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    // 1. Drain pending parameter automation wait-free at block boundary
    m_automationRegister.drainDirty([&](uint32_t paramId, float value) {
        if (m_pluginPtr) {
            m_pluginPtr->setParameterFromAudioThread(paramId, value);
        }
    });

    const bool currentlyBypassed = isBypassed();
    const bool transitioning = m_bypassRamp.isTransitioning();

    // If fully bypassed, faulted, or not transitioning, pure pass-through
    if ((currentlyBypassed && !transitioning) || m_innerNode->isFaulted()) {
        ctx.output.copyFrom(ctx.input);
        m_meter.process(ctx.output.channel(0), ctx.output.numChannels() > 1 ? ctx.output.channel(1) : nullptr, numSamples);
        return;
    }

    // Save dry signal
    auto dryView = m_dryBuffer.view(numSamples);
    dryView.copyFrom(ctx.input);

    // Apply input gain
    auto procView = m_processBuffer.view(numSamples);
    procView.copyFrom(ctx.input);
    const float inGain = DspUtils::dbToGain(m_inputGainDb.load(std::memory_order_relaxed));
    procView.applyGain(inGain);

    // Process through inner plugin node
    AudioProcessContext innerCtx{
        .input = procView,
        .output = procView,
        .sampleRate = ctx.sampleRate,
        .numSamples = numSamples
    };
    m_innerNode->process(innerCtx);

    // If plugin faulted during execution, safely bypass with dry signal
    if (m_innerNode->isFaulted()) {
        ctx.output.copyFrom(dryView);
        m_meter.process(ctx.output.channel(0), ctx.output.numChannels() > 1 ? ctx.output.channel(1) : nullptr, numSamples);
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

    m_meter.process(ctx.output.channel(0), numCh > 1 ? ctx.output.channel(1) : nullptr, numSamples);
}

// ==========================================
// ParallelBranch Implementation
// ==========================================

ParallelBranch::ParallelBranch(std::string name)
    : m_name(std::move(name)) {
    m_uiSlots.reserve(MAX_BRANCH_SLOTS);
    m_activeSlots.reserve(MAX_BRANCH_SLOTS);
}

ParallelBranch::~ParallelBranch() {
    // 1. Drain any remaining items from command queue
    SlotCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        cmd.slot.reset();
    }

    // 2. Drain any stashed reclamations
    for (size_t i = 0; i < m_stashedCount; ++i) {
        m_stashedReclamations[i].reset();
    }
    m_stashedCount = 0;

    // 3. Drain reclamation queue
    collectReclaimedSlots();

    // 4. Clear active slots
    m_activeSlots.clear();
    m_uiSlots.clear();
}

void ParallelBranch::prepare(double sampleRate, uint32_t maxBlockSize) {
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_branchBuffer.resize(2, maxBlockSize);
    m_slotTemp.resize(2, maxBlockSize);
    m_activeSlots.reserve(MAX_BRANCH_SLOTS);

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
    SlotCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        if (cmd.type == SlotCommandType::Add && cmd.slot) {
            m_activeSlots.push_back(std::move(cmd.slot));
        } else if (cmd.type == SlotCommandType::Insert && cmd.slot) {
            size_t idx = std::min(cmd.index, m_activeSlots.size());
            m_activeSlots.insert(m_activeSlots.begin() + idx, std::move(cmd.slot));
        } else if (cmd.type == SlotCommandType::Reorder) {
            if (cmd.index < m_activeSlots.size() && cmd.targetIndex < m_activeSlots.size() && cmd.index != cmd.targetIndex) {
                auto s = std::move(m_activeSlots[cmd.index]);
                m_activeSlots.erase(m_activeSlots.begin() + cmd.index);
                m_activeSlots.insert(m_activeSlots.begin() + cmd.targetIndex, std::move(s));
            }
        }
    }

    for (auto& slot : m_activeSlots) {
        if (slot) slot->prepare(sampleRate, maxBlockSize);
    }
}

void ParallelBranch::reset() {
    std::fill(m_delayBufferL.begin(), m_delayBufferL.end(), 0.0f);
    std::fill(m_delayBufferR.begin(), m_delayBufferR.end(), 0.0f);
    m_delayWritePos = 0;
    m_currentDelay = 0;
    m_targetDelay = 0;
    m_oldDelay = 0;
    m_crossfadePos = 0;
    m_isCrossfading = false;
    m_branchLatency.store(0, std::memory_order_relaxed);

    for (auto& slot : m_activeSlots) {
        if (slot) slot->reset();
    }
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

    PluginSlot* rawPtr = slot.get();
    m_uiSlots.push_back(rawPtr);

    SlotCommand cmd = SlotCommand::makeAdd(std::move(slot));
    if (!m_commandQueue.try_enqueue(std::move(cmd))) {
        // Queue full fallback: roll back shadow registry to prevent dangling pointers
        m_uiSlots.pop_back();
    }
}

void ParallelBranch::insertSlot(size_t index, std::unique_ptr<PluginSlot> slot) {
    if (!slot) return;
    collectReclaimedSlots();

    slot->prepare(m_sampleRate, m_maxBlockSize);

    PluginSlot* rawPtr = slot.get();
    index = std::min(index, m_uiSlots.size());
    m_uiSlots.insert(m_uiSlots.begin() + index, rawPtr);

    SlotCommand cmd = SlotCommand::makeInsert(index, std::move(slot));
    if (!m_commandQueue.try_enqueue(std::move(cmd))) {
        m_uiSlots.erase(m_uiSlots.begin() + index);
    }
}

void ParallelBranch::removeSlot(size_t index) {
    collectReclaimedSlots();
    if (index >= m_uiSlots.size()) return;

    PluginSlot* targetPtr = m_uiSlots[index];
    m_uiSlots.erase(m_uiSlots.begin() + index);

    SlotCommand cmd = SlotCommand::makeRemove(index, targetPtr);
    m_commandQueue.try_enqueue(std::move(cmd));
}

void ParallelBranch::reorderSlots(size_t fromIndex, size_t toIndex) {
    collectReclaimedSlots();
    if (fromIndex >= m_uiSlots.size() || toIndex >= m_uiSlots.size() || fromIndex == toIndex) return;

    auto* ptr = m_uiSlots[fromIndex];
    m_uiSlots.erase(m_uiSlots.begin() + fromIndex);
    m_uiSlots.insert(m_uiSlots.begin() + toIndex, ptr);

    SlotCommand cmd = SlotCommand::makeReorder(fromIndex, toIndex);
    m_commandQueue.try_enqueue(std::move(cmd));
}

std::unique_ptr<PluginSlot> ParallelBranch::takeSlot(size_t index) {
    if (m_isAudioRunning.load(std::memory_order_acquire)) {
        return nullptr;
    }
    collectReclaimedSlots();

    // 1. If command queue has unconsumed commands, drain them into active slots
    SlotCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        if (cmd.type == SlotCommandType::Add && cmd.slot) {
            m_activeSlots.push_back(std::move(cmd.slot));
        } else if (cmd.type == SlotCommandType::Insert && cmd.slot) {
            size_t idx = std::min(cmd.index, m_activeSlots.size());
            m_activeSlots.insert(m_activeSlots.begin() + idx, std::move(cmd.slot));
        } else if (cmd.type == SlotCommandType::Reorder) {
            if (cmd.index < m_activeSlots.size() && cmd.targetIndex < m_activeSlots.size() && cmd.index != cmd.targetIndex) {
                auto s = std::move(m_activeSlots[cmd.index]);
                m_activeSlots.erase(m_activeSlots.begin() + cmd.index);
                m_activeSlots.insert(m_activeSlots.begin() + cmd.targetIndex, std::move(s));
            }
        } else if (cmd.type == SlotCommandType::Remove) {
            auto it = m_activeSlots.end();
            if (cmd.targetSlot != nullptr) {
                it = std::find_if(m_activeSlots.begin(), m_activeSlots.end(),
                    [&](const auto& s) { return s.get() == cmd.targetSlot; });
            } else if (cmd.index < m_activeSlots.size()) {
                it = m_activeSlots.begin() + cmd.index;
            }
            if (it != m_activeSlots.end()) {
                m_activeSlots.erase(it);
            }
        }
    }

    // 2. Identify target slot from UI shadow
    PluginSlot* targetPtr = nullptr;
    if (index < m_uiSlots.size()) {
        targetPtr = m_uiSlots[index];
        m_uiSlots.erase(m_uiSlots.begin() + index);
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
    if (m_stashedCount < m_stashedReclamations.size()) {
        m_stashedReclamations[m_stashedCount++] = std::move(slot);
    } else {
        // Emergency overflow: retain in active slots in bypassed state to avoid audio-thread destruction
        slot->setBypassed(true);
        m_activeSlots.push_back(std::move(slot));
    }
}

void ParallelBranch::flushStashedReclamations() noexcept {
    while (m_stashedCount > 0) {
        if (m_reclaimQueue.try_enqueue(std::move(m_stashedReclamations[m_stashedCount - 1]))) {
            --m_stashedCount;
        } else {
            break;
        }
    }
}

void ParallelBranch::process(AudioProcessContext& ctx) {
    m_isAudioRunning.store(true, std::memory_order_release);
    const uint32_t numSamples = ctx.numSamples;
    if (numSamples == 0) return;

    // 1. Drain pending commands from UI thread (Wait-Free)
    SlotCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        switch (cmd.type) {
            case SlotCommandType::Add: {
                if (cmd.slot) {
                    if (m_activeSlots.size() < MAX_BRANCH_SLOTS) {
                        m_activeSlots.push_back(std::move(cmd.slot));
                    } else {
                        // Max capacity reached: reject to reclaim queue without destructing here
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
                        m_activeSlots.insert(m_activeSlots.begin() + idx, std::move(cmd.slot));
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
                    m_activeSlots.erase(m_activeSlots.begin() + cmd.index);
                    m_activeSlots.insert(m_activeSlots.begin() + cmd.targetIndex, std::move(s));
                }
                break;
            }
            case SlotCommandType::Remove: {
                auto it = m_activeSlots.end();
                if (cmd.targetSlot != nullptr) {
                    it = std::find_if(m_activeSlots.begin(), m_activeSlots.end(),
                        [&](const auto& s) { return s.get() == cmd.targetSlot; });
                } else if (cmd.index < m_activeSlots.size()) {
                    it = m_activeSlots.begin() + cmd.index;
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

    // 2. Attempt to flush any previously stashed items
    flushStashedReclamations();

    // 3. Audio Processing through active slots
    auto branchView = m_branchBuffer.view(numSamples);
    branchView.copyFrom(ctx.input);

    for (auto& slot : m_activeSlots) {
        if (!slot) continue;
        auto tempView = m_slotTemp.view(numSamples);
        AudioProcessContext slotCtx{
            .input = branchView,
            .output = tempView,
            .sampleRate = ctx.sampleRate,
            .numSamples = numSamples
        };
        slot->process(slotCtx);
        branchView.copyFrom(tempView);
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

            // Read sample at old delay tap
            float oldL = inSampL, oldR = inSampR;
            if (m_oldDelay > 0) {
                const size_t rOld = (m_delayWritePos + PDC_BUFFER_SIZE - m_oldDelay) & PDC_BUFFER_MASK;
                oldL = m_delayBufferL[rOld];
                oldR = m_delayBufferR[rOld];
            }

            // Read sample at new target delay tap
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

    m_meter.process(branchView.channel(0), branchView.numChannels() > 1 ? branchView.channel(1) : nullptr, numSamples);
}

// ==========================================
// ParallelSplitMergeBlock Implementation
// ==========================================

ParallelSplitMergeBlock::ParallelSplitMergeBlock(std::string name)
    : m_name(std::move(name)) {}

void ParallelSplitMergeBlock::prepare(double sampleRate, uint32_t maxBlockSize) {
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;
    m_mixBuffer.resize(2, maxBlockSize);
    for (auto& branch : m_branches) {
        if (branch) branch->prepare(sampleRate, maxBlockSize);
    }
}

void ParallelSplitMergeBlock::reset() {
    for (auto& branch : m_branches) {
        if (branch) branch->reset();
    }
}

uint32_t ParallelSplitMergeBlock::latencySamples() const noexcept {
    uint32_t maxLat = 0;
    for (const auto& branch : m_branches) {
        if (branch && !branch->isMuted()) {
            maxLat = std::max(maxLat, branch->totalLatencySamples());
        }
    }
    return maxLat;
}

ParallelBranch* ParallelSplitMergeBlock::addBranch(const std::string& branchName) {
    auto branch = std::make_unique<ParallelBranch>(branchName);
    branch->prepare(m_sampleRate, m_maxBlockSize);
    m_branches.push_back(std::move(branch));
    return m_branches.back().get();
}

void ParallelSplitMergeBlock::removeBranch(size_t index) {
    if (index < m_branches.size()) {
        m_branches.erase(m_branches.begin() + index);
    }
}

ParallelBranch* ParallelSplitMergeBlock::getBranch(size_t index) noexcept {
    if (index < m_branches.size()) return m_branches[index].get();
    return nullptr;
}

void ParallelSplitMergeBlock::collectReclaimedSlots() noexcept {
    for (auto& branch : m_branches) {
        if (branch) branch->collectReclaimedSlots();
    }
}

void ParallelSplitMergeBlock::process(AudioProcessContext& ctx) {
    const uint32_t numSamples = ctx.numSamples;
    if (m_branches.empty() || numSamples == 0) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    // 1. Calculate dynamic PDC branch latencies
    uint32_t maxBranchLatency = 0;
    for (const auto& branch : m_branches) {
        if (branch) {
            const uint32_t lat = branch->totalLatencySamples();
            branch->setBranchLatency(lat);
            if (!branch->isMuted()) {
                maxBranchLatency = std::max(maxBranchLatency, lat);
            }
        }
    }

    // 2. Set target delay for each branch: Delta L = max(L) - L_i (up to 16,384 samples)
    for (auto& branch : m_branches) {
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
    for (const auto& branch : m_branches) {
        if (branch->isSolo()) {
            hasSolo = true;
            break;
        }
    }

    const float blendVal = m_blend.load(std::memory_order_relaxed);

    // Process and sum active branches
    for (size_t bIdx = 0; bIdx < m_branches.size(); ++bIdx) {
        auto& branch = m_branches[bIdx];
        if (branch->isMuted()) continue;
        if (hasSolo && !branch->isSolo()) continue;

        branch->process(ctx);
        auto branchOut = branch->buffer().view(numSamples);

        float blendWeight = 1.0f;
        if (m_branches.size() == 2) {
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
        m_currentGain += 0.01f * (targetGain - m_currentGain); // Smooth gain transitions

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
    m_retiringNodes.reserve(MAX_GRAPH_NODES);
}

GraphEngine::~GraphEngine() {
    {
        std::lock_guard<std::mutex> lock(m_uiMutex);
        m_uiNodes.clear();
    }

    // 1. Drain pending commands
    GraphCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        cmd.node.reset();
        cmd.chainPayload.reset();
    }

    // 2. Drain stashed reclamations
    for (size_t i = 0; i < m_stashedCount; ++i) {
        m_stashedReclamations[i].reset();
    }
    m_stashedCount = 0;
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

    // 4. Clear active and retiring nodes
    m_activeNodes.clear();
    m_retiringNodes.clear();
}

void GraphEngine::stashForReclamation(std::unique_ptr<AudioNode> node) noexcept {
    if (m_stashedCount < m_stashedReclamations.size()) {
        m_stashedReclamations[m_stashedCount++] = std::move(node);
    } else {
        // Emergency overflow: retain in active nodes in bypassed state to avoid audio-thread destruction
        node->setBypassed(true);
        if (m_activeNodes.size() < MAX_GRAPH_NODES) {
            m_activeNodes.push_back(std::move(node));
        }
    }
}

void GraphEngine::stashChainForReclamation(std::unique_ptr<NodeChain> chain) noexcept {
    if (m_stashedChainCount < m_stashedChains.size()) {
        m_stashedChains[m_stashedChainCount++] = std::move(chain);
    }
}

void GraphEngine::flushStashedReclamations() noexcept {
    while (m_stashedCount > 0) {
        if (m_reclaimQueue.try_enqueue(std::move(m_stashedReclamations[m_stashedCount - 1]))) {
            --m_stashedCount;
        } else {
            break;
        }
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
                    m_activeNodes.push_back(std::move(cmd.node));
                }
                break;
            }
            case GraphCommandType::Insert: {
                if (cmd.node) {
                    if (cmd.indexA <= m_activeNodes.size()) {
                        m_activeNodes.insert(m_activeNodes.begin() + cmd.indexA, std::move(cmd.node));
                    } else {
                        m_activeNodes.push_back(std::move(cmd.node));
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
                            m_activeNodes.push_back(std::move(n));
                        }
                    }
                }
                break;
            }
            case GraphCommandType::SplitToParallel: {
                if (cmd.indexA < m_activeNodes.size() && cmd.node) {
                    auto originalNode = std::move(m_activeNodes[cmd.indexA]);
                    auto* parallelBlock = dynamic_cast<ParallelSplitMergeBlock*>(cmd.node.get());
                    if (parallelBlock && parallelBlock->numBranches() > 0) {
                        auto* branchA = parallelBlock->getBranch(0);
                        auto* slotA = dynamic_cast<PluginSlot*>(originalNode.get());
                        if (slotA && branchA) {
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
                    m_activeNodes.erase(m_activeNodes.begin() + cmd.indexA);
                    auto* block = dynamic_cast<ParallelSplitMergeBlock*>(blockNode.get());
                    if (block && cmd.indexB < block->numBranches()) {
                        auto* br = block->getBranch(cmd.indexB);
                        if (br) {
                            auto& actSlots = br->activeSlots();
                            size_t insertPos = cmd.indexA;
                            for (auto& s : actSlots) {
                                if (s) {
                                    m_activeNodes.insert(m_activeNodes.begin() + insertPos, std::move(s));
                                    ++insertPos;
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
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;

    m_noiseGate.prepare(sampleRate);
    m_mainProcessingBuffer.resize(2, maxBlockSize);
    m_scratchBuffer.resize(2, maxBlockSize);
    m_retiringBuffer.resize(2, maxBlockSize);
    m_activeBuffer.resize(2, maxBlockSize);
    m_activeNodes.reserve(MAX_GRAPH_NODES);
    m_retiringNodes.reserve(MAX_GRAPH_NODES);
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
    m_hasProcessedAudio = false;
    m_lastBlockSampleL = 0.0f;
    m_lastBlockSampleR = 0.0f;
    m_isSceneCrossfading.store(false, std::memory_order_relaxed);
    m_pendingCrossfadeSamples.store(0, std::memory_order_relaxed);
    m_inplaceCrossfadeRamp.reset(std::max(1u, static_cast<uint32_t>(m_sampleRate * 0.010)));
    m_activeNodes.reserve(MAX_GRAPH_NODES);
    m_retiringNodes.reserve(MAX_GRAPH_NODES);

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

    GraphCommand cmd = GraphCommand::makeAdd(std::move(node));
    if (!m_commandQueue.try_enqueue(std::move(cmd))) {
        m_uiNodes.pop_back();
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
        GraphCommand cmd = GraphCommand::makeAdd(std::move(node));
        if (!m_commandQueue.try_enqueue(std::move(cmd))) {
            m_uiNodes.pop_back();
        }
    } else {
        m_uiNodes.insert(m_uiNodes.begin() + index, rawPtr);
        GraphCommand cmd = GraphCommand::makeInsert(index, std::move(node));
        if (!m_commandQueue.try_enqueue(std::move(cmd))) {
            m_uiNodes.erase(m_uiNodes.begin() + index);
        }
    }
}

void GraphEngine::removeSerialNode(size_t index) {
    processReclamation();
    std::lock_guard<std::mutex> lock(m_uiMutex);
    if (index >= m_uiNodes.size()) return;

    AudioNode* target = m_uiNodes[index];
    m_uiNodes.erase(m_uiNodes.begin() + index);

    GraphCommand cmd = GraphCommand::makeRemove(index, target);
    m_commandQueue.try_enqueue(std::move(cmd));
}

std::unique_ptr<AudioNode> GraphEngine::takeSerialNode(size_t index) {
    if (m_isAudioRunning.load(std::memory_order_acquire)) {
        return nullptr;
    }
    processReclamation();
    std::lock_guard<std::mutex> lock(m_uiMutex);

    // Drain pending commands into active nodes
    drainCommandQueueIntoActive();

    AudioNode* targetPtr = nullptr;
    if (index < m_uiNodes.size()) {
        targetPtr = m_uiNodes[index];
        m_uiNodes.erase(m_uiNodes.begin() + index);
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
        GraphCommand cmd = GraphCommand::makeReorder(fromIndex, toIndex);
        m_commandQueue.try_enqueue(std::move(cmd));
    }
}

void GraphEngine::splitSerialNodeIntoParallel(size_t index) {
    processReclamation();
    std::lock_guard<std::mutex> lock(m_uiMutex);
    if (index >= m_uiNodes.size()) return;

    if (!m_isAudioRunning.load(std::memory_order_acquire)) {
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
    GraphCommand cmd = GraphCommand::makeSplitToParallel(index, std::move(parallelBlock));
    m_commandQueue.try_enqueue(std::move(cmd));

    if (!m_isAudioRunning.load(std::memory_order_acquire)) {
        drainCommandQueueIntoActive();
    }
}

void GraphEngine::dissolveParallelBlock(size_t blockIndex, int branchToKeep) {
    processReclamation();
    std::lock_guard<std::mutex> lock(m_uiMutex);
    if (blockIndex >= m_uiNodes.size()) return;

    if (!m_isAudioRunning.load(std::memory_order_acquire)) {
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
    GraphCommand cmd = GraphCommand::makeDissolveParallel(blockIndex, branchToKeep);
    m_commandQueue.try_enqueue(std::move(cmd));

    if (!m_isAudioRunning.load(std::memory_order_acquire)) {
        drainCommandQueueIntoActive();
    }
}

void GraphEngine::clearNodes() {
    processReclamation();
    std::lock_guard<std::mutex> lock(m_uiMutex);
    m_uiNodes.clear();
    m_retiringNodes.clear();
    m_sceneCrossfadeRamp.reset(1);

    GraphCommand cmd = GraphCommand::makeClear();
    m_commandQueue.try_enqueue(std::move(cmd));
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

void GraphEngine::process(const AudioBufferView& hardwareIn, AudioBufferView& hardwareOut) {
    m_isAudioRunning.store(true, std::memory_order_release);
    const uint32_t numSamples = hardwareIn.numSamples();
    if (numSamples == 0) return;

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
                for (auto& n : m_retiringNodes) {
                    if (n) {
                        if (!m_reclaimQueue.try_enqueue(std::move(n))) {
                            stashForReclamation(std::move(n));
                        }
                    }
                }
                m_retiringNodes.clear();

                for (auto& n : m_activeNodes) {
                    if (n) {
                        m_retiringNodes.push_back(std::move(n));
                    }
                }
                m_activeNodes.clear();

                if (cmd.chainPayload) {
                    for (auto& n : cmd.chainPayload->nodes) {
                        if (n) {
                            m_activeNodes.push_back(std::move(n));
                        }
                    }
                    cmd.chainPayload->nodes.clear();
                    if (!m_chainReclaimQueue.try_enqueue(std::move(cmd.chainPayload))) {
                        stashChainForReclamation(std::move(cmd.chainPayload));
                    }
                }

                const uint32_t rampSamples = static_cast<uint32_t>(m_sampleRate * 0.010);
                m_sceneCrossfadeRamp.reset(std::max(1u, rampSamples));
                m_sceneCrossfadeRamp.startTransition(true);
                m_isSceneCrossfading.store(true, std::memory_order_release);
                break;
            }
            case GraphCommandType::SplitToParallel: {
                if (cmd.indexA < m_activeNodes.size() && cmd.node) {
                    auto originalNode = std::move(m_activeNodes[cmd.indexA]);
                    auto* parallelBlock = dynamic_cast<ParallelSplitMergeBlock*>(cmd.node.get());
                    if (parallelBlock && parallelBlock->numBranches() > 0) {
                        auto* branchA = parallelBlock->getBranch(0);
                        auto* slotA = dynamic_cast<PluginSlot*>(originalNode.get());
                        if (slotA && branchA) {
                            originalNode.release();
                            branchA->addSlot(std::unique_ptr<PluginSlot>(slotA));
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
                    m_activeNodes.erase(m_activeNodes.begin() + cmd.indexA);
                    auto* block = dynamic_cast<ParallelSplitMergeBlock*>(blockNode.get());
                    if (block && cmd.indexB < block->numBranches()) {
                        auto* br = block->getBranch(cmd.indexB);
                        if (br) {
                            auto& actSlots = br->activeSlots();
                            size_t insertPos = cmd.indexA;
                            for (auto& s : actSlots) {
                                if (s) {
                                    m_activeNodes.insert(m_activeNodes.begin() + insertPos, std::move(s));
                                    ++insertPos;
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

    if (m_inputConfig.mode == InputRoutingMode::MonoLeft && inCh > 0) {
        const float* src = hardwareIn.channel(0);
        std::memcpy(mainL, src, numSamples * sizeof(float));
        std::memcpy(mainR, src, numSamples * sizeof(float));
    } else if (m_inputConfig.mode == InputRoutingMode::MonoRight && inCh > 1) {
        const float* src = hardwareIn.channel(1);
        std::memcpy(mainL, src, numSamples * sizeof(float));
        std::memcpy(mainR, src, numSamples * sizeof(float));
    } else if (m_inputConfig.mode == InputRoutingMode::MonoChannel && inCh > 0) {
        const uint32_t ch = std::min(static_cast<uint32_t>(m_inputConfig.channelLeft), inCh - 1);
        const float* src = hardwareIn.channel(ch);
        std::memcpy(mainL, src, numSamples * sizeof(float));
        std::memcpy(mainR, src, numSamples * sizeof(float));
    } else if (m_inputConfig.mode == InputRoutingMode::Stereo) {
        const float* srcL = inCh > 0 ? hardwareIn.channel(0) : nullptr;
        const float* srcR = inCh > 1 ? hardwareIn.channel(1) : srcL;
        if (srcL) std::memcpy(mainL, srcL, numSamples * sizeof(float));
        else std::memset(mainL, 0, numSamples * sizeof(float));
        if (srcR) std::memcpy(mainR, srcR, numSamples * sizeof(float));
        else std::memset(mainR, 0, numSamples * sizeof(float));
    } else if (m_inputConfig.mode == InputRoutingMode::StereoCustom && inCh > 0) {
        const uint32_t chL = std::min(static_cast<uint32_t>(m_inputConfig.channelLeft), inCh - 1);
        const uint32_t chR = std::min(static_cast<uint32_t>(m_inputConfig.channelRight), inCh - 1);
        std::memcpy(mainL, hardwareIn.channel(chL), numSamples * sizeof(float));
        std::memcpy(mainR, hardwareIn.channel(chR), numSamples * sizeof(float));
    } else {
        mainView.copyFrom(hardwareIn);
    }

    // Track input levels
    m_inputMeter.process(mainL, mainR, numSamples);

    // Apply Master Input Gain
    const float inGain = DspUtils::dbToGain(m_inputGainDb.load(std::memory_order_relaxed));
    mainView.applyGain(inGain);

    // Apply Input Noise Gate
    m_noiseGate.process(mainView);

    // Execute serial rack chain (Completely Lock-Free, 0 Mutex Locks)
    if (m_sceneCrossfadeRamp.isTransitioning()) {
        // 1. Process Retiring (Outgoing) Chain
        auto retView = m_retiringBuffer.view(numSamples);
        retView.copyFrom(mainView);
        for (auto& node : m_retiringNodes) {
            if (!node) continue;
            auto stepScratch = m_scratchBuffer.view(numSamples);
            AudioProcessContext ctx{
                .input = retView,
                .output = stepScratch,
                .sampleRate = m_sampleRate,
                .numSamples = numSamples
            };
            node->process(ctx);
            retView.copyFrom(stepScratch);
        }

        // 2. Process Active (Incoming) Chain
        auto actView = m_activeBuffer.view(numSamples);
        actView.copyFrom(mainView);
        for (auto& node : m_activeNodes) {
            if (!node) continue;
            auto stepScratch = m_scratchBuffer.view(numSamples);
            AudioProcessContext ctx{
                .input = actView,
                .output = stepScratch,
                .sampleRate = m_sampleRate,
                .numSamples = numSamples
            };
            node->process(ctx);
            actView.copyFrom(stepScratch);
        }

        // 3. Sample-by-Sample EqualPower Crossfade
        const uint32_t numCh = mainView.numChannels();
        for (uint32_t s = 0; s < numSamples; ++s) {
            float gainOut = 0.0f, gainIn = 0.0f;
            m_sceneCrossfadeRamp.getNextGains(gainOut, gainIn);
            for (uint32_t ch = 0; ch < numCh; ++ch) {
                float* out = mainView.channel(ch);
                const float* oldS = retView.channel(ch);
                const float* newS = actView.channel(ch);
                out[s] = (oldS[s] * gainOut) + (newS[s] * gainIn);
            }
        }

        if (!m_sceneCrossfadeRamp.isTransitioning()) {
            m_isSceneCrossfading.store(false, std::memory_order_release);
            for (auto& node : m_retiringNodes) {
                if (node) {
                    if (!m_reclaimQueue.try_enqueue(std::move(node))) {
                        stashForReclamation(std::move(node));
                    }
                }
            }
            m_retiringNodes.clear();
        }
    } else {
        for (auto& node : m_activeNodes) {
            if (!node) continue;
            auto scratchView = m_scratchBuffer.view(numSamples);
            AudioProcessContext ctx{
                .input = mainView,
                .output = scratchView,
                .sampleRate = m_sampleRate,
                .numSamples = numSamples
            };
            node->process(ctx);
            mainView.copyFrom(scratchView);
        }

        // Apply smooth 10ms equal-power crossfade between previous audio tail and new tone
        if (m_inplaceCrossfadeRamp.isTransitioning()) {
            const uint32_t numCh = mainView.numChannels();
            const uint32_t tailLimit = std::min(numSamples, m_outgoingTailBuffer.maxSamples());
            auto tailView = m_outgoingTailBuffer.view(tailLimit);
            for (uint32_t s = 0; s < numSamples; ++s) {
                float gainOld = 0.0f, gainNew = 0.0f;
                m_inplaceCrossfadeRamp.getNextGains(gainOld, gainNew);
                for (uint32_t ch = 0; ch < numCh; ++ch) {
                    float* out = mainView.channel(ch);
                    float oldS = (s < tailLimit && ch < tailView.numChannels())
                                 ? tailView.channel(ch)[s]
                                 : (ch == 0 ? m_lastBlockSampleL : m_lastBlockSampleR);
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

    // Soft Safety Limiter (transparent cubic saturator at +/-0.98 FS to prevent harsh ear damage)
    for (uint32_t ch = 0; ch < mainView.numChannels(); ++ch) {
        float* data = mainView.channel(ch);
        for (uint32_t s = 0; s < numSamples; ++s) {
            float x = data[s];
            if (!std::isfinite(x)) {
                x = 0.0f;
            } else if (x > 0.95f) {
                x = 0.95f + (1.0f - 0.95f) * std::tanh((x - 0.95f) / (1.0f - 0.95f));
            } else if (x < -0.95f) {
                x = -0.95f + (1.0f - 0.95f) * std::tanh((x + 0.95f) / (1.0f - 0.95f));
            }
            data[s] = x;
        }
    }

    hardwareOut.copyFrom(mainView);

    // Store outgoing audio tail for smooth in-place crossfading on tone / preset switches
    if (numSamples > 0 && numSamples <= m_outgoingTailBuffer.maxSamples()) {
        auto tailView = m_outgoingTailBuffer.view(numSamples);
        tailView.copyFrom(mainView);
    }

    // Store last samples for micro-crossfading on structural mutations
    m_lastBlockSampleL = hardwareOut.channel(0)[numSamples - 1];
    m_lastBlockSampleR = (hardwareOut.numChannels() > 1) ? hardwareOut.channel(1)[numSamples - 1] : m_lastBlockSampleL;
    m_hasProcessedAudio = true;

    // Track output levels
    m_outputMeter.process(hardwareOut.channel(0), hardwareOut.numChannels() > 1 ? hardwareOut.channel(1) : nullptr, numSamples);
}

} // namespace praccy::audio
