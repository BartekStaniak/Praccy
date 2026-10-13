#include "audio_player.h"
#include <fstream>
#include <filesystem>
#include <cmath>
#include <cstring>
#include <chrono>

namespace praccy::tools {

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

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() {
    unload();
}

void AudioPlayer::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
}

bool AudioPlayer::loadWavFile(const std::string& filePath) {
    if (filePath.empty() || !std::filesystem::exists(filePath)) {
        return false;
    }

    if (m_workerThread.joinable()) {
        m_workerThread.request_stop();
        m_workerThread.join();
    }

    m_isLoading.store(true, std::memory_order_release);
    m_loadProgress.store(0.0f, std::memory_order_relaxed);

    m_workerThread = std::jthread([this, filePath](std::stop_token stopToken) {
        bool ok = executeLoad(filePath, stopToken);
        if (!ok) {
            m_isLoading.store(false, std::memory_order_release);
            m_loadProgress.store(0.0f, std::memory_order_release);
        }
    });

    return true;
}

bool AudioPlayer::loadWavFileSync(const std::string& filePath) {
    if (filePath.empty() || !std::filesystem::exists(filePath)) {
        return false;
    }

    if (m_workerThread.joinable()) {
        m_workerThread.request_stop();
        m_workerThread.join();
    }

    m_isLoading.store(true, std::memory_order_release);
    m_loadProgress.store(0.0f, std::memory_order_relaxed);

    bool ok = executeLoad(filePath, {});
    if (!ok) {
        m_isLoading.store(false, std::memory_order_release);
        m_loadProgress.store(0.0f, std::memory_order_release);
    }
    return ok;
}

bool AudioPlayer::executeLoad(const std::string& filePath, std::stop_token stopToken) {
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
        if (stopToken.stop_requested()) return false;
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
    if (stopToken.stop_requested()) return false;

    const size_t bytesPerSample = fmt.bitsPerSample / 8;
    const size_t frameSize = fmt.numChannels * bytesPerSample;
    const size_t numFrames = rawAudioData.size() / frameSize;
    if (numFrames == 0) return false;

    m_loadProgress.store(0.15f, std::memory_order_relaxed);

    std::vector<float> srcL(numFrames);
    std::vector<float> srcR(numFrames);

    const uint8_t* ptr = rawAudioData.data();
    for (size_t i = 0; i < numFrames; ++i) {
        if ((i & 0x3FFF) == 0) {
            if (stopToken.stop_requested()) return false;
            float p = 0.15f + 0.50f * (static_cast<float>(i) / static_cast<float>(numFrames));
            m_loadProgress.store(p, std::memory_order_relaxed);
        }

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

    auto newTrack = std::make_unique<AudioTrackBuffer>();

    // Resample to m_sampleRate if needed
    if (fmt.sampleRate != static_cast<uint32_t>(m_sampleRate) && fmt.sampleRate > 0) {
        double ratio = m_sampleRate / static_cast<double>(fmt.sampleRate);
        size_t targetFrames = static_cast<size_t>(numFrames * ratio);
        newTrack->samplesL.resize(targetFrames);
        newTrack->samplesR.resize(targetFrames);

        for (size_t i = 0; i < targetFrames; ++i) {
            if ((i & 0x3FFF) == 0) {
                if (stopToken.stop_requested()) return false;
                float p = 0.65f + 0.25f * (static_cast<float>(i) / static_cast<float>(targetFrames));
                m_loadProgress.store(p, std::memory_order_relaxed);
            }
            double srcIdx = i / ratio;
            size_t idx0 = static_cast<size_t>(srcIdx);
            size_t idx1 = std::min(idx0 + 1, numFrames - 1);
            float frac = static_cast<float>(srcIdx - idx0);

            newTrack->samplesL[i] = srcL[idx0] * (1.0f - frac) + srcL[idx1] * frac;
            newTrack->samplesR[i] = srcR[idx0] * (1.0f - frac) + srcR[idx1] * frac;
        }
    } else {
        newTrack->samplesL = std::move(srcL);
        newTrack->samplesR = std::move(srcR);
    }

    newTrack->numFrames = newTrack->samplesL.size();
    newTrack->filePath = filePath;
    newTrack->fileName = std::filesystem::path(filePath).filename().string();

    // Precalculate 800-point overview waveform peaks
    constexpr size_t kPeakPoints = 800;
    newTrack->waveformPeaks800.resize(kPeakPoints, {-0.05f, 0.05f});
    if (newTrack->numFrames > 0) {
        const size_t framesPerPoint = std::max<size_t>(1, newTrack->numFrames / kPeakPoints);
        for (size_t p = 0; p < kPeakPoints; ++p) {
            size_t start = p * framesPerPoint;
            size_t end = std::min(start + framesPerPoint, newTrack->numFrames);
            float minVal = 0.0f;
            float maxVal = 0.0f;
            for (size_t f = start; f < end; ++f) {
                float mix = (newTrack->samplesL[f] + newTrack->samplesR[f]) * 0.5f;
                minVal = std::min(minVal, mix);
                maxVal = std::max(maxVal, mix);
            }
            if (minVal == 0.0f && maxVal == 0.0f) {
                minVal = -0.05f;
                maxVal = 0.05f;
            }
            newTrack->waveformPeaks800[p] = {minVal, maxVal};
        }
    }

    if (stopToken.stop_requested()) return false;

    // Cache metadata
    {
        std::lock_guard<std::mutex> lock(m_metadataMutex);
        m_cachedFilePath = newTrack->filePath;
        m_cachedFileName = newTrack->fileName;
        m_cachedPeaks800 = newTrack->waveformPeaks800;
    }

    // Atomic pointer swap
    AudioTrackBuffer* oldBuf = m_activeBuffer.exchange(newTrack.release(), std::memory_order_acq_rel);
    m_playhead.store(0, std::memory_order_relaxed);
    m_isLoaded.store(true, std::memory_order_release);
    m_loadProgress.store(1.0f, std::memory_order_release);
    m_isLoading.store(false, std::memory_order_release);

    if (oldBuf) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        delete oldBuf;
    }

    return true;
}

void AudioPlayer::unload() {
    stop();
    if (m_workerThread.joinable()) {
        m_workerThread.request_stop();
        m_workerThread.join();
    }
    AudioTrackBuffer* oldBuf = m_activeBuffer.exchange(nullptr, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(m_metadataMutex);
        m_cachedFilePath.clear();
        m_cachedFileName.clear();
        m_cachedPeaks800.clear();
    }
    m_isLoaded.store(false, std::memory_order_release);
    m_isLoading.store(false, std::memory_order_release);
    m_loadProgress.store(0.0f, std::memory_order_release);

    if (oldBuf) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        delete oldBuf;
    }
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
    const AudioTrackBuffer* buf = m_activeBuffer.load(std::memory_order_acquire);
    if (!buf || buf->numFrames == 0) return;
    normalizedPos = std::clamp(normalizedPos, 0.0f, 1.0f);
    size_t newHead = static_cast<size_t>(normalizedPos * (buf->numFrames - 1));
    m_playhead.store(newHead, std::memory_order_relaxed);
}

double AudioPlayer::durationSeconds() const noexcept {
    const AudioTrackBuffer* buf = m_activeBuffer.load(std::memory_order_acquire);
    if (!buf || m_sampleRate <= 0.0 || buf->numFrames == 0) return 0.0;
    return static_cast<double>(buf->numFrames) / m_sampleRate;
}

double AudioPlayer::positionSeconds() const noexcept {
    const AudioTrackBuffer* buf = m_activeBuffer.load(std::memory_order_acquire);
    if (!buf || m_sampleRate <= 0.0 || buf->numFrames == 0) return 0.0;
    return static_cast<double>(m_playhead.load(std::memory_order_relaxed)) / m_sampleRate;
}

float AudioPlayer::positionNormalized() const noexcept {
    const AudioTrackBuffer* buf = m_activeBuffer.load(std::memory_order_acquire);
    if (!buf || buf->numFrames == 0) return 0.0f;
    return static_cast<float>(m_playhead.load(std::memory_order_relaxed)) / static_cast<float>(buf->numFrames);
}

std::string AudioPlayer::filePath() const {
    std::lock_guard<std::mutex> lock(m_metadataMutex);
    return m_cachedFilePath;
}

std::string AudioPlayer::fileName() const {
    std::lock_guard<std::mutex> lock(m_metadataMutex);
    return m_cachedFileName;
}

std::vector<std::pair<float, float>> AudioPlayer::waveformPeaks800() const {
    std::lock_guard<std::mutex> lock(m_metadataMutex);
    return m_cachedPeaks800;
}

void AudioPlayer::process(audio::AudioBufferView& output) {
    if (!m_isPlaying.load(std::memory_order_relaxed) || !m_isLoaded.load(std::memory_order_relaxed)) return;

    const AudioTrackBuffer* buf = m_activeBuffer.load(std::memory_order_acquire);
    if (!buf || buf->numFrames == 0) return;

    const size_t totalFrames = buf->numFrames;
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
        outL[s] += buf->samplesL[curHead] * vol;
        outR[s] += buf->samplesR[curHead] * vol;
        curHead++;
    }

    m_playhead.store(curHead, std::memory_order_relaxed);
}

} // namespace praccy::tools
