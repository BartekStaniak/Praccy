#include "quick_looper.h"
#include <cstring>
#include <fstream>
#include <cstdint>
#include <cmath>
#include <filesystem>

#pragma pack(push, 1)
struct LooperRiffHeader {
    char riffTag[4];
    uint32_t fileSize;
    char waveTag[4];
};

struct LooperChunkHeader {
    char tag[4];
    uint32_t size;
};

struct LooperFmtChunk {
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
};
#pragma pack(pop)

namespace praccy::tools {

QuickLooper::QuickLooper() {
    prepare(48000.0, 60);
}

void QuickLooper::prepare(double sampleRate, uint32_t maxSeconds, uint32_t maxBlockSize) {
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        sampleRate = 48000.0;
    }
    if (maxSeconds == 0 || maxSeconds > 600) {
        maxSeconds = 60;
    }
    if (maxBlockSize == 0 || maxBlockSize > 65536) {
        maxBlockSize = 512;
    }
    (void)maxBlockSize;

    m_sampleRate = sampleRate;
    m_maxFrames = static_cast<size_t>(sampleRate * maxSeconds);
    try {
        m_loopBufferL.assign(m_maxFrames, 0.0f);
        m_loopBufferR.assign(m_maxFrames, 0.0f);
    } catch (const std::bad_alloc&) {
        m_maxFrames = 0;
        m_loopBufferL.clear();
        m_loopBufferR.clear();
    }
    clear();
}

void QuickLooper::triggerAction() {
    LooperState s = m_state.load(std::memory_order_relaxed);
    switch (s) {
        case LooperState::Empty:
        case LooperState::Stopped:
            // Start recording fresh loop
            m_head.store(0, std::memory_order_relaxed);
            m_loopLength.store(0, std::memory_order_relaxed);
            std::fill(m_loopBufferL.begin(), m_loopBufferL.end(), 0.0f);
            std::fill(m_loopBufferR.begin(), m_loopBufferR.end(), 0.0f);
            m_state.store(LooperState::Recording, std::memory_order_relaxed);
            break;

        case LooperState::Recording:
            // Lock loop length and immediately start looping playback
            if (m_head.load(std::memory_order_relaxed) > 1000) {
                m_loopLength.store(m_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
                m_head.store(0, std::memory_order_relaxed);
                m_state.store(LooperState::Playing, std::memory_order_relaxed);
            } else {
                clear();
            }
            break;

        case LooperState::Playing:
            // Switch to Overdubbing
            m_state.store(LooperState::Overdubbing, std::memory_order_relaxed);
            break;

        case LooperState::Overdubbing:
            // Switch back to Playing
            m_state.store(LooperState::Playing, std::memory_order_relaxed);
            break;
    }
}

void QuickLooper::stop() {
    LooperState s = m_state.load(std::memory_order_relaxed);
    if (s != LooperState::Empty) {
        m_state.store(LooperState::Stopped, std::memory_order_relaxed);
    }
}

void QuickLooper::clear() {
    m_state.store(LooperState::Empty, std::memory_order_relaxed);
    m_head.store(0, std::memory_order_relaxed);
    m_loopLength.store(0, std::memory_order_relaxed);
    std::fill(m_loopBufferL.begin(), m_loopBufferL.end(), 0.0f);
    std::fill(m_loopBufferR.begin(), m_loopBufferR.end(), 0.0f);
}

double QuickLooper::loopLengthSeconds() const noexcept {
    if (!std::isfinite(m_sampleRate) || m_sampleRate <= 0.0) return 0.0;
    return m_loopLength.load(std::memory_order_relaxed) / m_sampleRate;
}

float QuickLooper::playheadNormalized() const noexcept {
    size_t len = m_loopLength.load(std::memory_order_relaxed);
    if (len == 0) return 0.0f;
    return static_cast<float>(m_head.load(std::memory_order_relaxed)) / static_cast<float>(len);
}

void QuickLooper::process(const audio::AudioBufferView& in, audio::AudioBufferView& out) {
    LooperState s = m_state.load(std::memory_order_relaxed);
    if (s == LooperState::Empty || s == LooperState::Stopped) return;

    const uint32_t numSamples = out.numSamples();
    float* outL = out.channel(0);
    float* outR = (out.numChannels() > 1) ? out.channel(1) : outL;
    const float* inL = (in.numChannels() > 0) ? in.channel(0) : nullptr;
    const float* inR = (in.numChannels() > 1) ? in.channel(1) : inL;

    const float vol = m_volume.load(std::memory_order_relaxed);
    size_t curHead = m_head.load(std::memory_order_relaxed);
    size_t curLen = m_loopLength.load(std::memory_order_relaxed);

    if (s == LooperState::Recording) {
        // Record incoming audio into loop buffer
        for (uint32_t i = 0; i < numSamples; ++i) {
            if (curHead < m_maxFrames) {
                m_loopBufferL[curHead] = inL ? inL[i] : 0.0f;
                m_loopBufferR[curHead] = inR ? inR[i] : (inL ? inL[i] : 0.0f);
                curHead++;
            } else {
                // Auto-close loop if max duration reached
                m_loopLength.store(m_maxFrames, std::memory_order_relaxed);
                m_state.store(LooperState::Playing, std::memory_order_relaxed);
                curHead = 0;
                break;
            }
        }
    } else if (s == LooperState::Playing || s == LooperState::Overdubbing) {
        if (curLen == 0) return;

        for (uint32_t i = 0; i < numSamples; ++i) {
            if (curHead >= curLen) {
                curHead = 0;
            }

            // Output loop audio
            outL[i] += m_loopBufferL[curHead] * vol;
            outR[i] += m_loopBufferR[curHead] * vol;

            // If overdubbing, accumulate incoming guitar into loop buffer
            if (s == LooperState::Overdubbing) {
                float addL = inL ? inL[i] : 0.0f;
                float addR = inR ? inR[i] : addL;
                // Soft saturation clamp to prevent overdub runaway
                m_loopBufferL[curHead] = std::clamp(m_loopBufferL[curHead] * 0.95f + addL, -1.0f, 1.0f);
                m_loopBufferR[curHead] = std::clamp(m_loopBufferR[curHead] * 0.95f + addR, -1.0f, 1.0f);
            }

            curHead++;
        }
    }

    m_head.store(curHead, std::memory_order_relaxed);
}

bool QuickLooper::loadWavFile(const std::string& filePath) {
    try {
        std::ifstream file(filePath, std::ios::binary);
        if (!file.is_open()) return false;

        LooperRiffHeader riff{};
        file.read(reinterpret_cast<char*>(&riff), sizeof(riff));
        if (file.gcount() < static_cast<std::streamsize>(sizeof(riff))) return false;

        if (std::memcmp(riff.riffTag, "RIFF", 4) != 0 || std::memcmp(riff.waveTag, "WAVE", 4) != 0) {
            return false;
        }

        file.seekg(0, std::ios::end);
        std::streampos fileSize = file.tellg();
        if (fileSize < static_cast<std::streampos>(sizeof(riff))) return false;
        file.seekg(sizeof(riff), std::ios::beg);

        LooperFmtChunk fmt{};
        bool foundFmt = false;
        std::vector<uint8_t> rawAudioData;

        while (file) {
            LooperChunkHeader chunk{};
            file.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
            if (file.gcount() < static_cast<std::streamsize>(sizeof(chunk))) break;

            std::streampos currentPos = file.tellg();
            if (currentPos < 0) return false;

            // Maximum allowed chunk size is 256 MB for looper audio data
            constexpr uint32_t kMaxChunkSize = 256 * 1024 * 1024;
            if (chunk.size > kMaxChunkSize) {
                return false;
            }

            // Check against remaining file size in stream
            if (static_cast<std::streampos>(chunk.size) > (fileSize - currentPos)) {
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
                break;
            } else {
                file.seekg(chunk.size, std::ios::cur);
            }
        }

        if (!foundFmt || rawAudioData.empty()) return false;
        if (fmt.numChannels == 0 || fmt.bitsPerSample == 0) return false;

        const size_t bytesPerSample = fmt.bitsPerSample / 8;
        const size_t frameSize = fmt.numChannels * bytesPerSample;
        if (frameSize == 0) return false;
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

        if (m_maxFrames == 0) {
            prepare(m_sampleRate > 0.0 ? m_sampleRate : 48000.0, 60);
        }

        // Resample if needed
        std::vector<float> finalL;
        std::vector<float> finalR;
        if (fmt.sampleRate != static_cast<uint32_t>(m_sampleRate) && fmt.sampleRate > 0) {
            double ratio = m_sampleRate / static_cast<double>(fmt.sampleRate);
            size_t targetFrames = static_cast<size_t>(numFrames * ratio);
            finalL.resize(targetFrames);
            finalR.resize(targetFrames);

            for (size_t i = 0; i < targetFrames; ++i) {
                double srcIdx = i / ratio;
                size_t idx0 = static_cast<size_t>(srcIdx);
                size_t idx1 = std::min(idx0 + 1, numFrames - 1);
                float frac = static_cast<float>(srcIdx - idx0);

                finalL[i] = srcL[idx0] * (1.0f - frac) + srcL[idx1] * frac;
                finalR[i] = srcR[idx0] * (1.0f - frac) + srcR[idx1] * frac;
            }
        } else {
            finalL = std::move(srcL);
            finalR = std::move(srcR);
        }

        size_t copyFrames = std::min(finalL.size(), m_maxFrames);
        if (copyFrames == 0) return false;

        if (m_loopBufferL.size() < m_maxFrames) m_loopBufferL.resize(m_maxFrames, 0.0f);
        if (m_loopBufferR.size() < m_maxFrames) m_loopBufferR.resize(m_maxFrames, 0.0f);

        std::memcpy(m_loopBufferL.data(), finalL.data(), copyFrames * sizeof(float));
        std::memcpy(m_loopBufferR.data(), finalR.data(), copyFrames * sizeof(float));

        if (copyFrames < m_maxFrames) {
            std::memset(m_loopBufferL.data() + copyFrames, 0, (m_maxFrames - copyFrames) * sizeof(float));
            std::memset(m_loopBufferR.data() + copyFrames, 0, (m_maxFrames - copyFrames) * sizeof(float));
        }

        m_loopLength.store(copyFrames, std::memory_order_relaxed);
        m_head.store(0, std::memory_order_relaxed);
        m_state.store(LooperState::Stopped, std::memory_order_relaxed);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

} // namespace praccy::tools
