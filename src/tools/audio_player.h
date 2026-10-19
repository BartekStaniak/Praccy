#pragma once

#include "../audio/audio_buffer.h"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <memory>
#include <algorithm>
#include <utility>

namespace praccy::tools {

struct AudioTrackBuffer {
    std::vector<float> samplesL;
    std::vector<float> samplesR;
    std::string filePath;
    std::string fileName;
    size_t numFrames{0};
    std::vector<std::pair<float, float>> waveformPeaks800;
};

class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    void prepare(double sampleRate);
    bool loadWavFile(const std::string& filePath);
    bool loadWavFileSync(const std::string& filePath);
    void unload();

    void play();
    void pause();
    void stop();
    void seek(float normalizedPos);

    void setVolume(float vol) noexcept { m_volume.store(std::clamp(vol, 0.0f, 2.0f), std::memory_order_relaxed); }
    [[nodiscard]] float volume() const noexcept { return m_volume.load(std::memory_order_relaxed); }

    void setLooping(bool loop) noexcept { m_looping.store(loop, std::memory_order_relaxed); }
    [[nodiscard]] bool isLooping() const noexcept { return m_looping.load(std::memory_order_relaxed); }

    void setLoopPoints(float startNorm, float endNorm) noexcept {
        float s = std::clamp(startNorm, 0.0f, 1.0f);
        float e = std::clamp(endNorm, 0.0f, 1.0f);
        if (s > e) std::swap(s, e);
        m_loopStartNorm.store(s, std::memory_order_relaxed);
        m_loopEndNorm.store(e, std::memory_order_relaxed);
    }
    [[nodiscard]] float loopStart() const noexcept { return m_loopStartNorm.load(std::memory_order_relaxed); }
    [[nodiscard]] float loopEnd() const noexcept { return m_loopEndNorm.load(std::memory_order_relaxed); }

    [[nodiscard]] bool isPlaying() const noexcept { return m_isPlaying.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isLoaded() const noexcept { return m_isLoaded.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isLoading() const noexcept { return m_isLoading.load(std::memory_order_relaxed); }
    [[nodiscard]] float loadProgress() const noexcept { return m_loadProgress.load(std::memory_order_relaxed); }

    [[nodiscard]] std::string filePath() const;
    [[nodiscard]] std::string fileName() const;
    [[nodiscard]] double durationSeconds() const noexcept;
    [[nodiscard]] double positionSeconds() const noexcept;
    [[nodiscard]] float positionNormalized() const noexcept;
    [[nodiscard]] std::vector<std::pair<float, float>> waveformPeaks800() const;

    void process(audio::AudioBufferView& output);

private:
    void retireBuffer(AudioTrackBuffer* oldBuf);
    bool executeLoad(const std::string& filePath, std::stop_token stopToken);

    std::atomic<double> m_sampleRate{48000.0};
    std::atomic<size_t> m_playhead{0};

    std::atomic<bool> m_isPlaying{false};
    std::atomic<bool> m_isLoaded{false};
    std::atomic<bool> m_isLoading{false};
    std::atomic<float> m_loadProgress{0.0f};
    std::atomic<bool> m_looping{true};
    std::atomic<float> m_loopStartNorm{0.0f};
    std::atomic<float> m_loopEndNorm{1.0f};
    std::atomic<float> m_volume{1.0f};

    std::atomic<AudioTrackBuffer*> m_activeBuffer{nullptr};
    std::atomic<const AudioTrackBuffer*> m_audioReadingBuffer{nullptr};
    std::atomic<size_t> m_activeNumFrames{0};
    std::atomic<double> m_activeSampleRate{48000.0};
    mutable std::mutex m_metadataMutex;
    std::string m_cachedFilePath;
    std::string m_cachedFileName;
    std::vector<std::pair<float, float>> m_cachedPeaks800;

    std::jthread m_workerThread;
};

} // namespace praccy::tools
