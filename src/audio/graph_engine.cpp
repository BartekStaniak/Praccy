#include "graph_engine.h"
#include <cmath>
#include <algorithm>

namespace praccy::audio {

// ==========================================
// PluginSlot Implementation
// ==========================================

PluginSlot::PluginSlot(std::unique_ptr<AudioNode> innerNode)
    : m_innerNode(std::move(innerNode)) {}

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
    for (uint32_t ch = 0; ch < numCh; ++ch) {
        float* out = ctx.output.channel(ch);
        const float* dry = dryView.channel(ch);
        const float* wet = procView.channel(ch);

        for (uint32_t s = 0; s < numSamples; ++s) {
            float blended = (dry[s] * dryGain) + (wet[s] * wetGain);

            if (transitioning) {
                float rampOld = 0.0f, rampNew = 0.0f;
                m_bypassRamp.getNextGains(rampOld, rampNew);
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

    // Drain any pending commands before preparing
    SlotCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        if (cmd.type == SlotCommandType::Add && cmd.slot) {
            m_activeSlots.push_back(std::move(cmd.slot));
        }
    }

    for (auto& slot : m_activeSlots) {
        if (slot) slot->prepare(sampleRate, maxBlockSize);
    }
}

void ParallelBranch::reset() {
    for (auto& slot : m_activeSlots) {
        if (slot) slot->reset();
    }
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

void ParallelBranch::removeSlot(size_t index) {
    collectReclaimedSlots();
    if (index >= m_uiSlots.size()) return;

    PluginSlot* targetPtr = m_uiSlots[index];
    m_uiSlots.erase(m_uiSlots.begin() + index);

    SlotCommand cmd = SlotCommand::makeRemove(index, targetPtr);
    m_commandQueue.try_enqueue(std::move(cmd));
}

std::unique_ptr<PluginSlot> ParallelBranch::takeSlot(size_t index) {
    collectReclaimedSlots();

    // 1. If command queue has unconsumed commands, drain them into active slots
    SlotCommand cmd;
    while (m_commandQueue.try_dequeue(cmd)) {
        if (cmd.type == SlotCommandType::Add && cmd.slot) {
            m_activeSlots.push_back(std::move(cmd.slot));
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

    m_meter.process(branchView.channel(0), branchView.numChannels() > 1 ? branchView.channel(1) : nullptr, numSamples);
}

// ==========================================
// ParallelSplitMergeBlock Implementation
// ==========================================

ParallelSplitMergeBlock::ParallelSplitMergeBlock(std::string name)
    : m_name(std::move(name)) {}

void ParallelSplitMergeBlock::prepare(double sampleRate, uint32_t maxBlockSize) {
    m_mixBuffer.resize(2, maxBlockSize);
    for (auto& branch : m_branches) {
        branch->prepare(sampleRate, maxBlockSize);
    }
}

void ParallelSplitMergeBlock::reset() {
    for (auto& branch : m_branches) {
        branch->reset();
    }
}

ParallelBranch* ParallelSplitMergeBlock::addBranch(const std::string& branchName) {
    m_branches.push_back(std::make_unique<ParallelBranch>(branchName));
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

GraphEngine::GraphEngine() = default;
GraphEngine::~GraphEngine() = default;

void GraphEngine::prepare(double sampleRate, uint32_t maxBlockSize) {
    m_sampleRate = sampleRate;
    m_maxBlockSize = maxBlockSize;

    m_noiseGate.prepare(sampleRate);
    m_mainProcessingBuffer.resize(2, maxBlockSize);
    m_scratchBuffer.resize(2, maxBlockSize);
    m_retiringBuffer.resize(2, maxBlockSize);
    m_activeBuffer.resize(2, maxBlockSize);
    const uint32_t rampSamples = static_cast<uint32_t>(sampleRate * 0.010);
    m_sceneCrossfadeRamp.reset(std::max(1u, rampSamples));

    std::lock_guard<std::mutex> lock(m_graphMutex);
    for (auto& node : m_nodes) {
        node->prepare(sampleRate, maxBlockSize);
    }
}

void GraphEngine::reset() {
    std::lock_guard<std::mutex> lock(m_graphMutex);
    for (auto& node : m_nodes) {
        node->reset();
    }
}

void GraphEngine::addSerialNode(std::unique_ptr<AudioNode> node) {
    std::lock_guard<std::mutex> lock(m_graphMutex);
    if (node) {
        node->prepare(m_sampleRate, m_maxBlockSize);
        m_nodes.push_back(std::move(node));
    }
}

void GraphEngine::removeSerialNode(size_t index) {
    std::lock_guard<std::mutex> lock(m_graphMutex);
    if (index < m_nodes.size()) {
        m_nodes.erase(m_nodes.begin() + index);
    }
}

void GraphEngine::splitSerialNodeIntoParallel(size_t index) {
    std::lock_guard<std::mutex> lock(m_graphMutex);
    if (index >= m_nodes.size()) return;

    auto originalNode = std::move(m_nodes[index]);

    auto parallelBlock = std::make_unique<ParallelSplitMergeBlock>("Parallel Split/Merge");
    parallelBlock->prepare(m_sampleRate, m_maxBlockSize);

    auto* branchA = parallelBlock->addBranch("Branch A");
    branchA->prepare(m_sampleRate, m_maxBlockSize);
    branchA->setPan(-0.5f);

    auto* slotA = dynamic_cast<PluginSlot*>(originalNode.get());
    if (slotA) {
        originalNode.release();
        branchA->addSlot(std::unique_ptr<PluginSlot>(slotA));
    }

    auto* branchB = parallelBlock->addBranch("Branch B");
    branchB->prepare(m_sampleRate, m_maxBlockSize);
    branchB->setPan(+0.5f);

    m_nodes[index] = std::move(parallelBlock);
}

void GraphEngine::dissolveParallelBlock(size_t blockIndex, int branchToKeep) {
    std::lock_guard<std::mutex> lock(m_graphMutex);
    if (blockIndex >= m_nodes.size()) return;

    auto* block = dynamic_cast<ParallelSplitMergeBlock*>(m_nodes[blockIndex].get());
    if (!block) return;

    // Collect preserved slots from the branch to keep
    std::vector<std::unique_ptr<PluginSlot>> preservedSlots;
    if (branchToKeep >= 0 && static_cast<size_t>(branchToKeep) < block->numBranches()) {
        auto* br = block->getBranch(branchToKeep);
        if (br) {
            while (br->numSlots() > 0) {
                auto slotPtr = br->takeSlot(0);
                if (slotPtr) preservedSlots.push_back(std::move(slotPtr));
            }
        }
    }

    // Remove the parallel block
    m_nodes.erase(m_nodes.begin() + blockIndex);

    // Splice preserved slots into the serial chain at blockIndex
    for (size_t i = 0; i < preservedSlots.size(); ++i) {
        m_nodes.insert(m_nodes.begin() + blockIndex + i, std::move(preservedSlots[i]));
    }
}

void GraphEngine::clearNodes() {
    std::lock_guard<std::mutex> lock(m_graphMutex);
    m_nodes.clear();
    m_retiringNodes.clear();
    m_sceneCrossfadeRamp.reset(1);
}

void GraphEngine::crossfadeToNodes(std::vector<std::unique_ptr<AudioNode>> newNodes) {
    std::lock_guard<std::mutex> lock(m_graphMutex);
    for (auto& node : newNodes) {
        if (node) {
            node->prepare(m_sampleRate, m_maxBlockSize);
        }
    }
    m_retiringNodes = std::move(m_nodes);
    m_nodes = std::move(newNodes);
    const uint32_t rampSamples = static_cast<uint32_t>(m_sampleRate * 0.010);
    m_sceneCrossfadeRamp.reset(std::max(1u, rampSamples));
    m_sceneCrossfadeRamp.startTransition(true);
}

void GraphEngine::processReclamation() noexcept {
    std::unique_lock<std::mutex> lock(m_graphMutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!m_sceneCrossfadeRamp.isTransitioning() && !m_retiringNodes.empty()) {
        m_retiringNodes.clear();
    }
    for (auto& node : m_nodes) {
        if (auto* splitBlock = dynamic_cast<ParallelSplitMergeBlock*>(node.get())) {
            splitBlock->collectReclaimedSlots();
        }
    }
}

size_t GraphEngine::numNodes() const noexcept {
    return m_nodes.size();
}

AudioNode* GraphEngine::getNode(size_t index) noexcept {
    if (index < m_nodes.size()) return m_nodes[index].get();
    return nullptr;
}

void GraphEngine::process(const AudioBufferView& hardwareIn, AudioBufferView& hardwareOut) {
    const uint32_t numSamples = hardwareIn.numSamples();
    if (numSamples == 0) return;

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

    // Execute serial rack chain
    {
        std::unique_lock<std::mutex> lock(m_graphMutex, std::try_to_lock);
        if (lock.owns_lock()) {
            if (m_sceneCrossfadeRamp.isTransitioning()) {
                // 1. Process Retiring (Outgoing) Chain
                auto retView = m_retiringBuffer.view(numSamples);
                retView.copyFrom(mainView);
                for (auto& node : m_retiringNodes) {
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
                for (auto& node : m_nodes) {
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
            } else {
                for (auto& node : m_nodes) {
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
            if (x > 0.95f) {
                x = 0.95f + (1.0f - 0.95f) * std::tanh((x - 0.95f) / (1.0f - 0.95f));
            } else if (x < -0.95f) {
                x = -0.95f + (1.0f - 0.95f) * std::tanh((x + 0.95f) / (1.0f - 0.95f));
            }
            data[s] = x;
        }
    }

    hardwareOut.copyFrom(mainView);

    // Track output levels
    m_outputMeter.process(hardwareOut.channel(0), hardwareOut.numChannels() > 1 ? hardwareOut.channel(1) : nullptr, numSamples);
}

} // namespace praccy::audio
