#pragma once

#include "audio_buffer.h"
#include "audio_node.h"
#include "dsp_utils.h"
#include "input_config.h"
#include "param_automation.h"
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <array>
#include <algorithm>
#include "readerwriterqueue/readerwriterqueue.h"

namespace praccy::audio {

/**
 * @brief Wrapper around a plugin node providing per-node Dry/Wet mix, Gain trim,
 * wait-free parameter automation, and click-free bypass crossfade transitions.
 */
class PluginSlot : public AudioNode {
public:
    explicit PluginSlot(std::unique_ptr<AudioNode> innerNode);

    void prepare(double sampleRate, uint32_t maxBlockSize) override;
    void process(AudioProcessContext& ctx) override;
    void reset() override;

    [[nodiscard]] NodeType type() const noexcept override { return NodeType::Plugin; }
    [[nodiscard]] const std::string& name() const noexcept override;

    void setDryWet(float mix) noexcept { m_dryWet.store(std::clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed); }
    [[nodiscard]] float dryWet() const noexcept { return m_dryWet.load(std::memory_order_relaxed); }

    void setInputGainDb(float db) noexcept { m_inputGainDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float inputGainDb() const noexcept { return m_inputGainDb.load(std::memory_order_relaxed); }

    void setOutputGainDb(float db) noexcept { m_outputGainDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float outputGainDb() const noexcept { return m_outputGainDb.load(std::memory_order_relaxed); }

    void setBypassed(bool bypassed) noexcept override;
    [[nodiscard]] bool isBypassed() const noexcept override;
    [[nodiscard]] bool isFaulted() const noexcept override {
        return m_innerNode ? m_innerNode->isFaulted() : false;
    }

    [[nodiscard]] uint32_t latencySamples() const noexcept override {
        if (isBypassed() || !m_innerNode || m_innerNode->isFaulted() || m_innerNode->isBypassed()) {
            return 0;
        }
        return m_innerNode->latencySamples();
    }

    // Parameter automation registration (Wait-free, Lock-free)
    void setParameter(uint32_t paramId, float value) noexcept {
        m_automationRegister.setParameter(paramId, value);
    }
    [[nodiscard]] float getParameter(uint32_t paramId) const noexcept {
        return m_automationRegister.getParameter(paramId);
    }
    [[nodiscard]] ParamAutomationRegister& automationRegister() noexcept {
        return m_automationRegister;
    }
    [[nodiscard]] const ParamAutomationRegister& automationRegister() const noexcept {
        return m_automationRegister;
    }

    [[nodiscard]] AudioNode* innerNode() const noexcept { return m_innerNode.get(); }
    [[nodiscard]] LevelMeter& meter() noexcept { return m_meter; }

private:
    std::unique_ptr<AudioNode> m_innerNode;
    std::atomic<float> m_dryWet{1.0f};      // 1.0 = 100% wet
    std::atomic<float> m_inputGainDb{0.0f};  // 0 dB
    std::atomic<float> m_outputGainDb{0.0f}; // 0 dB

    ParamAutomationRegister m_automationRegister;
    EqualPowerRamp m_bypassRamp;
    OwnedAudioBuffer m_dryBuffer;
    OwnedAudioBuffer m_processBuffer;
    LevelMeter m_meter;
};

enum class SlotCommandType : uint8_t {
    Add,
    Remove,
    Clear
};

/**
 * @brief Lock-free command payload dispatched from UI thread to audio thread for parallel branches.
 */
struct SlotCommand {
    SlotCommandType type{SlotCommandType::Add};
    std::unique_ptr<PluginSlot> slot{nullptr};
    size_t index{0};
    PluginSlot* targetSlot{nullptr};

    static SlotCommand makeAdd(std::unique_ptr<PluginSlot> newSlot) noexcept {
        SlotCommand cmd;
        cmd.type = SlotCommandType::Add;
        cmd.slot = std::move(newSlot);
        return cmd;
    }

    static SlotCommand makeRemove(size_t slotIndex, PluginSlot* target = nullptr) noexcept {
        SlotCommand cmd;
        cmd.type = SlotCommandType::Remove;
        cmd.index = slotIndex;
        cmd.targetSlot = target;
        return cmd;
    }

    static SlotCommand makeClear() noexcept {
        SlotCommand cmd;
        cmd.type = SlotCommandType::Clear;
        return cmd;
    }
};

/**
 * @brief A single parallel branch containing a serial chain of plugin slots with
 * dynamic Parallel Delay Compensation (PDC) and click-free crossfading.
 * Concurrency-safe: UI mutations use lock-free SPSC command and reclamation queues.
 */
class ParallelBranch {
public:
    static constexpr size_t MAX_BRANCH_SLOTS = 32;
    static constexpr size_t QUEUE_CAPACITY = 64;
    static constexpr size_t PDC_BUFFER_SIZE = 16384;
    static constexpr size_t PDC_BUFFER_MASK = PDC_BUFFER_SIZE - 1;

    explicit ParallelBranch(std::string name);
    ~ParallelBranch();

    void prepare(double sampleRate, uint32_t maxBlockSize);
    void reset();

    // UI-thread mutations (Push to SPSC command queue, update UI shadow list)
    void addSlot(std::unique_ptr<PluginSlot> slot);
    void removeSlot(size_t index);
    std::unique_ptr<PluginSlot> takeSlot(size_t index);

    // Thread-safe UI readouts (Reads UI shadow list, zero lock contention)
    [[nodiscard]] size_t numSlots() const noexcept { return m_uiSlots.size(); }
    [[nodiscard]] PluginSlot* getSlot(size_t index) noexcept {
        return (index < m_uiSlots.size()) ? m_uiSlots[index] : nullptr;
    }

    // Audio thread processing (Drains commands wait-free, pushes retired to reclaim queue)
    void process(AudioProcessContext& ctx);

    // Reclamation collection (Must be called from UI thread)
    void collectReclaimedSlots() noexcept;

    // Latency reporting and Parallel Delay Compensation (PDC)
    [[nodiscard]] uint32_t totalLatencySamples() const noexcept;
    [[nodiscard]] uint32_t branchLatency() const noexcept {
        return m_branchLatency.load(std::memory_order_relaxed);
    }
    void setBranchLatency(uint32_t lat) noexcept {
        m_branchLatency.store(lat, std::memory_order_relaxed);
    }
    void setTargetDelay(uint32_t target) noexcept;
    [[nodiscard]] uint32_t currentDelay() const noexcept { return m_currentDelay; }
    [[nodiscard]] uint32_t targetDelay() const noexcept { return m_targetDelay; }
    [[nodiscard]] bool isCrossfading() const noexcept { return m_isCrossfading; }
    [[nodiscard]] const std::vector<float>& delayBufferL() const noexcept { return m_delayBufferL; }
    [[nodiscard]] const std::vector<float>& delayBufferR() const noexcept { return m_delayBufferR; }

    void setGainDb(float db) noexcept { m_gainDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float gainDb() const noexcept { return m_gainDb.load(std::memory_order_relaxed); }

    void setPan(float pan) noexcept { m_pan.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed); }
    [[nodiscard]] float pan() const noexcept { return m_pan.load(std::memory_order_relaxed); }

    void setMuted(bool muted) noexcept { m_muted.store(muted, std::memory_order_relaxed); }
    [[nodiscard]] bool isMuted() const noexcept { return m_muted.load(std::memory_order_relaxed); }

    void setSolo(bool solo) noexcept { m_solo.store(solo, std::memory_order_relaxed); }
    [[nodiscard]] bool isSolo() const noexcept { return m_solo.load(std::memory_order_relaxed); }

    void setPhaseInvert(bool invert) noexcept { m_phaseInvert.store(invert, std::memory_order_relaxed); }
    [[nodiscard]] bool isPhaseInvert() const noexcept { return m_phaseInvert.load(std::memory_order_relaxed); }

    [[nodiscard]] const std::string& name() const noexcept { return m_name; }
    [[nodiscard]] LevelMeter& meter() noexcept { return m_meter; }
    [[nodiscard]] OwnedAudioBuffer& buffer() noexcept { return m_branchBuffer; }

private:
    void stashForReclamation(std::unique_ptr<PluginSlot> slot) noexcept;
    void flushStashedReclamations() noexcept;

    std::string m_name;

    // 1. UI-Thread Shadow Registry (Only accessed from UI thread)
    std::vector<PluginSlot*> m_uiSlots;

    // 2. Audio-Thread Active Slots (Only accessed from Audio thread in process())
    std::vector<std::unique_ptr<PluginSlot>> m_activeSlots;

    // 3. SPSC Command Queue: UI Thread -> Audio Thread
    moodycamel::ReaderWriterQueue<SlotCommand> m_commandQueue{QUEUE_CAPACITY};

    // 4. SPSC Reclamation Queue: Audio Thread -> UI Thread
    moodycamel::ReaderWriterQueue<std::unique_ptr<PluginSlot>> m_reclaimQueue{QUEUE_CAPACITY};

    // 5. Fail-Safe Audio Thread Stash (Prevents destruction on audio thread if queue is saturated)
    std::array<std::unique_ptr<PluginSlot>, 16> m_stashedReclamations;
    size_t m_stashedCount{0};

    // 6. Parallel Delay Compensation (PDC) stereo circular delay lines (16,384 samples)
    std::vector<float> m_delayBufferL;
    std::vector<float> m_delayBufferR;
    size_t m_delayWritePos{0};
    uint32_t m_currentDelay{0};
    uint32_t m_targetDelay{0};
    uint32_t m_oldDelay{0};
    uint32_t m_crossfadePos{0};
    bool m_isCrossfading{false};
    std::atomic<uint32_t> m_branchLatency{0};

    std::atomic<float> m_gainDb{0.0f};
    std::atomic<float> m_pan{0.0f};
    std::atomic<bool> m_muted{false};
    std::atomic<bool> m_solo{false};
    std::atomic<bool> m_phaseInvert{false};

    OwnedAudioBuffer m_branchBuffer;
    OwnedAudioBuffer m_slotTemp;
    LevelMeter m_meter;

    double m_sampleRate{48000.0};
    uint32_t m_maxBlockSize{256};
};

/**
 * @brief Split & Merge Block: splits audio into parallel branches and mixes them,
 * coordinating dynamic phase-coherent Parallel Delay Compensation.
 */
class ParallelSplitMergeBlock : public AudioNode {
public:
    explicit ParallelSplitMergeBlock(std::string name = "Parallel Split/Merge");

    void prepare(double sampleRate, uint32_t maxBlockSize) override;
    void process(AudioProcessContext& ctx) override;
    void reset() override;

    [[nodiscard]] NodeType type() const noexcept override { return NodeType::ParallelSplitMerge; }
    [[nodiscard]] const std::string& name() const noexcept override { return m_name; }

    [[nodiscard]] uint32_t latencySamples() const noexcept override;

    ParallelBranch* addBranch(const std::string& branchName);
    void removeBranch(size_t index);
    [[nodiscard]] size_t numBranches() const noexcept { return m_branches.size(); }
    [[nodiscard]] ParallelBranch* getBranch(size_t index) noexcept;

    void setBlend(float blend) noexcept { m_blend.store(std::clamp(blend, -1.0f, 1.0f), std::memory_order_relaxed); }
    [[nodiscard]] float blend() const noexcept { return m_blend.load(std::memory_order_relaxed); }

    void collectReclaimedSlots() noexcept;

private:
    std::string m_name;
    std::vector<std::unique_ptr<ParallelBranch>> m_branches;
    OwnedAudioBuffer m_mixBuffer;
    std::atomic<float> m_blend{0.0f};
    double m_sampleRate{48000.0};
    uint32_t m_maxBlockSize{256};
};

/**
 * @brief Built-in input noise gate (crucial for high-gain instrument practicing).
 */
class NoiseGate {
public:
    NoiseGate() = default;

    void prepare(double sampleRate);
    void process(AudioBufferView& buffer) noexcept;

    void setThresholdDb(float db) noexcept { m_thresholdDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float thresholdDb() const noexcept { return m_thresholdDb.load(std::memory_order_relaxed); }

    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

private:
    std::atomic<float> m_thresholdDb{-60.0f};
    std::atomic<bool> m_enabled{false};
    float m_envelope{0.0f};
    float m_attackCoeff{0.0f};
    float m_releaseCoeff{0.0f};
    float m_currentGain{1.0f};
};

enum class GraphCommandType : uint8_t {
    Add,
    Insert,
    Remove,
    Swap,
    Reorder,
    Clear,
    CrossfadeTo
};

/**
 * @brief Container for a complete node chain transferred between UI and Audio threads.
 */
struct NodeChain {
    std::vector<std::unique_ptr<AudioNode>> nodes;
};

/**
 * @brief Lock-free command payload dispatched from UI thread to GraphEngine audio thread.
 */
struct GraphCommand {
    GraphCommandType type{GraphCommandType::Add};
    std::unique_ptr<AudioNode> node{nullptr};
    std::unique_ptr<NodeChain> chainPayload{nullptr};
    size_t indexA{0};
    size_t indexB{0};
    AudioNode* targetNode{nullptr};

    static GraphCommand makeAdd(std::unique_ptr<AudioNode> n) noexcept {
        GraphCommand cmd;
        cmd.type = GraphCommandType::Add;
        cmd.node = std::move(n);
        return cmd;
    }

    static GraphCommand makeInsert(size_t idx, std::unique_ptr<AudioNode> n) noexcept {
        GraphCommand cmd;
        cmd.type = GraphCommandType::Insert;
        cmd.indexA = idx;
        cmd.node = std::move(n);
        return cmd;
    }

    static GraphCommand makeRemove(size_t idx, AudioNode* target = nullptr) noexcept {
        GraphCommand cmd;
        cmd.type = GraphCommandType::Remove;
        cmd.indexA = idx;
        cmd.targetNode = target;
        return cmd;
    }

    static GraphCommand makeSwap(size_t a, size_t b) noexcept {
        GraphCommand cmd;
        cmd.type = GraphCommandType::Swap;
        cmd.indexA = a;
        cmd.indexB = b;
        return cmd;
    }

    static GraphCommand makeReorder(size_t fromIdx, size_t toIdx) noexcept {
        GraphCommand cmd;
        cmd.type = GraphCommandType::Reorder;
        cmd.indexA = fromIdx;
        cmd.indexB = toIdx;
        return cmd;
    }

    static GraphCommand makeClear() noexcept {
        GraphCommand cmd;
        cmd.type = GraphCommandType::Clear;
        return cmd;
    }

    static GraphCommand makeCrossfadeTo(std::unique_ptr<NodeChain> chain) noexcept {
        GraphCommand cmd;
        cmd.type = GraphCommandType::CrossfadeTo;
        cmd.chainPayload = std::move(chain);
        return cmd;
    }
};

/**
 * @brief Master Audio Graph Engine managing the entire realtime signal path.
 * Real-time safety contract: strictly 0 heap allocations, 0 mutex locks, and 0 blocking file I/O
 * in the ASIO audio callback. Structural mutations execute at block boundaries via SPSC queue
 * with a 64-sample equal-power micro-crossfade.
 */
class GraphEngine {
public:
    static constexpr size_t GRAPH_QUEUE_CAPACITY = 512;
    static constexpr size_t MAX_GRAPH_NODES = 128;

    GraphEngine();
    ~GraphEngine();

    void prepare(double sampleRate, uint32_t maxBlockSize);
    void process(const AudioBufferView& hardwareIn, AudioBufferView& hardwareOut);
    void reset();

    // UI-thread mutations (Push to SPSC command queue, update UI shadow list)
    void addSerialNode(std::unique_ptr<AudioNode> node);
    void insertSerialNode(size_t index, std::unique_ptr<AudioNode> node);
    void removeSerialNode(size_t index);
    std::unique_ptr<AudioNode> takeSerialNode(size_t index);
    void swapNodes(size_t indexA, size_t indexB);
    void reorderNodes(size_t fromIndex, size_t toIndex);
    void splitSerialNodeIntoParallel(size_t index);
    void dissolveParallelBlock(size_t blockIndex, int branchToKeep);
    void clearNodes();
    void crossfadeToNodes(std::vector<std::unique_ptr<AudioNode>> newNodes);
    [[nodiscard]] bool isSceneCrossfading() const noexcept {
        return m_isSceneCrossfading.load(std::memory_order_acquire);
    }

    void processReclamation() noexcept;

    // UI readouts (Reads UI shadow list, zero lock contention)
    [[nodiscard]] size_t numNodes() const noexcept;
    [[nodiscard]] AudioNode* getNode(size_t index) noexcept;

    void setInputGainDb(float db) noexcept { m_inputGainDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float inputGainDb() const noexcept { return m_inputGainDb.load(std::memory_order_relaxed); }

    void setInputRouting(const InputRoutingConfig& config) noexcept { m_inputConfig = config; }
    [[nodiscard]] const InputRoutingConfig& inputRouting() const noexcept { return m_inputConfig; }

    void setMasterVolumeDb(float db) noexcept { m_masterVolumeDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float masterVolumeDb() const noexcept { return m_masterVolumeDb.load(std::memory_order_relaxed); }

    [[nodiscard]] NoiseGate& inputNoiseGate() noexcept { return m_noiseGate; }
    [[nodiscard]] LevelMeter& inputMeter() noexcept { return m_inputMeter; }
    [[nodiscard]] LevelMeter& outputMeter() noexcept { return m_outputMeter; }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] uint32_t maxBlockSize() const noexcept { return m_maxBlockSize; }

private:
    void stashForReclamation(std::unique_ptr<AudioNode> node) noexcept;
    void stashChainForReclamation(std::unique_ptr<NodeChain> chain) noexcept;
    void flushStashedReclamations() noexcept;
    void drainCommandQueueIntoActive();

    double m_sampleRate{48000.0};
    uint32_t m_maxBlockSize{256};

    std::atomic<float> m_inputGainDb{0.0f};
    std::atomic<float> m_masterVolumeDb{0.0f};
    InputRoutingConfig m_inputConfig;

    NoiseGate m_noiseGate;
    LevelMeter m_inputMeter;
    LevelMeter m_outputMeter;

    // 1. UI-Thread Shadow Registry (Only accessed from UI thread)
    std::vector<AudioNode*> m_uiNodes;

    // 2. Audio-Thread Active Nodes (Only accessed from Audio thread in process() or prepare())
    std::vector<std::unique_ptr<AudioNode>> m_activeNodes;

    // 3. SPSC Command Queue: UI Thread -> Audio Thread
    moodycamel::ReaderWriterQueue<GraphCommand> m_commandQueue{GRAPH_QUEUE_CAPACITY};

    // 4. SPSC Reclamation Queue: Audio Thread -> UI Thread
    moodycamel::ReaderWriterQueue<std::unique_ptr<AudioNode>> m_reclaimQueue{GRAPH_QUEUE_CAPACITY};
    moodycamel::ReaderWriterQueue<std::unique_ptr<NodeChain>> m_chainReclaimQueue{GRAPH_QUEUE_CAPACITY};

    // 5. Fail-Safe Audio Thread Stash (Prevents destruction on audio thread if queue is saturated)
    std::array<std::unique_ptr<AudioNode>, 16> m_stashedReclamations;
    size_t m_stashedCount{0};
    std::array<std::unique_ptr<NodeChain>, 16> m_stashedChains;
    size_t m_stashedChainCount{0};

    // UI thread mutex (Guards UI shadow modifications, NEVER locked on audio thread!)
    std::mutex m_uiMutex;

    OwnedAudioBuffer m_mainProcessingBuffer;
    OwnedAudioBuffer m_scratchBuffer;
    EqualPowerRamp m_sceneCrossfadeRamp;
    std::vector<std::unique_ptr<AudioNode>> m_retiringNodes;
    OwnedAudioBuffer m_retiringBuffer;
    OwnedAudioBuffer m_activeBuffer;

    // 64-sample equal-power micro-crossfade for structural updates
    EqualPowerRamp m_microCrossfadeRamp;
    float m_lastBlockSampleL{0.0f};
    float m_lastBlockSampleR{0.0f};
    bool m_hasProcessedAudio{false};
    std::atomic<bool> m_isSceneCrossfading{false};
};

} // namespace praccy::audio
