#pragma once

#include "audio_buffer.h"
#include "audio_node.h"
#include "dsp_utils.h"
#include <vector>
#include <memory>
#include <string>
#include <atomic>
#include <mutex>

namespace praccy::audio {

/**
 * @brief Wrapper around a plugin node providing per-node Dry/Wet mix, Gain trim,
 * and click-free bypass crossfade transitions.
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

    [[nodiscard]] AudioNode* innerNode() const noexcept { return m_innerNode.get(); }
    [[nodiscard]] LevelMeter& meter() noexcept { return m_meter; }

private:
    std::unique_ptr<AudioNode> m_innerNode;
    std::atomic<float> m_dryWet{1.0f};      // 1.0 = 100% wet
    std::atomic<float> m_inputGainDb{0.0f};  // 0 dB
    std::atomic<float> m_outputGainDb{0.0f}; // 0 dB

    EqualPowerRamp m_bypassRamp;
    OwnedAudioBuffer m_dryBuffer;
    OwnedAudioBuffer m_processBuffer;
    LevelMeter m_meter;
};

/**
 * @brief A single parallel branch containing a serial chain of plugin slots.
 */
class ParallelBranch {
public:
    explicit ParallelBranch(std::string name);

    void prepare(double sampleRate, uint32_t maxBlockSize);
    void reset();

    void addSlot(std::unique_ptr<PluginSlot> slot);
    [[nodiscard]] size_t numSlots() const noexcept { return m_slots.size(); }
    [[nodiscard]] PluginSlot* getSlot(size_t index) noexcept;

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
    [[nodiscard]] const std::vector<std::unique_ptr<PluginSlot>>& slots() const noexcept { return m_slots; }

    void process(AudioProcessContext& ctx);

private:
    std::string m_name;
    std::vector<std::unique_ptr<PluginSlot>> m_slots;
    std::atomic<float> m_gainDb{0.0f};
    std::atomic<float> m_pan{0.0f};
    std::atomic<bool> m_muted{false};
    std::atomic<bool> m_solo{false};
    std::atomic<bool> m_phaseInvert{false};

    OwnedAudioBuffer m_branchBuffer;
    OwnedAudioBuffer m_slotTemp;
    LevelMeter m_meter;
};

/**
 * @brief Split & Merge Block: splits audio into parallel branches and mixes them.
 */
class ParallelSplitMergeBlock : public AudioNode {
public:
    explicit ParallelSplitMergeBlock(std::string name = "Parallel Split/Merge");

    void prepare(double sampleRate, uint32_t maxBlockSize) override;
    void process(AudioProcessContext& ctx) override;
    void reset() override;

    [[nodiscard]] NodeType type() const noexcept override { return NodeType::ParallelSplitMerge; }
    [[nodiscard]] const std::string& name() const noexcept override { return m_name; }

    ParallelBranch* addBranch(const std::string& branchName);
    [[nodiscard]] size_t numBranches() const noexcept { return m_branches.size(); }
    [[nodiscard]] ParallelBranch* getBranch(size_t index) noexcept;

private:
    std::string m_name;
    std::vector<std::unique_ptr<ParallelBranch>> m_branches;
    OwnedAudioBuffer m_mixBuffer;
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

/**
 * @brief Master Audio Graph Engine managing the entire realtime signal path.
 */
class GraphEngine {
public:
    GraphEngine();
    ~GraphEngine();

    void prepare(double sampleRate, uint32_t maxBlockSize);
    void process(const AudioBufferView& hardwareIn, AudioBufferView& hardwareOut);
    void reset();

    void addSerialNode(std::unique_ptr<AudioNode> node);
    void clearNodes();

    [[nodiscard]] size_t numNodes() const noexcept;
    [[nodiscard]] AudioNode* getNode(size_t index) noexcept;

    void setInputGainDb(float db) noexcept { m_inputGainDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float inputGainDb() const noexcept { return m_inputGainDb.load(std::memory_order_relaxed); }

    void setMasterVolumeDb(float db) noexcept { m_masterVolumeDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float masterVolumeDb() const noexcept { return m_masterVolumeDb.load(std::memory_order_relaxed); }

    [[nodiscard]] NoiseGate& inputNoiseGate() noexcept { return m_noiseGate; }
    [[nodiscard]] LevelMeter& inputMeter() noexcept { return m_inputMeter; }
    [[nodiscard]] LevelMeter& outputMeter() noexcept { return m_outputMeter; }

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] uint32_t maxBlockSize() const noexcept { return m_maxBlockSize; }

private:
    double m_sampleRate{48000.0};
    uint32_t m_maxBlockSize{256};

    std::atomic<float> m_inputGainDb{0.0f};
    std::atomic<float> m_masterVolumeDb{0.0f};

    NoiseGate m_noiseGate;
    LevelMeter m_inputMeter;
    LevelMeter m_outputMeter;

    std::vector<std::unique_ptr<AudioNode>> m_nodes;
    std::mutex m_graphMutex; // For topology modifications from UI thread

    OwnedAudioBuffer m_mainProcessingBuffer;
    OwnedAudioBuffer m_scratchBuffer;
};

} // namespace praccy::audio
