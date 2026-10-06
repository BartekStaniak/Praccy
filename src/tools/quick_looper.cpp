#include "quick_looper.h"
#include <cstring>

namespace praccy::tools {

QuickLooper::QuickLooper() {
    prepare(48000.0, 60);
}

void QuickLooper::prepare(double sampleRate, uint32_t maxSeconds) {
    m_sampleRate = sampleRate;
    m_maxFrames = static_cast<size_t>(sampleRate * maxSeconds);
    m_loopBufferL.assign(m_maxFrames, 0.0f);
    m_loopBufferR.assign(m_maxFrames, 0.0f);
    clear();
}

void QuickLooper::triggerAction() {
    LooperState s = m_state.load(std::memory_order_relaxed);
    switch (s) {
        case LooperState::Empty:
        case LooperState::Stopped:
            // Start recording fresh loop
            m_head.store(0, std::memory_order_relaxed);
            m_loopLength.store(0, std::memory_order_relaxed);
            std::fill(m_loopBufferL.begin(), m_loopBufferL.end(), 0.0f);
            std::fill(m_loopBufferR.begin(), m_loopBufferR.end(), 0.0f);
            m_state.store(LooperState::Recording, std::memory_order_relaxed);
            break;

        case LooperState::Recording:
            // Lock loop length and immediately start looping playback
            if (m_head.load(std::memory_order_relaxed) > 1000) {
                m_loopLength.store(m_head.load(std::memory_order_relaxed), std::memory_order_relaxed);
                m_head.store(0, std::memory_order_relaxed);
                m_state.store(LooperState::Playing, std::memory_order_relaxed);
            } else {
                clear();
            }
            break;

        case LooperState::Playing:
            // Switch to Overdubbing
            m_state.store(LooperState::Overdubbing, std::memory_order_relaxed);
            break;

        case LooperState::Overdubbing:
            // Switch back to Playing
            m_state.store(LooperState::Playing, std::memory_order_relaxed);
            break;
    }
}

void QuickLooper::stop() {
    LooperState s = m_state.load(std::memory_order_relaxed);
    if (s != LooperState::Empty) {
        m_state.store(LooperState::Stopped, std::memory_order_relaxed);
    }
}

void QuickLooper::clear() {
    m_state.store(LooperState::Empty, std::memory_order_relaxed);
    m_head.store(0, std::memory_order_relaxed);
    m_loopLength.store(0, std::memory_order_relaxed);
    std::fill(m_loopBufferL.begin(), m_loopBufferL.end(), 0.0f);
    std::fill(m_loopBufferR.begin(), m_loopBufferR.end(), 0.0f);
}

double QuickLooper::loopLengthSeconds() const noexcept {
    if (m_sampleRate <= 0.0) return 0.0;
    return m_loopLength.load(std::memory_order_relaxed) / m_sampleRate;
}

float QuickLooper::playheadNormalized() const noexcept {
    size_t len = m_loopLength.load(std::memory_order_relaxed);
    if (len == 0) return 0.0f;
    return static_cast<float>(m_head.load(std::memory_order_relaxed)) / static_cast<float>(len);
}

void QuickLooper::process(const audio::AudioBufferView& in, audio::AudioBufferView& out) {
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

} // namespace praccy::tools
