#pragma once

#include "audio_buffer.h"
#include "dsp_utils.h"
#include <string>
#include <memory>
#include <atomic>
#include <cstdint>

namespace praccy::audio {

struct AudioProcessContext {
    AudioBufferView input;
    AudioBufferView output;
    double sampleRate{48000.0};
    uint32_t numSamples{0};
    double tempoBpm{120.0};
    int32_t timeSigNumerator{4};
    int32_t timeSigDenominator{4};
    int64_t projectTimeSamples{0};
};

enum class NodeType {
    Plugin,
    ParallelSplitMerge,
    Utility
};

class AudioNode {
public:
    virtual ~AudioNode() = default;

    virtual void prepare(double sampleRate, uint32_t maxBlockSize) = 0;
    virtual void process(AudioProcessContext& ctx) = 0;
    virtual void reset() = 0;

    [[nodiscard]] virtual NodeType type() const noexcept = 0;
    [[nodiscard]] virtual const std::string& name() const noexcept = 0;

    virtual void setBypassed(bool bypassed) noexcept {
        m_bypassed.store(bypassed, std::memory_order_relaxed);
    }

    [[nodiscard]] virtual bool isBypassed() const noexcept {
        return m_bypassed.load(std::memory_order_relaxed);
    }

    [[nodiscard]] virtual uint32_t latencySamples() const noexcept {
        return m_latencySamples.load(std::memory_order_relaxed);
    }

    virtual void setLatencySamples(uint32_t samples) noexcept {
        m_latencySamples.store(samples, std::memory_order_relaxed);
    }

    [[nodiscard]] virtual bool isFaulted() const noexcept {
        return false;
    }

    [[nodiscard]] virtual bool isMissing() const noexcept {
        return false;
    }

protected:
    std::atomic<bool> m_bypassed{false};
    std::atomic<uint32_t> m_latencySamples{0};
};

class MissingPluginPlaceholder : public AudioNode {
public:
    MissingPluginPlaceholder(
        std::string missingPluginName,
        std::string expectedFormat = "VST3",
        std::string classGuid = "",
        std::string clapId = ""
    ) : m_missingPluginName(std::move(missingPluginName)),
        m_expectedFormat(std::move(expectedFormat)),
        m_classGuid(std::move(classGuid)),
        m_clapId(std::move(clapId)) {}

    void prepare(double /*sampleRate*/, uint32_t /*maxBlockSize*/) override {}

    void process(AudioProcessContext& ctx) override {
        // Invariant 4: Bit-exact dry pass-through
        ctx.output.copyFrom(ctx.input);
    }

    void reset() override {}

    [[nodiscard]] NodeType type() const noexcept override { return NodeType::Plugin; }
    [[nodiscard]] const std::string& name() const noexcept override { return m_missingPluginName; }
    [[nodiscard]] bool isMissing() const noexcept override { return true; }

    [[nodiscard]] const std::string& expectedFormat() const noexcept { return m_expectedFormat; }
    [[nodiscard]] const std::string& classGuid() const noexcept { return m_classGuid; }
    [[nodiscard]] const std::string& clapId() const noexcept { return m_clapId; }

private:
    std::string m_missingPluginName;
    std::string m_expectedFormat;
    std::string m_classGuid;
    std::string m_clapId;
};

} // namespace praccy::audio
