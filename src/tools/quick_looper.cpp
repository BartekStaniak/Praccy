#include "quick_looper.h"
#include "../utils/wav_reader.h"
#include <fstream>
#include <cstdint>
#include <cmath>
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

    m_sampleRate = sampleRate;
    m_maxFrames = static_cast<size_t>(sampleRate * static_cast<double>(maxSeconds));
    try {
        m_loopBufferL.assign(m_maxFrames, 0.0f);
        m_loopBufferR.assign(m_maxFrames, 0.0f);
        m_undoBufferL.assign(m_maxFrames, 0.0f);
        m_undoBufferR.assign(m_maxFrames, 0.0f);
        m_redoBufferL.assign(m_maxFrames, 0.0f);
        m_redoBufferR.assign(m_maxFrames, 0.0f);
    } catch (const std::bad_alloc&) {
        m_maxFrames = 0;
        m_loopBufferL.clear();
        m_loopBufferR.clear();
        m_undoBufferL.clear();
        m_undoBufferR.clear();
        m_redoBufferL.clear();
        m_redoBufferR.clear();
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

void QuickLooper::drainCommands() {
    LooperCommand cmd;
    while (popCommand(cmd)) {
        executeCommand(cmd);
    }
}

void QuickLooper::drainCommandsSafe() {
    struct StateLockSpinGuard {
        std::atomic_flag& lock;
        bool acquired{false};
        explicit StateLockSpinGuard(std::atomic_flag& l) : lock(l) {
            for (int spin = 0; spin < 1000; ++spin) {
                if (!lock.test_and_set(std::memory_order_acquire)) {
                    acquired = true;
                    break;
                }
#if defined(_M_X64) || defined(__x86_64__)
                _mm_pause();
#endif
            }
        }
        ~StateLockSpinGuard() {
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
    auto audioId = m_audioThreadId.load(std::memory_order_relaxed);
    if (audioId == std::thread::id{}) return false;
    if (audioId == std::this_thread::get_id()) return false;
    auto lastNs = m_lastProcessTimeNs.load(std::memory_order_relaxed);
    if (lastNs == 0) return false;
    auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return (nowNs - lastNs) < 100'000'000LL;
}

void QuickLooper::executeCommand(LooperCommand cmd) {
    switch (cmd) {
        case LooperCommand::Clear: {
            m_state.store(LooperState::Empty, std::memory_order_relaxed);
            m_head.store(0, std::memory_order_relaxed);
            m_loopLength.store(0, std::memory_order_relaxed);
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
                    // Switch to Overdubbing: mark undo ready (samples backed up sample-by-sample in process)
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
            // O(1) buffer swaps
            m_loopBufferL.swap(m_undoBufferL);
            m_loopBufferR.swap(m_undoBufferR);
            m_canUndo.store(false, std::memory_order_relaxed);
            m_canRedo.store(true, std::memory_order_relaxed);
            break;
        }
        case LooperCommand::Redo: {
            if (!m_canRedo.load(std::memory_order_relaxed)) break;
            // O(1) buffer swaps back
            m_loopBufferL.swap(m_undoBufferL);
            m_loopBufferR.swap(m_undoBufferR);
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
    if (!std::isfinite(m_sampleRate) || m_sampleRate <= 0.0) return 0.0;
    return static_cast<double>(m_loopLength.load(std::memory_order_relaxed)) / m_sampleRate;
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
        explicit StateLockGuard(std::atomic_flag& l) : lock(l) {
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
        ~StateLockGuard() {
            if (acquired) {
                lock.clear(std::memory_order_release);
            }
        }
    } stateGuard(m_stateLock);

    if (stateGuard.acquired) {
        drainCommands();
    }

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

            // If overdubbing, copy pre-overdub sample sample-by-sample and accumulate incoming guitar
            if (s == LooperState::Overdubbing) {
                if (curHead < m_undoBufferL.size() && curHead < m_undoBufferR.size()) {
                    m_undoBufferL[curHead] = m_loopBufferL[curHead];
                    m_undoBufferR[curHead] = m_loopBufferR[curHead];
                }
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
        praccy::utils::WavAudioData wavData;
        if (!praccy::utils::WavReader::loadWavFile(filePath, wavData) || wavData.numFrames == 0) {
            return false;
        }

        if (m_maxFrames == 0) {
            prepare(m_sampleRate > 0.0 ? m_sampleRate : 48000.0, 60);
        }

        // Resample if needed
        std::vector<float> finalL;
        std::vector<float> finalR;
        if (wavData.sampleRate != static_cast<uint32_t>(m_sampleRate) && wavData.sampleRate > 0) {
            double ratio = m_sampleRate / static_cast<double>(wavData.sampleRate);
            size_t targetFrames = static_cast<size_t>(static_cast<double>(wavData.numFrames) * ratio);
            finalL.resize(targetFrames);
            finalR.resize(targetFrames);

            for (size_t i = 0; i < targetFrames; ++i) {
                double srcIdx = static_cast<double>(i) / ratio;
                size_t idx0 = static_cast<size_t>(srcIdx);
                size_t idx1 = std::min(idx0 + 1, wavData.numFrames - 1);
                float frac = static_cast<float>(srcIdx - static_cast<double>(idx0));

                finalL[i] = wavData.samplesL[idx0] * (1.0f - frac) + wavData.samplesL[idx1] * frac;
                finalR[i] = wavData.samplesR[idx0] * (1.0f - frac) + wavData.samplesR[idx1] * frac;
            }
        } else {
            finalL = std::move(wavData.samplesL);
            finalR = std::move(wavData.samplesR);
        }

        size_t copyFrames = std::min(finalL.size(), m_maxFrames);
        if (copyFrames == 0) return false;

        // Ensure finalL and finalR are sized to m_maxFrames on caller thread
        finalL.resize(m_maxFrames, 0.0f);
        finalR.resize(m_maxFrames, 0.0f);

        // Hand over to the looper under m_stateLock
        {
            struct StateLockSpinGuard {
                std::atomic_flag& lock;
                explicit StateLockSpinGuard(std::atomic_flag& l) : lock(l) {
                    while (lock.test_and_set(std::memory_order_acquire)) {
#if defined(_M_X64) || defined(__x86_64__)
                        _mm_pause();
#endif
                    }
                }
                ~StateLockSpinGuard() {
                    lock.clear(std::memory_order_release);
                }
            } swapGuard(m_stateLock);

            m_loopBufferL.swap(finalL);
            m_loopBufferR.swap(finalR);
            m_loopLength.store(copyFrames, std::memory_order_relaxed);
            m_head.store(0, std::memory_order_relaxed);
            m_state.store(LooperState::Stopped, std::memory_order_relaxed);
            m_canUndo.store(false, std::memory_order_relaxed);
            m_canRedo.store(false, std::memory_order_relaxed);
        }

        // finalL and finalR (holding old buffers) are deallocated here outside audio callback
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

} // namespace praccy::tools
