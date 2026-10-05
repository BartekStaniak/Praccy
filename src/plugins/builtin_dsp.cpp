#include "builtin_dsp.h"
#include <algorithm>

namespace praccy::plugins {

// ==========================================
// OverdriveEffect Implementation
// ==========================================

OverdriveEffect::OverdriveEffect() = default;

void OverdriveEffect::prepare(double sampleRate, uint32_t maxBlockSize) {
    reset();
}

void OverdriveEffect::reset() {
    m_filterState[0] = 0.0f;
    m_filterState[1] = 0.0f;
}

PluginParameterDesc OverdriveEffect::getParameterDesc(size_t index) const {
    switch (index) {
        case 0: return {0, "Drive", 1.0f, 15.0f, 4.0f, m_drive};
        case 1: return {1, "Tone", 0.0f, 1.0f, 0.5f, m_tone};
        case 2: return {2, "Level", 0.0f, 2.0f, 1.0f, m_level};
        default: return {};
    }
}

void OverdriveEffect::setParameterValue(uint32_t paramId, float value) {
    switch (paramId) {
        case 0: m_drive = std::clamp(value, 1.0f, 15.0f); break;
        case 1: m_tone = std::clamp(value, 0.0f, 1.0f); break;
        case 2: m_level = std::clamp(value, 0.0f, 2.0f); break;
        default: break;
    }
}

float OverdriveEffect::getParameterValue(uint32_t paramId) const {
    switch (paramId) {
        case 0: return m_drive;
        case 1: return m_tone;
        case 2: return m_level;
        default: return 0.0f;
    }
}

void OverdriveEffect::process(audio::AudioProcessContext& ctx) {
    const uint32_t numSamples = ctx.numSamples;
    const uint32_t numCh = ctx.output.numChannels();

    const float drive = m_drive;
    const float level = m_level;
    const float alpha = 0.2f + (m_tone * 0.6f);

    for (uint32_t ch = 0; ch < numCh; ++ch) {
        const float* in = ctx.input.channel(ch);
        float* out = ctx.output.channel(ch);
        float state = m_filterState[ch % 2];

        for (uint32_t s = 0; s < numSamples; ++s) {
            float x = in[s] * drive;
            // Asymmetric soft saturation curve (tube-like)
            float clipped = (x > 0.0f) ? std::tanh(x) : (x / (1.0f - x * 0.5f));
            // 1-pole tone filter
            state += alpha * (clipped - state);
            out[s] = state * level;
        }
        m_filterState[ch % 2] = state;
    }
}

// ==========================================
// TubeAmpEffect Implementation
// ==========================================

TubeAmpEffect::TubeAmpEffect() = default;

void TubeAmpEffect::prepare(double sampleRate, uint32_t maxBlockSize) {
    reset();
}

void TubeAmpEffect::reset() {
    m_cabFilterL = 0.0f;
    m_cabFilterR = 0.0f;
}

PluginParameterDesc TubeAmpEffect::getParameterDesc(size_t index) const {
    switch (index) {
        case 0: return {0, "Gain", 1.0f, 20.0f, 6.0f, m_gain};
        case 1: return {1, "Bass", 0.0f, 1.0f, 0.5f, m_bass};
        case 2: return {2, "Treble", 0.0f, 1.0f, 0.5f, m_treble};
        case 3: return {3, "Master", 0.0f, 2.0f, 0.8f, m_master};
        default: return {};
    }
}

void TubeAmpEffect::setParameterValue(uint32_t paramId, float value) {
    switch (paramId) {
        case 0: m_gain = std::clamp(value, 1.0f, 20.0f); break;
        case 1: m_bass = std::clamp(value, 0.0f, 1.0f); break;
        case 2: m_treble = std::clamp(value, 0.0f, 1.0f); break;
        case 3: m_master = std::clamp(value, 0.0f, 2.0f); break;
        default: break;
    }
}

float TubeAmpEffect::getParameterValue(uint32_t paramId) const {
    switch (paramId) {
        case 0: return m_gain;
        case 1: return m_bass;
        case 2: return m_treble;
        case 3: return m_master;
        default: return 0.0f;
    }
}

void TubeAmpEffect::process(audio::AudioProcessContext& ctx) {
    const uint32_t numSamples = ctx.numSamples;
    const uint32_t numCh = ctx.output.numChannels();

    const float gain = m_gain;
    const float master = m_master;
    // Cabinet low-pass cutoff coefficient (~4.5 kHz roll-off)
    constexpr float cabAlpha = 0.35f;

    for (uint32_t ch = 0; ch < numCh; ++ch) {
        const float* in = ctx.input.channel(ch);
        float* out = ctx.output.channel(ch);
        float& cab = (ch == 0) ? m_cabFilterL : m_cabFilterR;

        for (uint32_t s = 0; s < numSamples; ++s) {
            float x = in[s] * gain;
            // Multi-stage non-linear tube curve
            float stage1 = std::tanh(x * 1.5f);
            float stage2 = std::tanh(stage1 * 1.2f - (stage1 * stage1 * 0.15f));
            // Built-in 4x12 cabinet simulation filter
            cab += cabAlpha * (stage2 - cab);
            out[s] = cab * master;
        }
    }
}

// ==========================================
// StereoDelayEffect Implementation
// ==========================================

StereoDelayEffect::StereoDelayEffect() = default;

void StereoDelayEffect::prepare(double sampleRate, uint32_t maxBlockSize) {
    m_sampleRate = sampleRate;
    // Max 2.0 seconds delay
    const size_t maxDelaySamples = static_cast<size_t>(sampleRate * 2.0);
    m_delayBufferL.assign(maxDelaySamples, 0.0f);
    m_delayBufferR.assign(maxDelaySamples, 0.0f);
    reset();
}

void StereoDelayEffect::reset() {
    std::fill(m_delayBufferL.begin(), m_delayBufferL.end(), 0.0f);
    std::fill(m_delayBufferR.begin(), m_delayBufferR.end(), 0.0f);
    m_writePos = 0;
}

PluginParameterDesc StereoDelayEffect::getParameterDesc(size_t index) const {
    switch (index) {
        case 0: return {0, "Time (ms)", 20.0f, 1500.0f, 350.0f, m_timeMs};
        case 1: return {1, "Feedback", 0.0f, 0.95f, 0.45f, m_feedback};
        case 2: return {2, "Mix", 0.0f, 1.0f, 0.35f, m_mix};
        default: return {};
    }
}

void StereoDelayEffect::setParameterValue(uint32_t paramId, float value) {
    switch (paramId) {
        case 0: m_timeMs = std::clamp(value, 20.0f, 1500.0f); break;
        case 1: m_feedback = std::clamp(value, 0.0f, 0.95f); break;
        case 2: m_mix = std::clamp(value, 0.0f, 1.0f); break;
        default: break;
    }
}

float StereoDelayEffect::getParameterValue(uint32_t paramId) const {
    switch (paramId) {
        case 0: return m_timeMs;
        case 1: return m_feedback;
        case 2: return m_mix;
        default: return 0.0f;
    }
}

void StereoDelayEffect::process(audio::AudioProcessContext& ctx) {
    if (m_delayBufferL.empty()) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    const uint32_t numSamples = ctx.numSamples;
    const uint32_t bufSize = static_cast<uint32_t>(m_delayBufferL.size());
    const uint32_t delaySamplesL = static_cast<uint32_t>((m_timeMs / 1000.0f) * m_sampleRate);
    // Ping-pong delay: right channel slightly offset (3/4 time)
    const uint32_t delaySamplesR = static_cast<uint32_t>(delaySamplesL * 0.75f);

    const float fb = m_feedback;
    const float mix = m_mix;

    const float* inL = ctx.input.channel(0);
    const float* inR = ctx.input.numChannels() > 1 ? ctx.input.channel(1) : inL;
    float* outL = ctx.output.channel(0);
    float* outR = ctx.output.numChannels() > 1 ? ctx.output.channel(1) : outL;

    for (uint32_t s = 0; s < numSamples; ++s) {
        const uint32_t readPosL = (m_writePos + bufSize - (delaySamplesL % bufSize)) % bufSize;
        const uint32_t readPosR = (m_writePos + bufSize - (delaySamplesR % bufSize)) % bufSize;

        const float delayedL = m_delayBufferL[readPosL];
        const float delayedR = m_delayBufferR[readPosR];

        m_delayBufferL[m_writePos] = inL[s] + (delayedR * fb); // Cross-feedback ping-pong
        m_delayBufferR[m_writePos] = inR[s] + (delayedL * fb);

        outL[s] = (inL[s] * (1.0f - mix)) + (delayedL * mix);
        outR[s] = (inR[s] * (1.0f - mix)) + (delayedR * mix);

        m_writePos = (m_writePos + 1) % bufSize;
    }
}

} // namespace praccy::plugins
