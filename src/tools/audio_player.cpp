#include "audio_player.h"
#include <fstream>
#include <filesystem>
#include <cmath>
#include <cstring>

namespace praccy::tools {

void AudioPlayer::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
}

#pragma pack(push, 1)
struct RiffHeader {
    char riffTag[4];      // "RIFF"
    uint32_t fileSize;
    char waveTag[4];      // "WAVE"
};

struct ChunkHeader {
    char tag[4];
    uint32_t size;
};

struct FmtChunk {
    uint16_t audioFormat;    // 1 = PCM, 3 = IEEE float
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)

bool AudioPlayer::loadWavFile(const std::string& filePath) {
    std::lock_guard<std::mutex> lock(m_loadMutex);

    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) return false;

    RiffHeader riff{};
    file.read(reinterpret_cast<char*>(&riff), sizeof(riff));
    if (file.gcount() < static_cast<std::streamsize>(sizeof(riff))) return false;

    if (std::memcmp(riff.riffTag, "RIFF", 4) != 0 || std::memcmp(riff.waveTag, "WAVE", 4) != 0) {
        return false;
    }

    FmtChunk fmt{};
    bool foundFmt = false;
    std::vector<uint8_t> rawAudioData;

    while (file) {
        ChunkHeader chunk{};
        file.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
        if (file.gcount() < static_cast<std::streamsize>(sizeof(chunk))) break;

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
            break;
        } else {
            file.seekg(chunk.size, std::ios::cur);
        }
    }

    if (!foundFmt || rawAudioData.empty()) return false;
    if (fmt.numChannels == 0 || fmt.bitsPerSample == 0) return false;

    const size_t bytesPerSample = fmt.bitsPerSample / 8;
    const size_t frameSize = fmt.numChannels * bytesPerSample;
    const size_t numFrames = rawAudioData.size() / frameSize;
    if (numFrames == 0) return false;

    std::vector<float> srcL(numFrames);
    std::vector<float> srcR(numFrames);

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
                    int32_t val = (b[0] << 8) | (b[1] << 16) | (b[2] << 24);
                    return (val >> 8) / 8388608.0f;
                };
                s0 = read24(ptr);
                s1 = (fmt.numChannels > 1) ? read24(ptr + 3) : s0;
            } else if (fmt.bitsPerSample == 32) {
                const int32_t* p32 = reinterpret_cast<const int32_t*>(ptr);
                s0 = p32[0] / 2147483648.0f;
                s1 = (fmt.numChannels > 1) ? (p32[1] / 2147483648.0f) : s0;
            }
        } else if (fmt.audioFormat == 3) { // IEEE Float
            if (fmt.bitsPerSample == 32) {
                const float* pf = reinterpret_cast<const float*>(ptr);
                s0 = pf[0];
                s1 = (fmt.numChannels > 1) ? pf[1] : s0;
            }
        }

        srcL[i] = s0;
        srcR[i] = s1;
        ptr += frameSize;
    }

    // Resample to m_sampleRate if needed
    if (fmt.sampleRate != static_cast<uint32_t>(m_sampleRate) && fmt.sampleRate > 0) {
        double ratio = m_sampleRate / static_cast<double>(fmt.sampleRate);
        size_t targetFrames = static_cast<size_t>(numFrames * ratio);
        m_samplesL.resize(targetFrames);
        m_samplesR.resize(targetFrames);

        for (size_t i = 0; i < targetFrames; ++i) {
            double srcIdx = i / ratio;
            size_t idx0 = static_cast<size_t>(srcIdx);
            size_t idx1 = std::min(idx0 + 1, numFrames - 1);
            float frac = static_cast<float>(srcIdx - idx0);

            m_samplesL[i] = srcL[idx0] * (1.0f - frac) + srcL[idx1] * frac;
            m_samplesR[i] = srcR[idx0] * (1.0f - frac) + srcR[idx1] * frac;
        }
    } else {
        m_samplesL = std::move(srcL);
        m_samplesR = std::move(srcR);
    }

    m_filePath = filePath;
    m_fileName = std::filesystem::path(filePath).filename().string();
    m_playhead.store(0);
    m_isLoaded.store(true);
    return true;
}

void AudioPlayer::unload() {
    stop();
    std::lock_guard<std::mutex> lock(m_loadMutex);
    m_samplesL.clear();
    m_samplesR.clear();
    m_filePath.clear();
    m_fileName.clear();
    m_isLoaded.store(false);
}

void AudioPlayer::play() {
    if (m_isLoaded.load(std::memory_order_relaxed)) {
        m_isPlaying.store(true, std::memory_order_relaxed);
    }
}

void AudioPlayer::pause() {
    m_isPlaying.store(false, std::memory_order_relaxed);
}

void AudioPlayer::stop() {
    m_isPlaying.store(false, std::memory_order_relaxed);
    m_playhead.store(0, std::memory_order_relaxed);
}

void AudioPlayer::seek(float normalizedPos) {
    if (!m_isLoaded.load(std::memory_order_relaxed) || m_samplesL.empty()) return;
    normalizedPos = std::clamp(normalizedPos, 0.0f, 1.0f);
    size_t newHead = static_cast<size_t>(normalizedPos * (m_samplesL.size() - 1));
    m_playhead.store(newHead, std::memory_order_relaxed);
}

double AudioPlayer::durationSeconds() const noexcept {
    if (m_sampleRate <= 0.0 || m_samplesL.empty()) return 0.0;
    return m_samplesL.size() / m_sampleRate;
}

double AudioPlayer::positionSeconds() const noexcept {
    if (m_sampleRate <= 0.0 || m_samplesL.empty()) return 0.0;
    return m_playhead.load(std::memory_order_relaxed) / m_sampleRate;
}

float AudioPlayer::positionNormalized() const noexcept {
    if (m_samplesL.empty()) return 0.0f;
    return static_cast<float>(m_playhead.load(std::memory_order_relaxed)) / static_cast<float>(m_samplesL.size());
}

void AudioPlayer::process(audio::AudioBufferView& output) {
    if (!m_isPlaying.load(std::memory_order_relaxed) || !m_isLoaded.load(std::memory_order_relaxed)) return;

    const size_t totalFrames = m_samplesL.size();
    if (totalFrames == 0) return;

    const uint32_t numSamples = output.numSamples();
    float* outL = output.channel(0);
    float* outR = (output.numChannels() > 1) ? output.channel(1) : outL;
    const float vol = m_volume.load(std::memory_order_relaxed);
    const bool loop = m_looping.load(std::memory_order_relaxed);

    size_t curHead = m_playhead.load(std::memory_order_relaxed);

    for (uint32_t s = 0; s < numSamples; ++s) {
        if (curHead >= totalFrames) {
            if (loop) {
                curHead = 0;
            } else {
                m_isPlaying.store(false, std::memory_order_relaxed);
                break;
            }
        }
        outL[s] += m_samplesL[curHead] * vol;
        outR[s] += m_samplesR[curHead] * vol;
        curHead++;
    }

    m_playhead.store(curHead, std::memory_order_relaxed);
}

} // namespace praccy::tools
