#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace praccy::utils {

#pragma pack(push, 1)
struct WavRiffHeader {
    char riffTag[4];      // "RIFF"
    uint32_t fileSize;
    char waveTag[4];      // "WAVE"
};

struct WavChunkHeader {
    char tag[4];
    uint32_t size;
};

struct WavFmtChunk {
    uint16_t audioFormat;    // 1 = PCM, 3 = IEEE float
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)

struct WavAudioData {
    std::vector<float> samplesL;
    std::vector<float> samplesR;
    uint32_t sampleRate{48000};
    uint16_t numChannels{2};
    size_t numFrames{0};
};

class WavReader {
public:
    static constexpr size_t kMaxChunkSize = 256 * 1024 * 1024; // 256 MB bounds limit

    static bool loadWavFile(const std::string& filePath, WavAudioData& outData) {
        outData.samplesL.clear();
        outData.samplesR.clear();
        outData.numFrames = 0;

        if (filePath.empty()) return false;

        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) return false;

        file.seekg(0, std::ios::end);
        std::streamsize fileSize = file.tellg();
        file.seekg(0, std::ios::beg);

        if (fileSize < static_cast<std::streamsize>(sizeof(WavRiffHeader))) {
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
                    uint32_t readSize = std::min(chunk.size, static_cast<uint32_t>(sizeof(fmt)));
                    file.read(reinterpret_cast<char*>(&fmt), readSize);
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
                    break;
                } else {
                    file.seekg(chunk.size, std::ios::cur);
                }

                // RIFF 2-byte word-alignment pad byte
                if (chunk.size & 1) {
                    file.seekg(1, std::ios::cur);
                }
            }

            if (!foundFmt || rawAudioData.empty()) return false;
            if (fmt.numChannels == 0 || fmt.bitsPerSample == 0) return false;

            const size_t bytesPerSample = fmt.bitsPerSample / 8;
            if (bytesPerSample == 0) return false;
            const size_t frameSize = fmt.numChannels * bytesPerSample;
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
                        const int16_t* p16 = reinterpret_cast<const int16_t*>(ptr);
                        s0 = p16[0] / 32768.0f;
                        s1 = (fmt.numChannels > 1) ? (p16[1] / 32768.0f) : s0;
                    } else if (fmt.bitsPerSample == 24) {
                        auto read24 = [](const uint8_t* b) -> float {
                            int32_t val = (static_cast<int32_t>(b[0]) << 8) |
                                          (static_cast<int32_t>(b[1]) << 16) |
                                          (static_cast<int32_t>(b[2]) << 24);
                            return (val >> 8) / 8388608.0f;
                        };
                        s0 = read24(ptr);
                        s1 = (fmt.numChannels > 1) ? read24(ptr + 3) : s0;
                    } else if (fmt.bitsPerSample == 32) {
                        const int32_t* p32 = reinterpret_cast<const int32_t*>(ptr);
                        s0 = p32[0] / 2147483648.0f;
                        s1 = (fmt.numChannels > 1) ? (p32[1] / 2147483648.0f) : s0;
                    } else if (fmt.bitsPerSample == 8) {
                        s0 = (static_cast<float>(ptr[0]) - 128.0f) / 128.0f;
                        s1 = (fmt.numChannels > 1) ? ((static_cast<float>(ptr[1]) - 128.0f) / 128.0f) : s0;
                    }
                } else if (fmt.audioFormat == 3) { // IEEE Float
                    if (fmt.bitsPerSample == 32) {
                        const float* pf = reinterpret_cast<const float*>(ptr);
                        s0 = pf[0];
                        s1 = (fmt.numChannels > 1) ? pf[1] : s0;
                    }
                }

                outData.samplesL[i] = s0;
                outData.samplesR[i] = s1;
                ptr += frameSize;
            }

            return true;
        } catch (const std::bad_alloc&) {
            outData.samplesL.clear();
            outData.samplesR.clear();
            outData.numFrames = 0;
            return false;
        }
    }
};

} // namespace praccy::utils
