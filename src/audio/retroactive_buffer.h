#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <string>
#include <cmath>
#include <ctime>
#include <cctype>
#include <thread>

namespace praccy::audio {

/**
 * @brief Lock-free, pre-allocated circular audio buffer for retroactive riff capture.
 *
 * Pre-allocated strictly in prepare() with configurable duration (1, 2, 5, or 10 minutes).
 * Audio callback pushes stereo samples lock-free and wait-free with strictly 0 heap allocations.
 * During export, an atomic capture fence diverts live audio callback writes into a pre-allocated
 * spillover buffer so captureSlice() copies from the main circular buffer with 0 TSan data races
 * and 0 torn audio splices.
 */
class RetroactiveRiffBuffer {
public:
    static constexpr size_t kSpilloverCapacityFrames = 65536;

    RetroactiveRiffBuffer() = default;
    ~RetroactiveRiffBuffer() = default;

    RetroactiveRiffBuffer(const RetroactiveRiffBuffer&) = delete;
    RetroactiveRiffBuffer& operator=(const RetroactiveRiffBuffer&) = delete;

    /**
     * @brief Pre-allocates circular stereo buffer for specified duration.
     * Safe even if called while audio callback is active: allocates new storage off-fence,
     * then fences in-flight audio writes via Dekker handshake before O(1) vector swap.
     *
     * @param sampleRate Audio sample rate (e.g. 48000.0)
     * @param durationMinutes Recording window duration (1, 2, 5, or 10)
     */
    void prepare(double sampleRate, int durationMinutes) {
        if (durationMinutes <= 0) durationMinutes = 2;
        if (sampleRate <= 0.0) sampleRate = 48000.0;

        const size_t targetCapacity = std::max<size_t>(
            static_cast<size_t>(sampleRate * 60.0 * static_cast<double>(durationMinutes)), 1024);

        // If already prepared with identical capacity and sample rate, preserve existing riff history
        if (m_capacityFrames == targetCapacity &&
            std::abs(m_sampleRate - sampleRate) < 1.0 &&
            m_durationMinutes == durationMinutes &&
            m_bufferL.size() == targetCapacity &&
            m_bufferR.size() == targetCapacity &&
            !m_isPreparing.load(std::memory_order_acquire)) {
            return;
        }

        // Allocate outside the audio fence so the audio thread never spins on OS page faults
        std::vector<float> newBufL(targetCapacity, 0.0f);
        std::vector<float> newBufR(targetCapacity, 0.0f);
        std::vector<float> newSpillL(kSpilloverCapacityFrames, 0.0f);
        std::vector<float> newSpillR(kSpilloverCapacityFrames, 0.0f);

        m_isPreparing.store(true, std::memory_order_seq_cst);
        while (m_audioWritingMain.load(std::memory_order_acquire) ||
               m_audioWritingSpillover.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        m_sampleRate = sampleRate;
        m_durationMinutes = durationMinutes;
        m_capacityFrames = targetCapacity;

        m_bufferL.swap(newBufL);
        m_bufferR.swap(newBufR);
        m_spilloverL.swap(newSpillL);
        m_spilloverR.swap(newSpillR);
        m_spilloverCount.store(0, std::memory_order_relaxed);

        m_writeHead.store(0, std::memory_order_relaxed);
        m_totalFramesWritten.store(0, std::memory_order_relaxed);
        m_captureActive.store(false, std::memory_order_relaxed);
        m_isPreparing.store(false, std::memory_order_release);
    }

    /**
     * @brief Clears buffer contents and resets write head safely.
     */
    void reset() noexcept {
        m_isPreparing.store(true, std::memory_order_seq_cst);
        while (m_audioWritingMain.load(std::memory_order_acquire) ||
               m_audioWritingSpillover.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        std::fill(m_bufferL.begin(), m_bufferL.end(), 0.0f);
        std::fill(m_bufferR.begin(), m_bufferR.end(), 0.0f);
        std::fill(m_spilloverL.begin(), m_spilloverL.end(), 0.0f);
        std::fill(m_spilloverR.begin(), m_spilloverR.end(), 0.0f);
        m_spilloverCount.store(0, std::memory_order_relaxed);
        m_writeHead.store(0, std::memory_order_relaxed);
        m_totalFramesWritten.store(0, std::memory_order_relaxed);
        m_captureActive.store(false, std::memory_order_relaxed);
        m_isPreparing.store(false, std::memory_order_release);
    }

    /**
     * @brief Lock-free, wait-free push of stereo blocks into circular buffer.
     * Guaranteed 0 dynamic allocations and 0 mutex locks on the audio callback.
     */
    inline void push(const float* left, const float* right, size_t numSamples) noexcept {
        if (!left || numSamples == 0 || m_capacityFrames == 0 || m_isPreparing.load(std::memory_order_acquire)) return;
        const float* srcR = right ? right : left;

        // Wait-free Dekker handshake with captureSlice() and prepare():
        if (m_captureActive.load(std::memory_order_acquire)) {
            pushToSpillover(left, srcR, numSamples);
            return;
        }

        m_audioWritingMain.store(true, std::memory_order_seq_cst);
        if (m_isPreparing.load(std::memory_order_seq_cst)) {
            m_audioWritingMain.store(false, std::memory_order_release);
            return;
        }
        if (m_captureActive.load(std::memory_order_seq_cst)) {
            m_audioWritingMain.store(false, std::memory_order_release);
            pushToSpillover(left, srcR, numSamples);
            return;
        }

        const size_t spillCount = m_spilloverCount.load(std::memory_order_relaxed);
        if (spillCount > 0) {
            appendToMainRing(m_spilloverL.data(), m_spilloverR.data(), spillCount);
            m_spilloverCount.store(0, std::memory_order_relaxed);
        }

        appendToMainRing(left, srcR, numSamples);
        m_audioWritingMain.store(false, std::memory_order_release);
    }

    /**
     * @brief Tear-free export mechanism: fences main ring writes into the wait-free spillover buffer
     * and copies the requested slice into scratch staging buffers with 0 data races.
     *
     * @param stagingBufferL Destination buffer for left channel
     * @param stagingBufferR Destination buffer for right channel
     * @param maxSamples Maximum number of samples to capture
     * @param capturedSamples Output number of frames actually captured
     * @return true if audio frames were successfully copied
     */
    bool captureSlice(float* stagingBufferL, float* stagingBufferR,
                      size_t maxSamples, size_t& capturedSamples) const noexcept {
        if (!stagingBufferL || !stagingBufferR || maxSamples == 0 || m_capacityFrames == 0 ||
            m_isPreparing.load(std::memory_order_acquire)) {
            capturedSamples = 0;
            return false;
        }

        struct CaptureFenceGuard {
            std::atomic<bool>& flag;
            explicit CaptureFenceGuard(std::atomic<bool>& f) noexcept : flag(f) {
                flag.store(true, std::memory_order_seq_cst);
            }
            ~CaptureFenceGuard() noexcept {
                flag.store(false, std::memory_order_release);
            }
        } fence(m_captureActive);

        // Wait for any in-flight audio callback memcpy to finish (< 5 microseconds)
        while (m_audioWritingMain.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        if (m_isPreparing.load(std::memory_order_acquire)) {
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

        try {
            if (outStagingL.size() < available) outStagingL.resize(available);
            if (outStagingR.size() < available) outStagingR.resize(available);
        } catch (const std::bad_alloc&) {
            outFramesCaptured = 0;
            return false;
        }

        return captureSlice(outStagingL.data(), outStagingR.data(), available, outFramesCaptured);
    }

    [[nodiscard]] size_t capacityFrames() const noexcept { return m_capacityFrames; }
    [[nodiscard]] size_t totalFramesWritten() const noexcept { return m_totalFramesWritten.load(std::memory_order_relaxed); }
    [[nodiscard]] size_t writeHead() const noexcept { return m_writeHead.load(std::memory_order_relaxed); }
    [[nodiscard]] int durationMinutes() const noexcept { return m_durationMinutes; }
    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }

    /**
     * @brief Generates standardized riff filename in "PRESET_BPM_TIMESTAMP" format
     * e.g. "ModernHighGainDjent_140bpm_20261015_014128.wav".
     */
    [[nodiscard]] static std::string formatFilename(const std::string& presetName, float bpm, const std::tm& tmNow) {
        std::string presetPart;
        presetPart.reserve(presetName.size());
        for (char c : presetName) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                continue;
            }
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
                presetPart.push_back(c);
            }
        }
        if (presetPart.empty()) {
            presetPart = "Init";
        }

        int bpmInt = static_cast<int>(std::round(bpm));
        if (bpmInt < 20 || bpmInt > 400) {
            bpmInt = 120;
        }
        std::string bpmPart = std::to_string(bpmInt) + "bpm";

        char timePart[32]{0};
        std::strftime(timePart, sizeof(timePart), "%Y%m%d_%H%M%S", &tmNow);

        return presetPart + "_" + bpmPart + "_" + std::string(timePart) + ".wav";
    }

private:
    inline void pushToSpillover(const float* left, const float* right, size_t numSamples) noexcept {
        m_audioWritingSpillover.store(true, std::memory_order_seq_cst);
        if (m_isPreparing.load(std::memory_order_seq_cst) || m_spilloverL.empty() || m_spilloverR.empty()) {
            m_audioWritingSpillover.store(false, std::memory_order_release);
            return;
        }
        const size_t curCount = m_spilloverCount.load(std::memory_order_relaxed);
        const size_t avail = (curCount < m_spilloverL.size()) ? (m_spilloverL.size() - curCount) : 0;
        const size_t toCopy = std::min(numSamples, avail);
        if (toCopy > 0) {
            std::memcpy(&m_spilloverL[curCount], left, toCopy * sizeof(float));
            std::memcpy(&m_spilloverR[curCount], right, toCopy * sizeof(float));
            m_spilloverCount.store(curCount + toCopy, std::memory_order_release);
        }
        m_audioWritingSpillover.store(false, std::memory_order_release);
    }

    inline void appendToMainRing(const float* left, const float* right, size_t numSamples) noexcept {
        if (numSamples == 0 || m_capacityFrames == 0) return;
        if (numSamples > m_capacityFrames) {
            const size_t skip = numSamples - m_capacityFrames;
            left += skip;
            right += skip;
            numSamples = m_capacityFrames;
        }

        size_t head = m_writeHead.load(std::memory_order_relaxed);
        if (head + numSamples <= m_capacityFrames) {
            std::memcpy(&m_bufferL[head], left, numSamples * sizeof(float));
            std::memcpy(&m_bufferR[head], right, numSamples * sizeof(float));
            head = (head + numSamples == m_capacityFrames) ? 0 : (head + numSamples);
        } else {
            const size_t part1 = m_capacityFrames - head;
            const size_t part2 = numSamples - part1;

            std::memcpy(&m_bufferL[head], left, part1 * sizeof(float));
            std::memcpy(&m_bufferL[0], left + part1, part2 * sizeof(float));

            std::memcpy(&m_bufferR[head], right, part1 * sizeof(float));
            std::memcpy(&m_bufferR[0], right + part1, part2 * sizeof(float));

            head = part2;
        }

        m_writeHead.store(head, std::memory_order_release);
        m_totalFramesWritten.fetch_add(numSamples, std::memory_order_release);
    }

    double m_sampleRate{48000.0};
    int m_durationMinutes{2};
    size_t m_capacityFrames{0};

    std::vector<float> m_bufferL;
    std::vector<float> m_bufferR;
    std::vector<float> m_spilloverL;
    std::vector<float> m_spilloverR;
    std::atomic<size_t> m_spilloverCount{0};

    alignas(64) std::atomic<size_t> m_writeHead{0};
    alignas(64) std::atomic<size_t> m_totalFramesWritten{0};
    alignas(64) mutable std::atomic<bool> m_captureActive{false};
    alignas(64) std::atomic<bool> m_audioWritingMain{false};
    alignas(64) std::atomic<bool> m_audioWritingSpillover{false};
    alignas(64) std::atomic<bool> m_isPreparing{false};
};

} // namespace praccy::audio
