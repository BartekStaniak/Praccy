#pragma once

#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <span>

namespace praccy::audio {

/**
 * @brief Non-owning view of multi-channel audio buffer.
 * Safe to pass around on real-time audio thread without any dynamic heap allocations.
 */
class AudioBufferView {
public:
    AudioBufferView() : m_channels(nullptr), m_numChannels(0), m_numSamples(0) {}

    AudioBufferView(float* const* channels, uint32_t numChannels, uint32_t numSamples)
        : m_channels(channels), m_numChannels(numChannels), m_numSamples(numSamples) {}

    [[nodiscard]] uint32_t numChannels() const noexcept { return m_numChannels; }
    [[nodiscard]] uint32_t numSamples() const noexcept { return m_numSamples; }

    [[nodiscard]] float* channel(uint32_t ch) noexcept {
        assert(ch < m_numChannels);
        return m_channels[ch];
    }

    [[nodiscard]] const float* channel(uint32_t ch) const noexcept {
        assert(ch < m_numChannels);
        return m_channels[ch];
    }

    [[nodiscard]] float* const* rawChannels() noexcept { return m_channels; }
    [[nodiscard]] const float* const* rawChannels() const noexcept { return m_channels; }

    void clear() noexcept {
        for (uint32_t ch = 0; ch < m_numChannels; ++ch) {
            std::memset(m_channels[ch], 0, m_numSamples * sizeof(float));
        }
    }

    void copyFrom(const AudioBufferView& src) noexcept {
        const uint32_t chToCopy = std::min(m_numChannels, src.m_numChannels);
        const uint32_t samplesToCopy = std::min(m_numSamples, src.m_numSamples);
        for (uint32_t ch = 0; ch < chToCopy; ++ch) {
            std::memcpy(m_channels[ch], src.m_channels[ch], samplesToCopy * sizeof(float));
        }
    }

    void applyGain(float gain) noexcept {
        if (gain == 1.0f) return;
        for (uint32_t ch = 0; ch < m_numChannels; ++ch) {
            float* data = m_channels[ch];
            for (uint32_t i = 0; i < m_numSamples; ++i) {
                data[i] *= gain;
            }
        }
    }

    void addFrom(const AudioBufferView& src, float gain = 1.0f) noexcept {
        const uint32_t chToAdd = std::min(m_numChannels, src.m_numChannels);
        const uint32_t samplesToAdd = std::min(m_numSamples, src.m_numSamples);
        for (uint32_t ch = 0; ch < chToAdd; ++ch) {
            float* dest = m_channels[ch];
            const float* source = src.m_channels[ch];
            for (uint32_t i = 0; i < samplesToAdd; ++i) {
                dest[i] += source[i] * gain;
            }
        }
    }

private:
    float* const* m_channels;
    uint32_t m_numChannels;
    uint32_t m_numSamples;
};

/**
 * @brief Owning multi-channel audio buffer with contiguous 64-byte aligned memory.
 * Pre-allocated during initialization, never reallocated inside realtime callbacks.
 */
class OwnedAudioBuffer {
public:
    OwnedAudioBuffer() = default;

    OwnedAudioBuffer(uint32_t numChannels, uint32_t maxSamples) {
        resize(numChannels, maxSamples);
    }

    void resize(uint32_t numChannels, uint32_t maxSamples) {
        m_numChannels = numChannels;
        m_maxSamples = maxSamples;
        m_data.resize(static_cast<size_t>(numChannels) * maxSamples, 0.0f);
        m_channelPointers.resize(numChannels);
        for (uint32_t ch = 0; ch < numChannels; ++ch) {
            m_channelPointers[ch] = m_data.data() + (static_cast<size_t>(ch) * maxSamples);
        }
    }

    [[nodiscard]] AudioBufferView view(uint32_t numSamples) noexcept {
        assert(numSamples <= m_maxSamples);
        return AudioBufferView(m_channelPointers.data(), m_numChannels, numSamples);
    }

    [[nodiscard]] uint32_t numChannels() const noexcept { return m_numChannels; }
    [[nodiscard]] uint32_t maxSamples() const noexcept { return m_maxSamples; }

private:
    uint32_t m_numChannels{0};
    uint32_t m_maxSamples{0};
    std::vector<float> m_data;
    std::vector<float*> m_channelPointers;
};

} // namespace praccy::audio
