#pragma once

#include "../audio/audio_buffer.h"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <algorithm>

namespace praccy::tools {

class AudioPlayer {
public:
    AudioPlayer() = default;

    void prepare(double sampleRate);
    bool loadWavFile(const std::string& filePath);
    void unload();

    void play();
    void pause();
    void stop();
    void seek(float normalizedPos);

    void setVolume(float vol) noexcept { m_volume.store(std::clamp(vol, 0.0f, 2.0f), std::memory_order_relaxed); }
    [[nodiscard]] float volume() const noexcept { return m_volume.load(std::memory_order_relaxed); }

    void setLooping(bool loop) noexcept { m_looping.store(loop, std::memory_order_relaxed); }
    [[nodiscard]] bool isLooping() const noexcept { return m_looping.load(std::memory_order_relaxed); }

    [[nodiscard]] bool isPlaying() const noexcept { return m_isPlaying.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isLoaded() const noexcept { return m_isLoaded.load(std::memory_order_relaxed); }
    [[nodiscard]] const std::string& filePath() const noexcept { return m_filePath; }
    [[nodiscard]] const std::string& fileName() const noexcept { return m_fileName; }
    [[nodiscard]] double durationSeconds() const noexcept;
    [[nodiscard]] double positionSeconds() const noexcept;
    [[nodiscard]] float positionNormalized() const noexcept;

    void process(audio::AudioBufferView& output);

private:
    double m_sampleRate{48000.0};
    std::string m_filePath;
    std::string m_fileName;

    std::vector<float> m_samplesL;
    std::vector<float> m_samplesR;
    std::atomic<size_t> m_playhead{0};

    std::atomic<bool> m_isPlaying{false};
    std::atomic<bool> m_isLoaded{false};
    std::atomic<bool> m_looping{true};
    std::atomic<float> m_volume{1.0f};

    std::mutex m_loadMutex;
};

} // namespace praccy::tools
