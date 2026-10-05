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

    // If fully bypassed and not transitioning, pure pass-through
    if (currentlyBypassed && !transitioning) {
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
    : m_name(std::move(name)) {}

void ParallelBranch::prepare(double sampleRate, uint32_t maxBlockSize) {
    m_branchBuffer.resize(2, maxBlockSize);
    m_slotTemp.resize(2, maxBlockSize);
    for (auto& slot : m_slots) {
        slot->prepare(sampleRate, maxBlockSize);
    }
}

void ParallelBranch::reset() {
    for (auto& slot : m_slots) {
        slot->reset();
    }
}

void ParallelBranch::addSlot(std::unique_ptr<PluginSlot> slot) {
    m_slots.push_back(std::move(slot));
}

PluginSlot* ParallelBranch::getSlot(size_t index) noexcept {
    if (index < m_slots.size()) return m_slots[index].get();
    return nullptr;
}

void ParallelBranch::process(AudioProcessContext& ctx) {
    const uint32_t numSamples = ctx.numSamples;
    auto branchView = m_branchBuffer.view(numSamples);
    branchView.copyFrom(ctx.input);

    for (auto& slot : m_slots) {
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

ParallelBranch* ParallelSplitMergeBlock::getBranch(size_t index) noexcept {
    if (index < m_branches.size()) return m_branches[index].get();
    return nullptr;
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

    // Process and sum active branches
    for (auto& branch : m_branches) {
        if (branch->isMuted()) continue;
        if (hasSolo && !branch->isSolo()) continue;

        branch->process(ctx);
        auto branchOut = branch->buffer().view(numSamples);

        const float gain = DspUtils::dbToGain(branch->gainDb()) * (branch->isPhaseInvert() ? -1.0f : 1.0f);
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

void GraphEngine::clearNodes() {
    std::lock_guard<std::mutex> lock(m_graphMutex);
    m_nodes.clear();
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

    // Track input levels
    m_inputMeter.process(hardwareIn.channel(0), hardwareIn.numChannels() > 1 ? hardwareIn.channel(1) : nullptr, numSamples);

    auto mainView = m_mainProcessingBuffer.view(numSamples);
    mainView.copyFrom(hardwareIn);

    // Apply Master Input Gain
    const float inGain = DspUtils::dbToGain(m_inputGainDb.load(std::memory_order_relaxed));
    mainView.applyGain(inGain);

    // Apply Input Noise Gate
    m_noiseGate.process(mainView);

    // Execute serial rack chain
    {
        std::unique_lock<std::mutex> lock(m_graphMutex, std::try_to_lock);
        if (lock.owns_lock()) {
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
