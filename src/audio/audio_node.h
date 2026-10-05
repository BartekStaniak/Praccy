#pragma once

#include "audio_buffer.h"
#include "dsp_utils.h"
#include <string>
#include <memory>
#include <atomic>

namespace praccy::audio {

struct AudioProcessContext {
    AudioBufferView input;
    AudioBufferView output;
    double sampleRate{48000.0};
    uint32_t numSamples{0};
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
        return m_latencySamples;
    }

protected:
    std::atomic<bool> m_bypassed{false};
    uint32_t m_latencySamples{0};
};

} // namespace praccy::audio
