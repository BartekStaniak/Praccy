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
 * @brief Asynchronously encodes and writes a 24-bit PCM WAV file on a detached worker thread.
 */
inline void asyncWriteWav24Bit(const std::string& filePath,
                              std::vector<float> left,
                              std::vector<float> right,
                              uint32_t sampleRate,
                              std::function<void(bool)> onComplete = nullptr) {
    std::thread worker([path = filePath, l = std::move(left), r = std::move(right), sr = sampleRate, cb = std::move(onComplete)]() {
        const size_t numSamples = l.size();
        const bool success = writeWav24Bit(path, l.data(), r.empty() ? nullptr : r.data(), numSamples, sr);
        if (cb) {
            cb(success);
        }
    });
    worker.detach();
}

} // namespace praccy::utils
