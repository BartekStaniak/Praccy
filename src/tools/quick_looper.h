#pragma once

#include "../audio/audio_buffer.h"
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>

namespace praccy::tools {

enum class LooperState {
    Empty,
    Recording,
    Playing,
    Overdubbing,
    Stopped
};

class QuickLooper {
public:
    QuickLooper();

    void prepare(double sampleRate, uint32_t maxSeconds = 60, uint32_t maxBlockSize = 512);

    void triggerAction(); // 1-Button smart cycling: Rec -> Play -> Dub -> Play
    void stop();
    void clear();
    bool loadWavFile(const std::string& filePath);

    void setVolume(float vol) noexcept { m_volume.store(std::clamp(vol, 0.0f, 2.0f), std::memory_order_relaxed); }
    [[nodiscard]] float volume() const noexcept { return m_volume.load(std::memory_order_relaxed); }

    [[nodiscard]] LooperState state() const noexcept { return m_state.load(std::memory_order_relaxed); }
    [[nodiscard]] double loopLengthSeconds() const noexcept;
    [[nodiscard]] float playheadNormalized() const noexcept;

    void process(const audio::AudioBufferView& in, audio::AudioBufferView& out);

private:
    double m_sampleRate{48000.0};
    size_t m_maxFrames{48000 * 60};

    std::vector<float> m_loopBufferL;
    std::vector<float> m_loopBufferR;

    std::atomic<LooperState> m_state{LooperState::Empty};
    std::atomic<size_t> m_loopLength{0};
    std::atomic<size_t> m_head{0};
    std::atomic<float> m_volume{1.0f};
};

} // namespace praccy::tools
