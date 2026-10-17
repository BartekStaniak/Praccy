#pragma once

#include "../audio/audio_buffer.h"
#include <vector>
#include <string>
#include <atomic>
#include <algorithm>

#include <array>
#include <thread>
#include <cstdint>

namespace praccy::tools {

enum class LooperState {
    Empty,
    Recording,
    Playing,
    Overdubbing,
    Stopped
};

enum class LooperCommand : uint32_t {
    None = 0,
    Clear,
    TriggerAction,
    Undo,
    Redo,
    Stop
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

    // Undo / Redo for overdub layers
    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const noexcept { return m_canUndo.load(std::memory_order_relaxed); }
    [[nodiscard]] bool canRedo() const noexcept { return m_canRedo.load(std::memory_order_relaxed); }

    // Tempo & Musical Bar/Beat Tracking
    void setBpm(float bpm) noexcept { m_bpm.store(std::clamp(bpm, 20.0f, 320.0f), std::memory_order_relaxed); }
    [[nodiscard]] float bpm() const noexcept { return m_bpm.load(std::memory_order_relaxed); }
    void setBeatsPerBar(int bpb) noexcept { m_beatsPerBar.store(std::clamp(bpb, 1, 16), std::memory_order_relaxed); }
    [[nodiscard]] int beatsPerBar() const noexcept { return m_beatsPerBar.load(std::memory_order_relaxed); }
    [[nodiscard]] int currentBar() const noexcept;
    [[nodiscard]] int currentBeat() const noexcept;
    [[nodiscard]] int totalBars() const noexcept;
    [[nodiscard]] int totalBeats() const noexcept;

    void process(const audio::AudioBufferView& in, audio::AudioBufferView& out);

private:
    double m_sampleRate{48000.0};
    size_t m_maxFrames{48000 * 60};

    std::vector<float> m_loopBufferL;
    std::vector<float> m_loopBufferR;

    // Preallocated buffers for wait-free layer undo/redo
    std::vector<float> m_undoBufferL;
    std::vector<float> m_undoBufferR;
    std::vector<float> m_redoBufferL;
    std::vector<float> m_redoBufferR;

    std::atomic<bool> m_canUndo{false};
    std::atomic<bool> m_canRedo{false};

    std::atomic<float> m_bpm{120.0f};
    std::atomic<int> m_beatsPerBar{4};

    std::atomic<LooperState> m_state{LooperState::Empty};
    std::atomic<size_t> m_loopLength{0};
    std::atomic<size_t> m_head{0};
    std::atomic<float> m_volume{1.0f};

    std::atomic<LooperCommand> m_pendingCommand{LooperCommand::None};
    std::atomic<bool> m_isProcessingAudio{false};
    std::atomic<uint64_t> m_lastProcessEpoch{0};
    std::atomic<int64_t> m_lastProcessTimeNs{0};
    std::atomic<std::thread::id> m_audioThreadId{};
    std::atomic_flag m_stateLock = ATOMIC_FLAG_INIT;

    static constexpr size_t kCmdQueueCapacity = 64;
    std::array<std::atomic<LooperCommand>, kCmdQueueCapacity> m_cmdQueue{};
    std::atomic<size_t> m_cmdWriteHead{0};
    std::atomic<size_t> m_cmdReadHead{0};

    void enqueueCommand(LooperCommand cmd) noexcept;
    bool popCommand(LooperCommand& outCmd) noexcept;
    void drainCommands();
    void drainCommandsSafe();
    [[nodiscard]] bool isAudioThreadActive() const noexcept;
    void executeCommand(LooperCommand cmd);
};

} // namespace praccy::tools
