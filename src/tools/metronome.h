#pragma once

#include "../audio/audio_buffer.h"
#include <atomic>
#include <cstdint>

namespace praccy::tools {

/**
 * @brief Sample-accurate metronome click generator.
 */
class Metronome {
public:
    Metronome();

    void prepare(double sampleRate);
    void process(audio::AudioBufferView& output);
    void reset();

    void setBpm(float bpm) noexcept { m_bpm.store(std::clamp(bpm, 20.0f, 400.0f), std::memory_order_relaxed); }
    [[nodiscard]] float bpm() const noexcept { return m_bpm.load(std::memory_order_relaxed); }

    void setBeatsPerBar(int beats) noexcept { m_beatsPerBar.store(std::clamp(beats, 1, 16), std::memory_order_relaxed); }
    [[nodiscard]] int beatsPerBar() const noexcept { return m_beatsPerBar.load(std::memory_order_relaxed); }

    void setPlaying(bool playing) noexcept {
        if (playing) {
            m_currentBeatSample = 0;
            m_clickSamplePosition = 0; // Trigger downbeat click immediately
            m_currentBeat.store(0, std::memory_order_relaxed);
            m_isAccent = true;
        } else {
            m_currentBeatSample = 0;
            m_clickSamplePosition = m_clickLengthSamples;
        }
        m_playing.store(playing, std::memory_order_relaxed);
    }
    [[nodiscard]] bool isPlaying() const noexcept { return m_playing.load(std::memory_order_relaxed); }

    void setVolumeDb(float db) noexcept { m_volumeDb.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float volumeDb() const noexcept { return m_volumeDb.load(std::memory_order_relaxed); }

    [[nodiscard]] int currentBeat() const noexcept { return m_currentBeat.load(std::memory_order_relaxed); }

private:
    double m_sampleRate{48000.0};
    std::atomic<float> m_bpm{120.0f};
    std::atomic<int> m_beatsPerBar{4};
    std::atomic<bool> m_playing{false};
    std::atomic<float> m_volumeDb{-6.0f};
    std::atomic<int> m_currentBeat{0};

    uint64_t m_currentBeatSample{0};
    uint32_t m_clickSamplePosition{0};
    uint32_t m_clickLengthSamples{0};
    bool m_isAccent{false};
};

} // namespace praccy::tools
