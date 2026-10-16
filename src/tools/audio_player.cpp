#include "audio_player.h"
#include "../utils/wav_reader.h"
#include <fstream>
#include <filesystem>
#include <cmath>
#include <cstring>
#include <chrono>

namespace praccy::tools {

AudioPlayer::AudioPlayer() = default;

AudioPlayer::~AudioPlayer() {
    unload();
}

void AudioPlayer::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
}

void AudioPlayer::retireBuffer(AudioTrackBuffer* oldBuf) {
    if (!oldBuf) return;

    while (m_audioReadingBuffer.load(std::memory_order_seq_cst) == oldBuf) {
        std::this_thread::yield();
    }
    delete oldBuf;
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
    if (stopToken.stop_requested()) return false;

    praccy::utils::WavAudioData wavData;
    m_loadProgress.store(0.10f, std::memory_order_relaxed);

    if (!praccy::utils::WavReader::loadWavFile(filePath, wavData) || wavData.numFrames == 0) {
        return false;
    }

    if (stopToken.stop_requested()) return false;
    m_loadProgress.store(0.40f, std::memory_order_relaxed);

    auto newTrack = std::make_unique<AudioTrackBuffer>();

    // Resample to m_sampleRate if needed
    if (wavData.sampleRate != static_cast<uint32_t>(m_sampleRate) && wavData.sampleRate > 0) {
        double ratio = m_sampleRate / static_cast<double>(wavData.sampleRate);
        size_t targetFrames = static_cast<size_t>(wavData.numFrames * ratio);
        newTrack->samplesL.resize(targetFrames);
        newTrack->samplesR.resize(targetFrames);

        for (size_t i = 0; i < targetFrames; ++i) {
            if ((i & 0x3FFF) == 0) {
                if (stopToken.stop_requested()) return false;
                float p = 0.40f + 0.35f * (static_cast<float>(i) / static_cast<float>(targetFrames));
                m_loadProgress.store(p, std::memory_order_relaxed);
            }
            double srcIdx = i / ratio;
            size_t idx0 = static_cast<size_t>(srcIdx);
            size_t idx1 = std::min(idx0 + 1, wavData.numFrames - 1);
            float frac = static_cast<float>(srcIdx - idx0);

            newTrack->samplesL[i] = wavData.samplesL[idx0] * (1.0f - frac) + wavData.samplesL[idx1] * frac;
            newTrack->samplesR[i] = wavData.samplesR[idx0] * (1.0f - frac) + wavData.samplesR[idx1] * frac;
        }
    } else {
        newTrack->samplesL = std::move(wavData.samplesL);
        newTrack->samplesR = std::move(wavData.samplesR);
    }

    newTrack->numFrames = newTrack->samplesL.size();
    newTrack->filePath = filePath;
    newTrack->fileName = std::filesystem::path(filePath).filename().string();

    if (stopToken.stop_requested()) return false;
    m_loadProgress.store(0.80f, std::memory_order_relaxed);

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

    retireBuffer(oldBuf);
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

    retireBuffer(oldBuf);
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
    m_audioReadingBuffer.store(buf, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (buf != m_activeBuffer.load(std::memory_order_acquire)) {
        // Buffer swap in flight; release hazard pointer and return wait-free
        m_audioReadingBuffer.store(nullptr, std::memory_order_release);
        return;
    }

    if (!buf || buf->numFrames == 0) {
        m_audioReadingBuffer.store(nullptr, std::memory_order_release);
        return;
    }

    const size_t totalFrames = buf->numFrames;
    const uint32_t numSamples = output.numSamples();
    float* outL = output.channel(0);
    float* outR = (output.numChannels() > 1) ? output.channel(1) : outL;
    const float vol = m_volume.load(std::memory_order_relaxed);
    const bool loop = m_looping.load(std::memory_order_relaxed);

    size_t curHead = m_playhead.load(std::memory_order_relaxed);

    const float loopStartNorm = std::clamp(m_loopStartNorm.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const float loopEndNorm = std::clamp(m_loopEndNorm.load(std::memory_order_relaxed), 0.0f, 1.0f);
    const size_t loopStartFrame = static_cast<size_t>(loopStartNorm * totalFrames);
    const size_t loopEndFrame = (loopEndNorm > loopStartNorm)
        ? std::min<size_t>(totalFrames, static_cast<size_t>(loopEndNorm * totalFrames))
        : totalFrames;

    if (loop && (curHead < loopStartFrame || curHead >= loopEndFrame)) {
        curHead = loopStartFrame;
    }

    for (uint32_t s = 0; s < numSamples; ++s) {
        if (loop) {
            if (curHead >= loopEndFrame) {
                curHead = loopStartFrame;
            }
        } else {
            if (curHead >= totalFrames) {
                m_isPlaying.store(false, std::memory_order_relaxed);
                break;
            }
        }
        outL[s] += buf->samplesL[curHead] * vol;
        outR[s] += buf->samplesR[curHead] * vol;
        curHead++;
    }

    m_playhead.store(curHead, std::memory_order_relaxed);
    m_audioReadingBuffer.store(nullptr, std::memory_order_release);
}

} // namespace praccy::tools
