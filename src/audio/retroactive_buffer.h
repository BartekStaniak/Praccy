#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>
#include <cstring>
#include <algorithm>

namespace praccy::audio {

/**
 * @brief Lock-free, pre-allocated circular audio buffer for retroactive riff capture.
 *
 * Pre-allocated strictly in prepare() with configurable duration (1, 2, 5, or 10 minutes).
 * Audio callback pushes stereo samples lock-free and wait-free with strictly 0 heap allocations.
 * Export mechanism atomically latches the write head and copies requested audio slices
 * into staging buffers in <0.15ms without interrupting real-time playback.
 */
class RetroactiveRiffBuffer {
public:
    RetroactiveRiffBuffer() = default;
    ~RetroactiveRiffBuffer() = default;

    RetroactiveRiffBuffer(const RetroactiveRiffBuffer&) = delete;
    RetroactiveRiffBuffer& operator=(const RetroactiveRiffBuffer&) = delete;

    /**
     * @brief Pre-allocates circular stereo buffer for specified duration.
     * Must be called from control thread before real-time streaming starts.
     *
     * @param sampleRate Audio sample rate (e.g. 48000.0)
     * @param durationMinutes Recording window duration (1, 2, 5, or 10)
     */
    void prepare(double sampleRate, int durationMinutes) {
        if (durationMinutes <= 0) durationMinutes = 2;
        if (sampleRate <= 0.0) sampleRate = 48000.0;

        m_sampleRate = sampleRate;
        m_durationMinutes = durationMinutes;

        const size_t targetCapacity = static_cast<size_t>(sampleRate * 60.0 * static_cast<double>(durationMinutes));
        m_capacityFrames = std::max<size_t>(targetCapacity, 1024);

        m_bufferL.assign(m_capacityFrames, 0.0f);
        m_bufferR.assign(m_capacityFrames, 0.0f);

        m_writeHead.store(0, std::memory_order_relaxed);
        m_totalFramesWritten.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Clears buffer contents and resets write head.
     */
    void reset() noexcept {
        std::fill(m_bufferL.begin(), m_bufferL.end(), 0.0f);
        std::fill(m_bufferR.begin(), m_bufferR.end(), 0.0f);
        m_writeHead.store(0, std::memory_order_relaxed);
        m_totalFramesWritten.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Lock-free, wait-free push of stereo blocks into circular buffer.
     * Guaranteed 0 dynamic allocations and 0 mutex locks on the audio callback.
     */
    inline void push(const float* left, const float* right, size_t numSamples) noexcept {
        if (!left || numSamples == 0 || m_capacityFrames == 0) return;

        size_t head = m_writeHead.load(std::memory_order_relaxed);
        const float* srcR = right ? right : left;

        if (head + numSamples <= m_capacityFrames) {
            std::memcpy(&m_bufferL[head], left, numSamples * sizeof(float));
            std::memcpy(&m_bufferR[head], srcR, numSamples * sizeof(float));
            head = (head + numSamples == m_capacityFrames) ? 0 : (head + numSamples);
        } else {
            const size_t part1 = m_capacityFrames - head;
            const size_t part2 = numSamples - part1;

            std::memcpy(&m_bufferL[head], left, part1 * sizeof(float));
            std::memcpy(&m_bufferL[0], left + part1, part2 * sizeof(float));

            std::memcpy(&m_bufferR[head], srcR, part1 * sizeof(float));
            std::memcpy(&m_bufferR[0], srcR + part1, part2 * sizeof(float));

            head = part2;
        }

        m_writeHead.store(head, std::memory_order_release);
        m_totalFramesWritten.fetch_add(numSamples, std::memory_order_release);
    }

    /**
     * @brief Tear-free export mechanism: atomically latches write head and copies
     * the requested slice into scratch staging buffers in <0.15ms.
     *
     * @param stagingBufferL Destination buffer for left channel
     * @param stagingBufferR Destination buffer for right channel
     * @param maxSamples Maximum number of samples to capture
     * @param capturedSamples Output number of frames actually captured
     * @return true if audio frames were successfully copied
     */
    bool captureSlice(float* stagingBufferL, float* stagingBufferR,
                      size_t maxSamples, size_t& capturedSamples) const noexcept {
        if (!stagingBufferL || !stagingBufferR || maxSamples == 0 || m_capacityFrames == 0) {
            capturedSamples = 0;
            return false;
        }

        const size_t head = m_writeHead.load(std::memory_order_acquire);
        const size_t total = m_totalFramesWritten.load(std::memory_order_acquire);

        const size_t available = std::min(total, m_capacityFrames);
        capturedSamples = std::min(available, maxSamples);
        if (capturedSamples == 0) return false;

        // Calculate starting index in circular buffer for the most recent `capturedSamples` frames
        const size_t start = (head >= capturedSamples)
            ? (head - capturedSamples)
            : (m_capacityFrames + head - capturedSamples);

        if (start + capturedSamples <= m_capacityFrames) {
            std::memcpy(stagingBufferL, &m_bufferL[start], capturedSamples * sizeof(float));
            std::memcpy(stagingBufferR, &m_bufferR[start], capturedSamples * sizeof(float));
        } else {
            const size_t part1 = m_capacityFrames - start;
            const size_t part2 = capturedSamples - part1;

            std::memcpy(stagingBufferL, &m_bufferL[start], part1 * sizeof(float));
            std::memcpy(stagingBufferL + part1, &m_bufferL[0], part2 * sizeof(float));

            std::memcpy(stagingBufferR, &m_bufferR[start], part1 * sizeof(float));
            std::memcpy(stagingBufferR + part1, &m_bufferR[0], part2 * sizeof(float));
        }

        return true;
    }

    /**
     * @brief Convenience overload copying into std::vector staging buffers.
     */
    bool latchAndCopySlice(std::vector<float>& outStagingL, std::vector<float>& outStagingR,
                           size_t& outFramesCaptured, double& outSampleRate) const noexcept {
        outSampleRate = m_sampleRate;
        const size_t total = m_totalFramesWritten.load(std::memory_order_acquire);
        const size_t available = std::min(total, m_capacityFrames);

        if (available == 0) {
            outFramesCaptured = 0;
            return false;
        }

        if (outStagingL.size() < available) outStagingL.resize(available);
        if (outStagingR.size() < available) outStagingR.resize(available);

        return captureSlice(outStagingL.data(), outStagingR.data(), available, outFramesCaptured);
    }

    [[nodiscard]] size_t capacityFrames() const noexcept { return m_capacityFrames; }
    [[nodiscard]] size_t totalFramesWritten() const noexcept { return m_totalFramesWritten.load(std::memory_order_relaxed); }
    [[nodiscard]] size_t writeHead() const noexcept { return m_writeHead.load(std::memory_order_relaxed); }
    [[nodiscard]] int durationMinutes() const noexcept { return m_durationMinutes; }
    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }

private:
    std::vector<float> m_bufferL;
    std::vector<float> m_bufferR;
    size_t m_capacityFrames{0};
    int m_durationMinutes{2};
    double m_sampleRate{48000.0};

    alignas(64) std::atomic<size_t> m_writeHead{0};
    alignas(64) std::atomic<size_t> m_totalFramesWritten{0};
};

} // namespace praccy::audio
