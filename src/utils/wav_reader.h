#pragma once

#include <vector>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace praccy::utils {

struct WavAudioData {
    std::vector<float> samplesL;
    std::vector<float> samplesR;
    uint32_t sampleRate{48000};
    uint16_t numChannels{2};
    size_t numFrames{0};
};

#pragma pack(push, 1)
struct WavRiffHeader {
    char riffTag[4];
    uint32_t fileSize;
    char waveTag[4];
};

struct WavChunkHeader {
    char tag[4];
    uint32_t size;
};

struct WavFmtChunk {
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)

class WavReader {
public:
    static constexpr uint32_t kMaxChunkSize = 512 * 1024 * 1024; // 512 MB guard
    static constexpr uint32_t kMinSampleRate = 8000;
    static constexpr uint32_t kMaxSampleRate = 384000;
    static constexpr uint16_t kMaxChannels = 8;

    static bool loadWavFile(const std::string& filePath, WavAudioData& outData) {
        if (filePath.empty() || filePath.size() > 4096) return false;

        std::error_code ec;
        std::filesystem::path p(filePath);
        if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
            return false;
        }

        std::ifstream file(p, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;

        std::streamsize fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        if (fileSize < static_cast<std::streamsize>(sizeof(WavRiffHeader) + sizeof(WavChunkHeader) + sizeof(WavFmtChunk))) {
            return false;
        }

        WavRiffHeader riff{};
        file.read(reinterpret_cast<char*>(&riff), sizeof(riff));
        if (file.gcount() < static_cast<std::streamsize>(sizeof(riff))) return false;

        if (std::memcmp(riff.riffTag, "RIFF", 4) != 0 || std::memcmp(riff.waveTag, "WAVE", 4) != 0) {
            return false;
        }

        WavFmtChunk fmt{};
        bool foundFmt = false;
        std::vector<uint8_t> rawAudioData;

        try {
            while (file) {
                std::streamsize curPos = file.tellg();
                if (curPos < 0 || curPos >= fileSize) break;

                WavChunkHeader chunk{};
                file.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
                if (file.gcount() < static_cast<std::streamsize>(sizeof(chunk))) break;

                curPos = file.tellg();
                if (chunk.size > kMaxChunkSize || static_cast<std::streamsize>(chunk.size) > (fileSize - curPos)) {
                    return false;
                }

                if (std::memcmp(chunk.tag, "fmt ", 4) == 0) {
                    if (chunk.size < sizeof(WavFmtChunk)) {
                        return false;
                    }
                    uint32_t readSize = std::min(chunk.size, static_cast<uint32_t>(sizeof(fmt)));
                    file.read(reinterpret_cast<char*>(&fmt), readSize);
                    if (file.gcount() < static_cast<std::streamsize>(readSize)) {
                        return false;
                    }
                    if (chunk.size > readSize) {
                        file.seekg(chunk.size - readSize, std::ios::cur);
                    }
                    foundFmt = true;
                } else if (std::memcmp(chunk.tag, "data", 4) == 0) {
                    rawAudioData.resize(chunk.size);
                    file.read(reinterpret_cast<char*>(rawAudioData.data()), chunk.size);
                    if (file.gcount() < static_cast<std::streamsize>(chunk.size)) {
                        return false;
                    }
                    if (foundFmt) {
                        break;
                    }
                } else {
                    file.seekg(chunk.size, std::ios::cur);
                }

                // RIFF 2-byte word-alignment pad byte
                if (chunk.size & 1) {
                    file.seekg(1, std::ios::cur);
                }
            }

            if (!foundFmt || rawAudioData.empty()) return false;
            if (fmt.numChannels == 0 || fmt.numChannels > kMaxChannels) return false;
            if (fmt.sampleRate < kMinSampleRate || fmt.sampleRate > kMaxSampleRate) return false;
            if (fmt.audioFormat != 1 && fmt.audioFormat != 3) return false;
            if (fmt.bitsPerSample != 8 && fmt.bitsPerSample != 16 && fmt.bitsPerSample != 24 && fmt.bitsPerSample != 32) return false;
            if (fmt.audioFormat == 3 && fmt.bitsPerSample != 32) return false;

            const size_t bytesPerSample = fmt.bitsPerSample / 8;
            const size_t frameSize = static_cast<size_t>(fmt.numChannels) * bytesPerSample;
            if (frameSize == 0) return false;
            const size_t numFrames = rawAudioData.size() / frameSize;
            if (numFrames == 0) return false;

            outData.sampleRate = fmt.sampleRate;
            outData.numChannels = fmt.numChannels;
            outData.numFrames = numFrames;
            outData.samplesL.resize(numFrames);
            outData.samplesR.resize(numFrames);

            const uint8_t* ptr = rawAudioData.data();
            for (size_t i = 0; i < numFrames; ++i) {
                float s0 = 0.0f;
                float s1 = 0.0f;

                if (fmt.audioFormat == 1) { // PCM
                    if (fmt.bitsPerSample == 16) {
                        int16_t v0 = 0;
                        std::memcpy(&v0, ptr, sizeof(int16_t));
                        s0 = static_cast<float>(v0) / 32768.0f;
                        if (fmt.numChannels > 1) {
                            int16_t v1 = 0;
                            std::memcpy(&v1, ptr + sizeof(int16_t), sizeof(int16_t));
                            s1 = static_cast<float>(v1) / 32768.0f;
                        } else {
                            s1 = s0;
                        }
                    } else if (fmt.bitsPerSample == 24) {
                        auto read24 = [](const uint8_t* b) noexcept -> float {
                            const uint32_t uval = (static_cast<uint32_t>(b[0]) << 8) |
                                                  (static_cast<uint32_t>(b[1]) << 16) |
                                                  (static_cast<uint32_t>(b[2]) << 24);
                            const int32_t val = static_cast<int32_t>(uval) >> 8;
                            return static_cast<float>(static_cast<double>(val) / 8388608.0);
                        };
                        s0 = read24(ptr);
                        s1 = (fmt.numChannels > 1) ? read24(ptr + 3) : s0;
                    } else if (fmt.bitsPerSample == 32) {
                        int32_t v0 = 0;
                        std::memcpy(&v0, ptr, sizeof(int32_t));
                        s0 = static_cast<float>(static_cast<double>(v0) / 2147483648.0);
                        if (fmt.numChannels > 1) {
                            int32_t v1 = 0;
                            std::memcpy(&v1, ptr + sizeof(int32_t), sizeof(int32_t));
                            s1 = static_cast<float>(static_cast<double>(v1) / 2147483648.0);
                        } else {
                            s1 = s0;
                        }
                    } else if (fmt.bitsPerSample == 8) {
                        s0 = (static_cast<float>(ptr[0]) - 128.0f) / 128.0f;
                        s1 = (fmt.numChannels > 1) ? ((static_cast<float>(ptr[1]) - 128.0f) / 128.0f) : s0;
                    }
                } else if (fmt.audioFormat == 3) { // IEEE Float
                    float v0 = 0.0f;
                    std::memcpy(&v0, ptr, sizeof(float));
                    s0 = std::isfinite(v0) ? v0 : 0.0f;
                    if (fmt.numChannels > 1) {
                        float v1 = 0.0f;
                        std::memcpy(&v1, ptr + sizeof(float), sizeof(float));
                        s1 = std::isfinite(v1) ? v1 : 0.0f;
                    } else {
                        s1 = s0;
                    }
                }

                outData.samplesL[i] = s0;
                outData.samplesR[i] = s1;
                ptr += frameSize;
            }

            return true;
        } catch (const std::bad_alloc&) {
            outData = WavAudioData{};
            return false;
        }
    }
};

} // namespace praccy::utils
