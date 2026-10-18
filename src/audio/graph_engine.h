#pragma once

#include "audio_buffer.h"
#include "audio_node.h"
#include "dsp_utils.h"
#include "input_config.h"
#include "param_automation.h"
#include "../midi/midi_types.h"
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <mutex>
#include <array>
#include "readerwriterqueue/readerwriterqueue.h"

namespace praccy::plugins {
class IPluginInstance;
}

namespace praccy::audio {

/**
 * @brief Wrapper around a plugin node providing per-node Dry/Wet mix, Gain trim,
 * wait-free parameter automation, and click-free bypass crossfade transitions.
 */
class PluginSlot : public AudioNode {
public:
    static constexpr size_t MAX_CACHED_PARAM_RANGES = 256;

    struct CachedParamRange {
        uint32_t id{0};
        float minVal{0.0f};
        float maxVal{1.0f};
    };

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
        AudioNode* inner = m_rtInnerNode.load(std::memory_order_relaxed);
        return inner ? inner->isFaulted() : false;
    }
    [[nodiscard]] bool isMissing() const noexcept override {
        AudioNode* inner = m_rtInnerNode.load(std::memory_order_relaxed);
        return inner ? inner->isMissing() : false;
    }
    [[nodiscard]] bool isLoading() const noexcept override {
        AudioNode* inner = m_rtInnerNode.load(std::memory_order_relaxed);
        return inner ? inner->isLoading() : false;
    }

    void replaceInnerNode(std::unique_ptr<AudioNode> newInner, std::unique_ptr<AudioNode>& outOld) noexcept;

    [[nodiscard]] uint32_t latencySamples() const noexcept override {
        AudioNode* inner = m_rtInnerNode.load(std::memory_order_relaxed);
        if (isBypassed() || !inner || inner->isFaulted() || inner->isBypassed()) {
            return 0;
        }
        return inner->latencySamples();
    }

    // Parameter automation registration (Wait-free, Lock-free)
    void setParameter(uint32_t paramId, float value) noexcept {
        m_automationRegister.setParameter(paramId, value);
    }
    void setParameterNormalized(uint32_t paramId, float normalizedValue) noexcept;
    void refreshCachedParameterRanges() noexcept;

    [[nodiscard]] float getParameter(uint32_t paramId) const noexcept {
        return m_automationRegister.getParameter(paramId);
    }
    [[nodiscard]] ParamAutomationRegister& automationRegister() noexcept {
        return m_automationRegister;
    }
    [[nodiscard]] const ParamAutomationRegister& automationRegister() const noexcept {
        return m_automationRegister;
    }

    [[nodiscard]] AudioNode* innerNode() const noexcept {
        AudioNode* node = m_rtInnerNode.load(std::memory_order_relaxed);
        return node ? node : m_innerNode.get();
    }
    [[nodiscard]] LevelMeter& meter() noexcept { return m_meter; }

    struct Macro {
        std::string label;
        uint32_t paramId{0};
        bool bound{false};
        bool isSlotMix{false};
        bool isSlotTrim{false};
        float currentValue{0.0f};
        float defaultValue{0.0f};
        float minValue{0.0f};
        float maxValue{1.0f};
    };

    [[nodiscard]] std::array<Macro, 4>& macros() noexcept { return m_macros; }
    [[nodiscard]] const std::array<Macro, 4>& macros() const noexcept { return m_macros; }
    [[nodiscard]] bool hasMacrosInitialized() const noexcept { return m_macrosInitialized; }
    void setMacrosInitialized(bool init) noexcept { m_macrosInitialized = init; }

private:
    std::unique_ptr<AudioNode> m_innerNode;
    std::atomic<AudioNode*> m_rtInnerNode{nullptr};
    std::atomic<plugins::IPluginInstance*> m_rtPluginPtr{nullptr};
    plugins::IPluginInstance* m_pluginPtr{nullptr};
    std::atomic<float> m_dryWet{1.0f};      // 1.0 = 100% wet
    std::atomic<float> m_inputGainDb{0.0f};  // 0 dB
    std::atomic<float> m_outputGainDb{0.0f}; // 0 dB

    std::array<CachedParamRange, MAX_CACHED_PARAM_RANGES> m_paramRanges{};
    std::atomic<size_t> m_numParamRanges{0};

    ParamAutomationRegister m_automationRegister;
    EqualPowerRamp m_bypassRamp;
    OwnedAudioBuffer m_dryBuffer;
    OwnedAudioBuffer m_processBuffer;
    OwnedAudioBuffer m_inputScaledBuffer;
    LevelMeter m_meter;
    std::array<Macro, 4> m_macros{};
    bool m_macrosInitialized{false};
};

enum class SlotCommandType : uint8_t {
    Add,
    Insert,
    Remove,
    Reorder,
    Clear
};

/**
 * @brief Lock-free command payload dispatched from UI thread to audio thread for parallel branches.
 */
struct SlotCommand {
    SlotCommandType type{SlotCommandType::Add};
    std::unique_ptr<PluginSlot> slot{nullptr};
    size_t index{0};
    size_t targetIndex{0};
    PluginSlot* targetSlot{nullptr};

    static SlotCommand makeAdd(std::unique_ptr<PluginSlot> newSlot) noexcept {
        SlotCommand cmd;
        cmd.type = SlotCommandType::Add;
        cmd.slot = std::move(newSlot);
        return cmd;
    }

    static SlotCommand makeInsert(size_t idx, std::unique_ptr<PluginSlot> newSlot) noexcept {
        SlotCommand cmd;
        cmd.type = SlotCommandType::Insert;
        cmd.index = idx;
        cmd.slot = std::move(newSlot);
        return cmd;
    }

    static SlotCommand makeReorder(size_t fromIdx, size_t toIdx) noexcept {
        SlotCommand cmd;
        cmd.type = SlotCommandType::Reorder;
        cmd.index = fromIdx;
        cmd.targetIndex = toIdx;
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
    static constexpr size_t DEFERRED_RECLAIM_CAPACITY = 2048;

    explicit ParallelBranch(std::string name);
    ~ParallelBranch();

    void prepare(double sampleRate, uint32_t maxBlockSize);
    void reset();
    void setAudioRunning(bool running) noexcept { m_isAudioRunning.store(running, std::memory_order_release); }

    // UI-thread mutations (Push to SPSC command queue, update UI shadow list)
    void addSlot(std::unique_ptr<PluginSlot> slot);
    void insertSlot(size_t index, std::unique_ptr<PluginSlot> slot);
    void removeSlot(size_t index);
    std::unique_ptr<PluginSlot> takeSlot(size_t index);
    void reorderSlots(size_t fromIndex, size_t toIndex);

    // Thread-safe UI readouts
    [[nodiscard]] size_t numSlots() const noexcept;
    [[nodiscard]] PluginSlot* getSlot(size_t index) noexcept;
    [[nodiscard]] PluginSlot* getSlotLockFree(size_t index) const noexcept {
        const size_t count = m_rtNumSlots.load(std::memory_order_acquire);
        if (index >= count || index >= MAX_BRANCH_SLOTS) return nullptr;
        return m_rtSlots[index].load(std::memory_order_acquire);
    }
    void addSlotToUiShadow(PluginSlot* slot);
    [[nodiscard]] std::vector<std::unique_ptr<PluginSlot>>& activeSlots() noexcept { return m_activeSlots; }
    void drainSlotCommandsIntoActive() noexcept;

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
    void syncRtSlotsLocked() noexcept;

    std::string m_name;
    mutable std::mutex m_slotMutex;

    // 1. UI-Thread Shadow Registry (guarded by m_slotMutex)
    std::vector<PluginSlot*> m_uiSlots;
    std::array<std::atomic<PluginSlot*>, MAX_BRANCH_SLOTS> m_rtSlots{};
    std::atomic<size_t> m_rtNumSlots{0};

    // 2. Audio-Thread Active Slots (Only accessed from Audio thread in process())
    std::vector<std::unique_ptr<PluginSlot>> m_activeSlots;

    // 3. SPSC Command Queue: UI Thread -> Audio Thread
    moodycamel::ReaderWriterQueue<SlotCommand> m_commandQueue{QUEUE_CAPACITY};

    // 4. SPSC Reclamation Queue: Audio Thread -> UI Thread (1024 capacity for bounded reclamation)
    moodycamel::ReaderWriterQueue<std::unique_ptr<PluginSlot>> m_reclaimQueue{1024};

    // 5. Fail-Safe Audio Thread Stash (Prevents destruction on audio thread if queue is saturated)
    std::vector<std::unique_ptr<PluginSlot>> m_deferredReclaim;
    std::atomic<bool> m_isAudioRunning{false};

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

enum class BranchCommandType : uint8_t {
    Add,
    Remove,
    Clear
};

/**
 * @brief Lock-free command payload dispatched from UI thread to ParallelSplitMergeBlock audio thread.
 */
struct BranchCommand {
    BranchCommandType type{BranchCommandType::Add};
    std::unique_ptr<ParallelBranch> branch{nullptr};
    size_t index{0};
    ParallelBranch* targetBranch{nullptr};

    static BranchCommand makeAdd(std::unique_ptr<ParallelBranch> b) noexcept {
        BranchCommand cmd;
        cmd.type = BranchCommandType::Add;
        cmd.branch = std::move(b);
        return cmd;
    }

    static BranchCommand makeRemove(size_t idx, ParallelBranch* target = nullptr) noexcept {
        BranchCommand cmd;
        cmd.type = BranchCommandType::Remove;
        cmd.index = idx;
        cmd.targetBranch = target;
        return cmd;
    }

    static BranchCommand makeClear() noexcept {
        BranchCommand cmd;
        cmd.type = BranchCommandType::Clear;
        return cmd;
    }
};

/**
 * @brief Split & Merge Block: splits audio into parallel branches and mixes them,
 * coordinating dynamic phase-coherent Parallel Delay Compensation.
 * Concurrency-safe: UI mutations use lock-free SPSC branch command and reclamation queues.
 */
class ParallelSplitMergeBlock : public AudioNode {
public:
    static constexpr size_t BRANCH_QUEUE_CAPACITY = 64;
    static constexpr size_t MAX_BRANCHES = 16;
    static constexpr size_t DEFERRED_BRANCH_RECLAIM_CAPACITY = 256;

    explicit ParallelSplitMergeBlock(std::string name = "Parallel Split/Merge");
    ~ParallelSplitMergeBlock() override;

    void prepare(double sampleRate, uint32_t maxBlockSize) override;
    void process(AudioProcessContext& ctx) override;
    void reset() override;
    void setAudioRunning(bool running) noexcept;

    [[nodiscard]] NodeType type() const noexcept override { return NodeType::ParallelSplitMerge; }
    [[nodiscard]] const std::string& name() const noexcept override { return m_name; }

    [[nodiscard]] uint32_t latencySamples() const noexcept override;

    ParallelBranch* addBranch(const std::string& branchName);
    void removeBranch(size_t index);
    [[nodiscard]] size_t numBranches() const noexcept;
    [[nodiscard]] ParallelBranch* getBranch(size_t index) noexcept;
    [[nodiscard]] ParallelBranch* getBranchLockFree(size_t index) const noexcept {
        const size_t count = m_rtNumBranches.load(std::memory_order_acquire);
        if (index >= count || index >= MAX_BRANCHES) return nullptr;
        return m_rtBranches[index].load(std::memory_order_acquire);
    }

    // Audio-thread lock-free branch access (must ONLY be called on the audio thread or when audio is stopped)
    void flushBranchCommandsIntoActive() noexcept { drainCommandQueueIntoActive(); }
    [[nodiscard]] std::vector<std::unique_ptr<ParallelBranch>>& activeBranches() noexcept { return m_activeBranches; }
    [[nodiscard]] const std::vector<std::unique_ptr<ParallelBranch>>& activeBranches() const noexcept { return m_activeBranches; }

    void setBlend(float blend) noexcept { m_blend.store(std::clamp(blend, -1.0f, 1.0f), std::memory_order_relaxed); }
    [[nodiscard]] float blend() const noexcept { return m_blend.load(std::memory_order_relaxed); }

    void collectReclaimedSlots() noexcept;
    void collectReclaimedBranches() noexcept;

private:
    void drainCommandQueueIntoActive();
    void stashBranchForReclamation(std::unique_ptr<ParallelBranch> branch) noexcept;
    void syncRtBranchesLocked() noexcept;

    std::string m_name;
    mutable std::mutex m_branchMutex;

    // 1. UI-Thread Branch Shadow Registry (guarded by m_branchMutex)
    std::vector<ParallelBranch*> m_uiBranches;
    std::array<std::atomic<ParallelBranch*>, MAX_BRANCHES> m_rtBranches{};
    std::atomic<size_t> m_rtNumBranches{0};

    // 2. Audio-Thread Active Branches (owned exclusively by Audio thread in process())
    std::vector<std::unique_ptr<ParallelBranch>> m_activeBranches;

    // 3. SPSC Command Queue: UI Thread -> Audio Thread
    moodycamel::ReaderWriterQueue<BranchCommand> m_branchCmdQueue{BRANCH_QUEUE_CAPACITY};

    // 4. SPSC Reclamation Queue: Audio Thread -> UI Thread
    moodycamel::ReaderWriterQueue<std::unique_ptr<ParallelBranch>> m_branchReclaimQueue{BRANCH_QUEUE_CAPACITY};

    // 5. Overflow stash for branches
    std::vector<std::unique_ptr<ParallelBranch>> m_deferredReclaimBranches;

    OwnedAudioBuffer m_mixBuffer;
    std::atomic<float> m_blend{0.0f};
    std::atomic<bool> m_isAudioRunning{false};
    std::atomic<uint32_t> m_blockLatency{0};
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
    CrossfadeTo,
    SplitToParallel,
    DissolveParallel
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

    static GraphCommand makeSplitToParallel(size_t index, std::unique_ptr<AudioNode> block) noexcept {
        GraphCommand cmd;
        cmd.type = GraphCommandType::SplitToParallel;
        cmd.indexA = index;
        cmd.node = std::move(block);
        return cmd;
    }

    static GraphCommand makeDissolveParallel(size_t blockIndex, int branchToKeep) noexcept {
        GraphCommand cmd;
        cmd.type = GraphCommandType::DissolveParallel;
        cmd.indexA = blockIndex;
        cmd.indexB = static_cast<size_t>(branchToKeep >= 0 ? branchToKeep : 0);
        return cmd;
    }
};

/**
 * @brief Master Audio Graph Engine managing the entire realtime signal path.
 * Real-time safety contract: strictly 0 heap allocations, 0 mutex locks, and 0 blocking file I/O
 * in the ASIO audio callback and WinMM MIDI callback. Structural mutations execute at block
 * boundaries via SPSC queue with a 64-sample equal-power micro-crossfade.
 */
class GraphEngine {
public:
    static constexpr size_t GRAPH_QUEUE_CAPACITY = 512;
    static constexpr size_t MAX_GRAPH_NODES = 128;
    static constexpr size_t DEFERRED_RECLAIM_CAPACITY = 4096;

    GraphEngine();
    ~GraphEngine();

    void prepare(double sampleRate, uint32_t maxBlockSize);
    void process(const AudioBufferView& hardwareIn, AudioBufferView& hardwareOut);
    void process(const AudioBufferView& hardwareIn, AudioBufferView& hardwareOut, const AudioProcessContext& transportCtx);
    void reset();
    void setAudioRunning(bool running) noexcept;

    template <typename Fn>
    void executeWithAudioFence(Fn&& fn) {
        const bool wasRunning = m_isAudioRunning.load(std::memory_order_acquire);
        if (wasRunning) {
            setAudioRunning(false);
        }
        std::forward<Fn>(fn)();
        if (wasRunning) {
            setAudioRunning(true);
        }
    }

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
    void triggerCrossfade(float durationMs = 10.0f) noexcept {
        const uint32_t rampSamples = static_cast<uint32_t>(m_sampleRate * (durationMs / 1000.0f));
        m_pendingCrossfadeSamples.store(std::max(1u, rampSamples), std::memory_order_release);
    }
    [[nodiscard]] bool isSceneCrossfading() const noexcept {
        return m_isSceneCrossfading.load(std::memory_order_acquire);
    }

    void processReclamation() noexcept;

    // UI readouts
    [[nodiscard]] size_t numNodes() const noexcept;
    [[nodiscard]] AudioNode* getNode(size_t index) noexcept;

    // Wait-free, lock-free parameter and binding dispatch (0 mutex locks on MIDI/Audio threads)
    bool dispatchParameterThreadSafe(int slotIdx, int branchIdx, int branchSlotIdx,
                                     uint32_t paramId, float normalizedValue) noexcept;
    bool dispatchBindingThreadSafe(praccy::midi::BindingTargetType type,
                                   int slotIdx, int branchIdx, int branchSlotIdx,
                                   uint32_t paramId, float value) noexcept;

    void setInputGainDb(float db) noexcept { m_inputGainDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float inputGainDb() const noexcept { return m_inputGainDb.load(std::memory_order_relaxed); }

    void setInputRouting(const InputRoutingConfig& config) noexcept {
        const uint64_t packed =
            (static_cast<uint64_t>(static_cast<uint8_t>(config.mode)) & 0xFFull) |
            ((static_cast<uint64_t>(static_cast<uint16_t>(std::max(0, config.channelLeft))) & 0xFFFFull) << 8) |
            ((static_cast<uint64_t>(static_cast<uint16_t>(std::max(0, config.channelRight))) & 0xFFFFull) << 24);
        m_packedInputConfig.store(packed, std::memory_order_release);
    }
    [[nodiscard]] InputRoutingConfig inputRouting() const noexcept {
        const uint64_t packed = m_packedInputConfig.load(std::memory_order_acquire);
        InputRoutingConfig cfg;
        cfg.mode = static_cast<InputRoutingMode>(packed & 0xFFull);
        cfg.channelLeft = static_cast<int>((packed >> 8) & 0xFFFFull);
        cfg.channelRight = static_cast<int>((packed >> 24) & 0xFFFFull);
        return cfg;
    }

    void setMasterVolumeDb(float db) noexcept { m_masterVolumeDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float masterVolumeDb() const noexcept { return m_masterVolumeDb.load(std::memory_order_relaxed); }

    [[nodiscard]] NoiseGate& inputNoiseGate() noexcept { return m_noiseGate; }
    [[nodiscard]] LevelMeter& inputMeter() noexcept { return m_inputMeter; }
    [[nodiscard]] LevelMeter& outputMeter() noexcept { return m_outputMeter; }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] uint32_t maxBlockSize() const noexcept { return m_maxBlockSize; }

    void stashForReclamation(std::unique_ptr<AudioNode> node) noexcept;

private:
    void stashChainForReclamation(std::unique_ptr<NodeChain> chain) noexcept;
    void flushStashedReclamations() noexcept;
    void drainCommandQueueIntoActive();
    void syncRtNodesLocked() noexcept;

    double m_sampleRate{48000.0};
    uint32_t m_maxBlockSize{256};

    std::atomic<float> m_inputGainDb{0.0f};
    std::atomic<float> m_masterVolumeDb{0.0f};
    std::atomic<uint64_t> m_packedInputConfig{
        (static_cast<uint64_t>(static_cast<uint8_t>(InputRoutingMode::MonoLeft)) & 0xFFull) |
        (0ull << 8) |
        (1ull << 24)
    };

    NoiseGate m_noiseGate;
    LevelMeter m_inputMeter;
    LevelMeter m_outputMeter;

    // 1. UI-Thread Shadow Registry & Wait-Free Atomic Topology Mirror
    std::vector<AudioNode*> m_uiNodes;
    std::array<std::atomic<AudioNode*>, MAX_GRAPH_NODES> m_rtNodes{};
    std::atomic<size_t> m_rtNumNodes{0};

    // 2. Audio-Thread Active Nodes (Only accessed from Audio thread in process() or prepare())
    std::vector<std::unique_ptr<AudioNode>> m_activeNodes;

    // 3. SPSC Command Queue: UI Thread -> Audio Thread
    moodycamel::ReaderWriterQueue<GraphCommand> m_commandQueue{GRAPH_QUEUE_CAPACITY};

    // 4. SPSC Reclamation Queue: Audio Thread -> UI Thread (1024 capacity for bounded reclamation)
    moodycamel::ReaderWriterQueue<std::unique_ptr<AudioNode>> m_reclaimQueue{1024};
    moodycamel::ReaderWriterQueue<std::unique_ptr<NodeChain>> m_chainReclaimQueue{1024};

    // 5. Fail-Safe Audio Thread Stash (Prevents destruction on audio thread if queue is saturated)
    std::vector<std::unique_ptr<AudioNode>> m_deferredReclaim;
    std::array<std::unique_ptr<NodeChain>, 16> m_stashedChains;
    size_t m_stashedChainCount{0};

    // UI thread mutex (Guards UI shadow modifications, NEVER locked on audio or MIDI threads!)
    mutable std::mutex m_uiMutex;
    std::atomic<bool> m_isAudioRunning{false};
    std::atomic<bool> m_cancelSceneCrossfade{false};

    OwnedAudioBuffer m_mainProcessingBuffer;
    OwnedAudioBuffer m_scratchBuffer;
    EqualPowerRamp m_sceneCrossfadeRamp;
    std::vector<std::unique_ptr<AudioNode>> m_fadingOutNodes;
    OwnedAudioBuffer m_retiringBuffer;
    OwnedAudioBuffer m_activeBuffer;
    uint32_t m_crossfadeRemainingSamples{0};

    // 64-sample equal-power micro-crossfade for structural updates
    EqualPowerRamp m_microCrossfadeRamp;
    float m_lastBlockSampleL{0.0f};
    float m_lastBlockSampleR{0.0f};
    bool m_hasProcessedAudio{false};
    std::atomic<bool> m_isSceneCrossfading{false};

    // 10ms smooth equal-power crossfade for in-place preset / snapshot / A-B switching
    std::atomic<uint32_t> m_pendingCrossfadeSamples{0};
    EqualPowerRamp m_inplaceCrossfadeRamp;
    OwnedAudioBuffer m_outgoingTailBuffer;
};

} // namespace praccy::audio
