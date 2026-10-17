#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <functional>
#include <deque>
#include <mutex>
#include <condition_variable>

namespace praccy::utils {

#pragma pack(push, 1)
struct RiffWaveHeader24Bit {
    char riffTag[4]{'R', 'I', 'F', 'F'};
    uint32_t riffSize{0};
    char waveTag[4]{'W', 'A', 'V', 'E'};
    char fmtTag[4]{'f', 'm', 't', ' '};
    uint32_t fmtSize{16};
    uint16_t audioFormat{1};    // PCM
    uint16_t numChannels{2};    // Stereo
    uint32_t sampleRate{48000};
    uint32_t byteRate{288000};  // sampleRate * numChannels * 3
    uint16_t blockAlign{6};     // numChannels * 3
    uint16_t bitsPerSample{24};
    char dataTag[4]{'d', 'a', 't', 'a'};
    uint32_t dataSize{0};       // numSamples * blockAlign
};
#pragma pack(pop)

/**
 * @brief Header-only 24-bit PCM RIFF/WAV file writer.
 *
 * Encodes 32-bit floating-point audio into standard 24-bit PCM little-endian WAV format.
 * Creates any necessary parent directories automatically.
 *
 * @param filePath Absolute or relative destination filesystem path
 * @param left Pointer to left channel audio samples
 * @param right Pointer to right channel audio samples (falls back to left if nullptr)
 * @param numSamples Number of audio frames to encode
 * @param sampleRate Sampling rate in Hz (e.g. 44100, 48000, 96000)
 * @return true if the WAV file was successfully encoded and written to disk
 */
inline bool writeWav24Bit(const std::string& filePath, const float* left, const float* right,
                          size_t numSamples, uint32_t sampleRate) {
    if (!left || numSamples == 0 || sampleRate == 0) {
        return false;
    }

    try {
        const std::filesystem::path path(filePath);
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            return false;
        }

        const uint32_t dataBytes = static_cast<uint32_t>(numSamples * 6); // 2 channels * 3 bytes
        RiffWaveHeader24Bit header;
        header.riffSize = 36 + dataBytes;
        header.sampleRate = sampleRate;
        header.byteRate = sampleRate * 6;
        header.dataSize = dataBytes;

        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        if (!out) return false;

        const float* srcR = right ? right : left;

        // Stream write in chunks of 4096 frames to minimize I/O overhead and stack usage
        constexpr size_t CHUNK_FRAMES = 4096;
        std::vector<uint8_t> byteBuffer(CHUNK_FRAMES * 6);

        size_t samplesRemaining = numSamples;
        size_t offset = 0;

        while (samplesRemaining > 0) {
            const size_t batch = std::min(samplesRemaining, CHUNK_FRAMES);

            for (size_t i = 0; i < batch; ++i) {
                // Left channel 24-bit conversion
                float sampL = left[offset + i];
                if (std::isnan(sampL)) sampL = 0.0f;
                sampL = std::clamp(sampL, -1.0f, 1.0f);
                const int32_t i24L = std::clamp(static_cast<int32_t>(std::round(sampL * 8388607.0f)), -8388608, 8388607);

                // Right channel 24-bit conversion
                float sampR = srcR[offset + i];
                if (std::isnan(sampR)) sampR = 0.0f;
                sampR = std::clamp(sampR, -1.0f, 1.0f);
                const int32_t i24R = std::clamp(static_cast<int32_t>(std::round(sampR * 8388607.0f)), -8388608, 8388607);

                const size_t byteIdx = i * 6;
                // Left: 3 bytes LSB
                byteBuffer[byteIdx + 0] = static_cast<uint8_t>(i24L & 0xFF);
                byteBuffer[byteIdx + 1] = static_cast<uint8_t>((i24L >> 8) & 0xFF);
                byteBuffer[byteIdx + 2] = static_cast<uint8_t>((i24L >> 16) & 0xFF);

                // Right: 3 bytes LSB
                byteBuffer[byteIdx + 3] = static_cast<uint8_t>(i24R & 0xFF);
                byteBuffer[byteIdx + 4] = static_cast<uint8_t>((i24R >> 8) & 0xFF);
                byteBuffer[byteIdx + 5] = static_cast<uint8_t>((i24R >> 16) & 0xFF);
            }

            out.write(reinterpret_cast<const char*>(byteBuffer.data()), batch * 6);
            if (!out) return false;

            offset += batch;
            samplesRemaining -= batch;
        }

        out.flush();
        return out.good();
    } catch (...) {
        return false;
    }
}

/**
 * @brief Bounded background queue draining asynchronous 24-bit WAV export tasks.
 *
 * Owns a single joinable std::jthread worker that drains a mutex-guarded queue
 * of pending export tasks and joins cleanly on exit. Drops duplicate requests
 * if already queued, or if more than 4 exports are queued.
 */
class AsyncWavExportQueue {
public:
    struct ExportTask {
        std::string filePath;
        std::vector<float> left;
        std::vector<float> right;
        uint32_t sampleRate{0};
        std::function<void(bool)> onComplete;
    };

    static AsyncWavExportQueue& instance() {
        static AsyncWavExportQueue s_instance;
        return s_instance;
    }

    AsyncWavExportQueue() {
        m_worker = std::jthread([this](std::stop_token st) {
            workerLoop(st);
        });
    }

    ~AsyncWavExportQueue() {
        shutdown();
    }

    AsyncWavExportQueue(const AsyncWavExportQueue&) = delete;
    AsyncWavExportQueue& operator=(const AsyncWavExportQueue&) = delete;
    AsyncWavExportQueue(AsyncWavExportQueue&&) = delete;
    AsyncWavExportQueue& operator=(AsyncWavExportQueue&&) = delete;

    void push(ExportTask task) {
        std::function<void(bool)> dropCallback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping || m_tasks.size() >= 4) {
                dropCallback = std::move(task.onComplete);
            } else {
                bool isDuplicate = false;
                for (const auto& existing : m_tasks) {
                    if (existing.filePath == task.filePath) {
                        isDuplicate = true;
                        break;
                    }
                }
                if (isDuplicate) {
                    dropCallback = std::move(task.onComplete);
                } else {
                    m_tasks.push_back(std::move(task));
                    m_cv.notify_one();
                    return;
                }
            }
        }
        if (dropCallback) {
            dropCallback(false);
        }
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_stopping = true;
        }
        m_worker.request_stop();
        m_cv.notify_all();
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    size_t pendingCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tasks.size();
    }

    bool isBusy() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_tasks.empty() || m_isProcessing;
    }

    void drain() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_drainCv.wait(lock, [&]() {
            return m_tasks.empty() && !m_isProcessing;
        });
    }

private:
    void workerLoop(std::stop_token st) {
        while (true) {
            ExportTask task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [&]() {
                    return !m_tasks.empty() || st.stop_requested();
                });

                if (m_tasks.empty() && st.stop_requested()) {
                    break;
                }

                if (!m_tasks.empty()) {
                    task = std::move(m_tasks.front());
                    m_tasks.pop_front();
                    m_isProcessing = true;
                }
            }

            if (!task.filePath.empty()) {
                try {
                    const size_t numSamples = task.left.size();
                    const bool success = writeWav24Bit(task.filePath, task.left.data(),
                                                       task.right.empty() ? nullptr : task.right.data(),
                                                       numSamples, task.sampleRate);
                    if (task.onComplete) {
                        task.onComplete(success);
                    }
                } catch (...) {
                    if (task.onComplete) {
                        try { task.onComplete(false); } catch (...) {}
                    }
                }
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_isProcessing = false;
            }
            m_drainCv.notify_all();
        }
    }

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::condition_variable m_drainCv;
    std::deque<ExportTask> m_tasks;
    bool m_stopping{false};
    bool m_isProcessing{false};
    std::jthread m_worker;
};

/**
 * @brief Asynchronously encodes and writes a 24-bit PCM WAV file using a bounded worker queue.
 */
inline void asyncWriteWav24Bit(const std::string& filePath,
                              std::vector<float> left,
                              std::vector<float> right,
                              uint32_t sampleRate,
                              std::function<void(bool)> onComplete = nullptr) {
    AsyncWavExportQueue::instance().push(AsyncWavExportQueue::ExportTask{
        filePath,
        std::move(left),
        std::move(right),
        sampleRate,
        std::move(onComplete)
    });
}

} // namespace praccy::utils
