#include "quick_looper.h"
#include "../utils/wav_reader.h"
#include <fstream>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <chrono>
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

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

    const size_t maxFrames = static_cast<size_t>(sampleRate * static_cast<double>(maxSeconds));
    m_sampleRate.store(sampleRate, std::memory_order_relaxed);
    m_maxFrames.store(maxFrames, std::memory_order_relaxed);
    try {
        m_loopBufferL.assign(maxFrames, 0.0f);
        m_loopBufferR.assign(maxFrames, 0.0f);
        m_undoBufferL.assign(maxFrames, 0.0f);
        m_undoBufferR.assign(maxFrames, 0.0f);
        const size_t numBlocks = (maxFrames + kUndoBlockSize - 1) / kUndoBlockSize;
        const size_t maskWords = (numBlocks + 63) / 64;
        m_overdubbedBlockMask.assign(maskWords, 0ULL);
        m_pendingCommitBlockMask.assign(maskWords, 0ULL);
        m_undoLayerActive = false;
    } catch (const std::bad_alloc&) {
        m_maxFrames.store(0, std::memory_order_relaxed);
        m_loopBufferL.clear();
        m_loopBufferR.clear();
        m_undoBufferL.clear();
        m_undoBufferR.clear();
        m_overdubbedBlockMask.clear();
        m_pendingCommitBlockMask.clear();
        m_undoLayerActive = false;
    }
    m_cmdWriteHead.store(0, std::memory_order_relaxed);
    m_cmdReadHead.store(0, std::memory_order_relaxed);
    for (auto& slot : m_cmdQueue) {
        slot.store(LooperCommand::None, std::memory_order_relaxed);
    }
    m_pendingCommand.store(LooperCommand::None, std::memory_order_relaxed);
    m_lastProcessEpoch.store(0, std::memory_order_relaxed);
    m_lastProcessTimeNs.store(0, std::memory_order_relaxed);
    m_audioThreadId.store(std::thread::id{}, std::memory_order_relaxed);
    clear();
}

void QuickLooper::enqueueCommand(LooperCommand cmd) noexcept {
    size_t idx = m_cmdWriteHead.fetch_add(1, std::memory_order_relaxed);
    m_cmdQueue[idx % kCmdQueueCapacity].store(cmd, std::memory_order_release);
}

bool QuickLooper::popCommand(LooperCommand& outCmd) noexcept {
    size_t read = m_cmdReadHead.load(std::memory_order_relaxed);
    size_t write = m_cmdWriteHead.load(std::memory_order_acquire);
    if (read >= write) {
        return false;
    }
    for (int spin = 0; spin < 50; ++spin) {
        LooperCommand c = m_cmdQueue[read % kCmdQueueCapacity].exchange(LooperCommand::None, std::memory_order_acq_rel);
        if (c != LooperCommand::None) {
            outCmd = c;
            m_cmdReadHead.store(read + 1, std::memory_order_release);
            return true;
        }
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
#endif
    }
    m_cmdReadHead.store(read + 1, std::memory_order_release);
    return false;
}

void QuickLooper::drainCommands() noexcept {
    LooperCommand cmd;
    while (popCommand(cmd)) {
        executeCommand(cmd);
    }
}

void QuickLooper::drainCommandsSafe() noexcept {
    if (m_isProcessingAudio.load(std::memory_order_acquire)) {
        return; // Audio thread will drain commands wait-free on the current/next block
    }
    struct StateLockSpinGuard {
        std::atomic_flag& lock;
        bool acquired{false};
        explicit StateLockSpinGuard(std::atomic_flag& l) noexcept : lock(l) {
            for (int spin = 0; spin < 200; ++spin) {
                if (!lock.test_and_set(std::memory_order_acquire)) {
                    acquired = true;
                    break;
                }
#if defined(_M_X64) || defined(__x86_64__)
                _mm_pause();
#endif
            }
        }
        ~StateLockSpinGuard() noexcept {
            if (acquired) {
                lock.clear(std::memory_order_release);
            }
        }
    } guard(m_stateLock);

    if (guard.acquired) {
        drainCommands();
    }
}

bool QuickLooper::isAudioThreadActive() const noexcept {
    if (m_isProcessingAudio.load(std::memory_order_acquire)) return true;
    auto audioId = m_audioThreadId.load(std::memory_order_relaxed);
    if (audioId == std::thread::id{}) return false;
    if (audioId == std::this_thread::get_id()) return false;
    auto lastNs = m_lastProcessTimeNs.load(std::memory_order_relaxed);
    if (lastNs == 0) return false;
    auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return (nowNs - lastNs) < 100'000'000LL;
}

void QuickLooper::executeCommand(LooperCommand cmd) noexcept {
    switch (cmd) {
        case LooperCommand::Clear: {
            m_state.store(LooperState::Empty, std::memory_order_relaxed);
            m_head.store(0, std::memory_order_relaxed);
            m_loopLength.store(0, std::memory_order_relaxed);
            m_undoLayerActive = false;
            m_canUndo.store(false, std::memory_order_relaxed);
            m_canRedo.store(false, std::memory_order_relaxed);
            break;
        }
        case LooperCommand::TriggerAction: {
            LooperState s = m_state.load(std::memory_order_relaxed);
            switch (s) {
                case LooperState::Empty:
                case LooperState::Stopped:
                    // Start recording fresh loop without zeroing buffer
                    m_head.store(0, std::memory_order_relaxed);
                    m_loopLength.store(0, std::memory_order_relaxed);
                    m_undoLayerActive = false;
                    m_canUndo.store(false, std::memory_order_relaxed);
                    m_canRedo.store(false, std::memory_order_relaxed);
                    m_state.store(LooperState::Recording, std::memory_order_relaxed);
                    break;

                case LooperState::Recording:
                    // Lock loop length and immediately start looping playback
                    if (m_head.load(std::memory_order_relaxed) > 1000) {
                        m_loopLength.store(m_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
                        m_head.store(0, std::memory_order_relaxed);
                        m_state.store(LooperState::Playing, std::memory_order_relaxed);
                    } else {
                        executeCommand(LooperCommand::Clear);
                    }
                    break;

                case LooperState::Playing: {
                    // Switch to Overdubbing: if an undone layer is active, stage its mask for lazy per-block commit
                    if (m_undoLayerActive) {
                        m_pendingCommitBlockMask = m_overdubbedBlockMask;
                        m_undoLayerActive = false;
                    } else {
                        std::fill(m_pendingCommitBlockMask.begin(), m_pendingCommitBlockMask.end(), 0ULL);
                    }
                    std::fill(m_overdubbedBlockMask.begin(), m_overdubbedBlockMask.end(), 0ULL);
                    m_canUndo.store(true, std::memory_order_relaxed);
                    m_canRedo.store(false, std::memory_order_relaxed);
                    m_state.store(LooperState::Overdubbing, std::memory_order_relaxed);
                    break;
                }

                case LooperState::Overdubbing:
                    // Switch back to Playing
                    m_state.store(LooperState::Playing, std::memory_order_relaxed);
                    break;
            }
            break;
        }
        case LooperCommand::Undo: {
            if (!m_canUndo.load(std::memory_order_relaxed)) break;
            if (m_state.load(std::memory_order_relaxed) == LooperState::Overdubbing) {
                m_state.store(LooperState::Playing, std::memory_order_relaxed);
            }
            // O(1) wait-free zero-copy layer switch (< 5 ns, 0 bytes copied on audio thread)
            m_undoLayerActive = true;
            m_canUndo.store(false, std::memory_order_relaxed);
            m_canRedo.store(true, std::memory_order_relaxed);
            break;
        }
        case LooperCommand::Redo: {
            if (!m_canRedo.load(std::memory_order_relaxed)) break;
            // O(1) wait-free zero-copy layer switch (< 5 ns, 0 bytes copied on audio thread)
            m_undoLayerActive = false;
            m_canUndo.store(true, std::memory_order_relaxed);
            m_canRedo.store(false, std::memory_order_relaxed);
            break;
        }
        case LooperCommand::Stop: {
            LooperState s = m_state.load(std::memory_order_relaxed);
            if (s != LooperState::Empty) {
                m_state.store(LooperState::Stopped, std::memory_order_relaxed);
            }
            break;
        }
        default:
            break;
    }
}

void QuickLooper::triggerAction() {
    enqueueCommand(LooperCommand::TriggerAction);
    if (!isAudioThreadActive()) {
        drainCommandsSafe();
    }
}

void QuickLooper::stop() {
    enqueueCommand(LooperCommand::Stop);
    if (!isAudioThreadActive()) {
        drainCommandsSafe();
    }
}

void QuickLooper::clear() {
    enqueueCommand(LooperCommand::Clear);
    if (!isAudioThreadActive()) {
        drainCommandsSafe();
    }
}

bool QuickLooper::undo() {
    if (!m_canUndo.load(std::memory_order_relaxed)) return false;
    enqueueCommand(LooperCommand::Undo);
    if (!isAudioThreadActive()) {
        drainCommandsSafe();
    }
    return true;
}

bool QuickLooper::redo() {
    if (!m_canRedo.load(std::memory_order_relaxed)) return false;
    enqueueCommand(LooperCommand::Redo);
    if (!isAudioThreadActive()) {
        drainCommandsSafe();
    }
    return true;
}

int QuickLooper::totalBeats() const noexcept {
    double sec = loopLengthSeconds();
    if (sec < 0.1) return 0;
    float bpmVal = m_bpm.load(std::memory_order_relaxed);
    if (bpmVal <= 0.0f) bpmVal = 120.0f;
    return std::max(1, static_cast<int>(std::round((sec * bpmVal) / 60.0)));
}

int QuickLooper::totalBars() const noexcept {
    int tb = totalBeats();
    if (tb == 0) return 0;
    int bpb = m_beatsPerBar.load(std::memory_order_relaxed);
    if (bpb <= 0) bpb = 4;
    return std::max(1, (tb + bpb - 1) / bpb);
}

int QuickLooper::currentBeat() const noexcept {
    int tb = totalBeats();
    if (tb == 0) return 1;
    float norm = playheadNormalized();
    int beatIdx = static_cast<int>(static_cast<double>(norm) * static_cast<double>(tb));
    int bpb = m_beatsPerBar.load(std::memory_order_relaxed);
    if (bpb <= 0) bpb = 4;
    return (beatIdx % bpb) + 1;
}

int QuickLooper::currentBar() const noexcept {
    int tb = totalBeats();
    if (tb == 0) return 1;
    float norm = playheadNormalized();
    int beatIdx = static_cast<int>(static_cast<double>(norm) * static_cast<double>(tb));
    int bpb = m_beatsPerBar.load(std::memory_order_relaxed);
    if (bpb <= 0) bpb = 4;
    return (beatIdx / bpb) + 1;
}

double QuickLooper::loopLengthSeconds() const noexcept {
    const double sr = m_sampleRate.load(std::memory_order_relaxed);
    if (!std::isfinite(sr) || sr <= 0.0) return 0.0;
    return static_cast<double>(m_loopLength.load(std::memory_order_relaxed)) / sr;
}

float QuickLooper::playheadNormalized() const noexcept {
    size_t len = m_loopLength.load(std::memory_order_relaxed);
    if (len == 0) return 0.0f;
    return static_cast<float>(static_cast<double>(m_head.load(std::memory_order_relaxed)) / static_cast<double>(len));
}

void QuickLooper::process(const audio::AudioBufferView& in, audio::AudioBufferView& out) {
    m_audioThreadId.store(std::this_thread::get_id(), std::memory_order_relaxed);
    m_lastProcessEpoch.fetch_add(1, std::memory_order_relaxed);
    auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_lastProcessTimeNs.store(nowNs, std::memory_order_relaxed);

    struct ProcessingGuard {
        std::atomic<bool>& flag;
        explicit ProcessingGuard(std::atomic<bool>& f) noexcept : flag(f) {
            flag.store(true, std::memory_order_release);
        }
        ~ProcessingGuard() noexcept {
            flag.store(false, std::memory_order_release);
        }
    } guard(m_isProcessingAudio);

    struct StateLockGuard {
        std::atomic_flag& lock;
        bool acquired{false};
        explicit StateLockGuard(std::atomic_flag& l) noexcept : lock(l) {
            for (int spin = 0; spin < 100; ++spin) {
                if (!lock.test_and_set(std::memory_order_acquire)) {
                    acquired = true;
                    break;
                }
#if defined(_M_X64) || defined(__x86_64__)
                _mm_pause();
#endif
            }
        }
        ~StateLockGuard() noexcept {
            if (acquired) {
                lock.clear(std::memory_order_release);
            }
        }
    } stateGuard(m_stateLock);

    if (!stateGuard.acquired) {
        if (out.numChannels() > 0 && in.numChannels() > 0 && out.channel(0) != in.channel(0)) {
            out.copyFrom(in);
        }
        return;
    }
    drainCommands();

    LooperState s = m_state.load(std::memory_order_relaxed);
    const size_t maxFrames = m_maxFrames.load(std::memory_order_relaxed);
    if (s == LooperState::Empty || s == LooperState::Stopped || maxFrames == 0) return;

    const uint32_t numSamples = out.numSamples();
    float* outL = out.channel(0);
    float* outR = (out.numChannels() > 1) ? out.channel(1) : outL;
    const float* inL = (in.numChannels() > 0) ? in.channel(0) : nullptr;
    const float* inR = (in.numChannels() > 1) ? in.channel(1) : inL;

    const float vol = m_volume.load(std::memory_order_relaxed);
    size_t curHead = m_head.load(std::memory_order_relaxed);
    size_t curLen = m_loopLength.load(std::memory_order_relaxed);

    if (s == LooperState::Recording) {
        for (uint32_t i = 0; i < numSamples; ++i) {
            if (curHead < maxFrames && curHead < m_loopBufferL.size()) {
                m_loopBufferL[curHead] = inL ? inL[i] : 0.0f;
                m_loopBufferR[curHead] = inR ? inR[i] : (inL ? inL[i] : 0.0f);
                curHead++;
            } else {
                m_loopLength.store(maxFrames, std::memory_order_relaxed);
                m_state.store(LooperState::Playing, std::memory_order_relaxed);
                curHead = 0;
                break;
            }
        }
    } else if (s == LooperState::Playing || s == LooperState::Overdubbing) {
        if (curLen == 0 || m_loopBufferL.empty()) return;

        for (uint32_t i = 0; i < numSamples; ++i) {
            if (curHead >= curLen) {
                curHead = 0;
            }

            const size_t blkIdx = curHead / kUndoBlockSize;
            const size_t wordIdx = blkIdx / 64;
            const uint64_t bitMask = 1ULL << (blkIdx % 64);
            const bool maskValid = (wordIdx < m_overdubbedBlockMask.size());

            // If a previous overdub was undone before starting a new overdub, lazily restore its 256-sample block first
            if (maskValid && (m_pendingCommitBlockMask[wordIdx] & bitMask) != 0ULL) {
                m_pendingCommitBlockMask[wordIdx] &= ~bitMask;
                const size_t blkStart = blkIdx * kUndoBlockSize;
                const size_t copyCount = std::min(kUndoBlockSize, m_loopBufferL.size() - blkStart);
                std::memcpy(&m_loopBufferL[blkStart], &m_undoBufferL[blkStart], copyCount * sizeof(float));
                std::memcpy(&m_loopBufferR[blkStart], &m_undoBufferR[blkStart], copyCount * sizeof(float));
            }

            const bool readFromUndo = m_undoLayerActive && maskValid && ((m_overdubbedBlockMask[wordIdx] & bitMask) != 0ULL);
            const float playL = readFromUndo ? m_undoBufferL[curHead] : m_loopBufferL[curHead];
            const float playR = readFromUndo ? m_undoBufferR[curHead] : m_loopBufferR[curHead];

            outL[i] += playL * vol;
            outR[i] += playR * vol;

            if (s == LooperState::Overdubbing) {
                if (maskValid && (m_overdubbedBlockMask[wordIdx] & bitMask) == 0ULL) {
                    m_overdubbedBlockMask[wordIdx] |= bitMask;
                    const size_t blkStart = blkIdx * kUndoBlockSize;
                    const size_t copyCount = std::min(kUndoBlockSize, m_loopBufferL.size() - blkStart);
                    std::memcpy(&m_undoBufferL[blkStart], &m_loopBufferL[blkStart], copyCount * sizeof(float));
                    std::memcpy(&m_undoBufferR[blkStart], &m_loopBufferR[blkStart], copyCount * sizeof(float));
                }
                float addL = inL ? inL[i] : 0.0f;
                float addR = inR ? inR[i] : addL;
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
        praccy::utils::WavAudioData wavData;
        if (!praccy::utils::WavReader::loadWavFile(filePath, wavData) || wavData.numFrames == 0) {
            return false;
        }

        if (m_maxFrames.load(std::memory_order_relaxed) == 0) {
            const double srInit = m_sampleRate.load(std::memory_order_relaxed);
            prepare(srInit > 0.0 ? srInit : 48000.0, 60);
        }
        const double sr = m_sampleRate.load(std::memory_order_relaxed);
        const size_t maxFrames = m_maxFrames.load(std::memory_order_relaxed);

        std::vector<float> finalL;
        std::vector<float> finalR;
        if (wavData.sampleRate != static_cast<uint32_t>(sr) && wavData.sampleRate >= utils::WavReader::kMinSampleRate) {
            double ratio = sr / static_cast<double>(wavData.sampleRate);
            size_t targetFrames = std::min(maxFrames, static_cast<size_t>(static_cast<double>(wavData.numFrames) * ratio));
            if (targetFrames == 0) return false;
            finalL.resize(targetFrames);
            finalR.resize(targetFrames);

            for (size_t i = 0; i < targetFrames; ++i) {
                double srcIdx = static_cast<double>(i) / ratio;
                size_t idx0 = std::min(static_cast<size_t>(srcIdx), wavData.numFrames - 1);
                size_t idx1 = std::min(idx0 + 1, wavData.numFrames - 1);
                float frac = static_cast<float>(srcIdx - static_cast<double>(idx0));

                finalL[i] = wavData.samplesL[idx0] * (1.0f - frac) + wavData.samplesL[idx1] * frac;
                finalR[i] = wavData.samplesR[idx0] * (1.0f - frac) + wavData.samplesR[idx1] * frac;
            }
        } else {
            finalL = std::move(wavData.samplesL);
            finalR = std::move(wavData.samplesR);
        }

        size_t copyFrames = std::min(finalL.size(), maxFrames);
        if (copyFrames == 0) return false;

        finalL.resize(maxFrames, 0.0f);
        finalR.resize(maxFrames, 0.0f);

        {
            struct StateLockSpinGuard {
                std::atomic_flag& lock;
                explicit StateLockSpinGuard(std::atomic_flag& l) noexcept : lock(l) {
                    while (lock.test_and_set(std::memory_order_acquire)) {
#if defined(_M_X64) || defined(__x86_64__)
                        _mm_pause();
#endif
                    }
                }
                ~StateLockSpinGuard() noexcept {
                    lock.clear(std::memory_order_release);
                }
            } swapGuard(m_stateLock);

            m_loopBufferL.swap(finalL);
            m_loopBufferR.swap(finalR);
            m_undoLayerActive = false;
            std::fill(m_overdubbedBlockMask.begin(), m_overdubbedBlockMask.end(), 0ULL);
            std::fill(m_pendingCommitBlockMask.begin(), m_pendingCommitBlockMask.end(), 0ULL);
            m_loopLength.store(copyFrames, std::memory_order_relaxed);
            m_head.store(0, std::memory_order_relaxed);
            m_state.store(LooperState::Stopped, std::memory_order_relaxed);
            m_canUndo.store(false, std::memory_order_relaxed);
            m_canRedo.store(false, std::memory_order_relaxed);
        }

        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

} // namespace praccy::tools
