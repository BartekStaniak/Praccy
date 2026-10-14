#include "quick_looper.h"
#include "../utils/wav_reader.h"
#include <cstring>
#include <fstream>
#include <cstdint>
#include <cmath>
#include <filesystem>


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
    clear();
}

void QuickLooper::executeCommand(LooperCommand cmd) {
    switch (cmd) {
        case LooperCommand::Clear: {
            m_state.store(LooperState::Empty, std::memory_order_relaxed);
            m_head.store(0, std::memory_order_relaxed);
            m_loopLength.store(0, std::memory_order_relaxed);
            m_canUndo.store(false, std::memory_order_relaxed);
            m_canRedo.store(false, std::memory_order_relaxed);
            std::fill(m_loopBufferL.begin(), m_loopBufferL.end(), 0.0f);
            std::fill(m_loopBufferR.begin(), m_loopBufferR.end(), 0.0f);
            break;
        }
        case LooperCommand::TriggerAction: {
            LooperState s = m_state.load(std::memory_order_relaxed);
            switch (s) {
                case LooperState::Empty:
                case LooperState::Stopped:
                    // Start recording fresh loop
                    m_head.store(0, std::memory_order_relaxed);
                    m_loopLength.store(0, std::memory_order_relaxed);
                    std::fill(m_loopBufferL.begin(), m_loopBufferL.end(), 0.0f);
                    std::fill(m_loopBufferR.begin(), m_loopBufferR.end(), 0.0f);
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
                    // Switch to Overdubbing: snapshot current layer so it can be undone
                    size_t len = m_loopLength.load(std::memory_order_relaxed);
                    if (len > 0 && len <= m_maxFrames && len <= m_undoBufferL.size()) {
                        std::memcpy(m_undoBufferL.data(), m_loopBufferL.data(), len * sizeof(float));
                        std::memcpy(m_undoBufferR.data(), m_loopBufferR.data(), len * sizeof(float));
                        m_canUndo.store(true, std::memory_order_relaxed);
                        m_canRedo.store(false, std::memory_order_relaxed);
                    }
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
            size_t len = m_loopLength.load(std::memory_order_relaxed);
            if (len == 0 || len > m_maxFrames || len > m_undoBufferL.size() || len > m_redoBufferL.size()) break;

            // Save current loop state into redo buffer
            std::memcpy(m_redoBufferL.data(), m_loopBufferL.data(), len * sizeof(float));
            std::memcpy(m_redoBufferR.data(), m_loopBufferR.data(), len * sizeof(float));

            // Restore undo buffer into loop buffer
            std::memcpy(m_loopBufferL.data(), m_undoBufferL.data(), len * sizeof(float));
            std::memcpy(m_loopBufferR.data(), m_undoBufferR.data(), len * sizeof(float));

            m_canUndo.store(false, std::memory_order_relaxed);
            m_canRedo.store(true, std::memory_order_relaxed);
            break;
        }
        case LooperCommand::Redo: {
            if (!m_canRedo.load(std::memory_order_relaxed)) break;
            size_t len = m_loopLength.load(std::memory_order_relaxed);
            if (len == 0 || len > m_maxFrames || len > m_undoBufferL.size() || len > m_redoBufferL.size()) break;

            // Save current into undo buffer
            std::memcpy(m_undoBufferL.data(), m_loopBufferL.data(), len * sizeof(float));
            std::memcpy(m_undoBufferR.data(), m_loopBufferR.data(), len * sizeof(float));

            // Restore redo buffer into loop buffer
            std::memcpy(m_loopBufferL.data(), m_redoBufferL.data(), len * sizeof(float));
            std::memcpy(m_loopBufferR.data(), m_redoBufferR.data(), len * sizeof(float));

            m_canUndo.store(true, std::memory_order_relaxed);
            m_canRedo.store(false, std::memory_order_relaxed);
            break;
        }
        default:
            break;
    }
}

void QuickLooper::triggerAction() {
    if (m_isProcessingAudio.load(std::memory_order_acquire)) {
        m_pendingCommand.store(LooperCommand::TriggerAction, std::memory_order_release);
    } else {
        executeCommand(LooperCommand::TriggerAction);
    }
}

void QuickLooper::stop() {
    LooperState s = m_state.load(std::memory_order_relaxed);
    if (s != LooperState::Empty) {
        m_state.store(LooperState::Stopped, std::memory_order_relaxed);
    }
}

void QuickLooper::clear() {
    if (m_isProcessingAudio.load(std::memory_order_acquire)) {
        m_pendingCommand.store(LooperCommand::Clear, std::memory_order_release);
    } else {
        executeCommand(LooperCommand::Clear);
    }
}

bool QuickLooper::undo() {
    if (!m_canUndo.load(std::memory_order_relaxed)) return false;
    if (m_isProcessingAudio.load(std::memory_order_acquire)) {
        m_pendingCommand.store(LooperCommand::Undo, std::memory_order_release);
        return true;
    } else {
        executeCommand(LooperCommand::Undo);
        return true;
    }
}

bool QuickLooper::redo() {
    if (!m_canRedo.load(std::memory_order_relaxed)) return false;
    if (m_isProcessingAudio.load(std::memory_order_acquire)) {
        m_pendingCommand.store(LooperCommand::Redo, std::memory_order_release);
        return true;
    } else {
        executeCommand(LooperCommand::Redo);
        return true;
    }
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
    int beatIdx = static_cast<int>(norm * static_cast<float>(tb));
    int bpb = m_beatsPerBar.load(std::memory_order_relaxed);
    if (bpb <= 0) bpb = 4;
    return (beatIdx % bpb) + 1;
}

int QuickLooper::currentBar() const noexcept {
    int tb = totalBeats();
    if (tb == 0) return 1;
    float norm = playheadNormalized();
    int beatIdx = static_cast<int>(norm * static_cast<float>(tb));
    int bpb = m_beatsPerBar.load(std::memory_order_relaxed);
    if (bpb <= 0) bpb = 4;
    return (beatIdx / bpb) + 1;
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
    struct ProcessingGuard {
        std::atomic<bool>& flag;
        explicit ProcessingGuard(std::atomic<bool>& f) noexcept : flag(f) {
            flag.store(true, std::memory_order_release);
        }
        ~ProcessingGuard() noexcept {
            flag.store(false, std::memory_order_release);
        }
    } guard(m_isProcessingAudio);

    LooperCommand cmd = m_pendingCommand.exchange(LooperCommand::None, std::memory_order_acq_rel);
    if (cmd != LooperCommand::None) {
        executeCommand(cmd);
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
            size_t targetFrames = static_cast<size_t>(wavData.numFrames * ratio);
            finalL.resize(targetFrames);
            finalR.resize(targetFrames);

            for (size_t i = 0; i < targetFrames; ++i) {
                double srcIdx = i / ratio;
                size_t idx0 = static_cast<size_t>(srcIdx);
                size_t idx1 = std::min(idx0 + 1, wavData.numFrames - 1);
                float frac = static_cast<float>(srcIdx - idx0);

                finalL[i] = wavData.samplesL[idx0] * (1.0f - frac) + wavData.samplesL[idx1] * frac;
                finalR[i] = wavData.samplesR[idx0] * (1.0f - frac) + wavData.samplesR[idx1] * frac;
            }
        } else {
            finalL = std::move(wavData.samplesL);
            finalR = std::move(wavData.samplesR);
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
