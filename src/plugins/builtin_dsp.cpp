#include "builtin_dsp.h"
#include <algorithm>
#include <cstring>

namespace praccy::plugins {

// ==========================================
// OverdriveEffect Implementation
// ==========================================

OverdriveEffect::OverdriveEffect() = default;

void OverdriveEffect::prepare(double sampleRate, uint32_t /*maxBlockSize*/) {
    m_sampleRate = (sampleRate > 8000.0) ? sampleRate : 48000.0;
    reset();
}

void OverdriveEffect::reset() {
    m_filterState[0] = 0.0f;
    m_filterState[1] = 0.0f;
}

PluginParameterDesc OverdriveEffect::getParameterDesc(size_t index) const {
    switch (index) {
        case 0: return {0, "Drive", 1.0f, 15.0f, 4.0f, m_drive.load(std::memory_order_relaxed)};
        case 1: return {1, "Tone", 0.0f, 1.0f, 0.5f, m_tone.load(std::memory_order_relaxed)};
        case 2: return {2, "Level", 0.0f, 2.0f, 1.0f, m_level.load(std::memory_order_relaxed)};
        default: return {};
    }
}

void OverdriveEffect::setParameterValue(uint32_t paramId, float value) {
    switch (paramId) {
        case 0: m_drive.store(std::clamp(value, 1.0f, 15.0f), std::memory_order_relaxed); break;
        case 1: m_tone.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case 2: m_level.store(std::clamp(value, 0.0f, 2.0f), std::memory_order_relaxed); break;
        default: break;
    }
}

float OverdriveEffect::getParameterValue(uint32_t paramId) const {
    switch (paramId) {
        case 0: return m_drive.load(std::memory_order_relaxed);
        case 1: return m_tone.load(std::memory_order_relaxed);
        case 2: return m_level.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void OverdriveEffect::process(audio::AudioProcessContext& ctx) {
    const uint32_t numSamples = ctx.numSamples;
    const uint32_t numCh = ctx.output.numChannels();

    const float drive = m_drive.load(std::memory_order_relaxed);
    const float tone  = m_tone.load(std::memory_order_relaxed);
    const float level = m_level.load(std::memory_order_relaxed);
    const float sr    = static_cast<float>((ctx.sampleRate > 8000.0) ? ctx.sampleRate : m_sampleRate);
    // Sample-rate-invariant 1-pole tone filter (cutoff 1.2 kHz to 9.5 kHz)
    const float cutoffHz = 1200.0f + (tone * 8300.0f);
    const float alpha = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * cutoffHz / sr);

    for (uint32_t ch = 0; ch < numCh; ++ch) {
        const float* in = ctx.input.channel(ch);
        float* out = ctx.output.channel(ch);
        float state = m_filterState[ch % 2];

        for (uint32_t s = 0; s < numSamples; ++s) {
            float x = in[s] * drive;
            // Asymmetric soft saturation curve (tube-like)
            float clipped = (x > 0.0f) ? audio::DspUtils::fastTanh(x) : (x / (1.0f - x * 0.5f));
            // 1-pole tone filter
            state += alpha * (clipped - state);
            out[s] = state * level;
        }
        m_filterState[ch % 2] = state;
    }
}

std::vector<uint8_t> OverdriveEffect::saveState() const {
    std::vector<uint8_t> data(3 * sizeof(float));
    float vals[3] = {
        m_drive.load(std::memory_order_relaxed),
        m_tone.load(std::memory_order_relaxed),
        m_level.load(std::memory_order_relaxed)
    };
    std::memcpy(data.data(), vals, sizeof(vals));
    return data;
}

bool OverdriveEffect::loadState(const std::vector<uint8_t>& state) {
    if (state.size() < 3 * sizeof(float)) return false;
    float vals[3];
    std::memcpy(vals, state.data(), sizeof(vals));
    setParameterValue(0, vals[0]);
    setParameterValue(1, vals[1]);
    setParameterValue(2, vals[2]);
    return true;
}

// ==========================================
// TubeAmpEffect Implementation
// ==========================================

TubeAmpEffect::TubeAmpEffect() = default;

void TubeAmpEffect::prepare(double sampleRate, uint32_t /*maxBlockSize*/) {
    m_sampleRate = (sampleRate > 8000.0) ? sampleRate : 48000.0;
    reset();
}

void TubeAmpEffect::reset() {
    m_bassFilterL = 0.0f;
    m_bassFilterR = 0.0f;
    m_cabFilterL = 0.0f;
    m_cabFilterR = 0.0f;
}

PluginParameterDesc TubeAmpEffect::getParameterDesc(size_t index) const {
    switch (index) {
        case 0: return {0, "Gain", 1.0f, 20.0f, 6.0f, m_gain.load(std::memory_order_relaxed)};
        case 1: return {1, "Bass", 0.0f, 1.0f, 0.5f, m_bass.load(std::memory_order_relaxed)};
        case 2: return {2, "Treble", 0.0f, 1.0f, 0.5f, m_treble.load(std::memory_order_relaxed)};
        case 3: return {3, "Master", 0.0f, 2.0f, 0.8f, m_master.load(std::memory_order_relaxed)};
        default: return {};
    }
}

void TubeAmpEffect::setParameterValue(uint32_t paramId, float value) {
    switch (paramId) {
        case 0: m_gain.store(std::clamp(value, 1.0f, 20.0f), std::memory_order_relaxed); break;
        case 1: m_bass.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case 2: m_treble.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        case 3: m_master.store(std::clamp(value, 0.0f, 2.0f), std::memory_order_relaxed); break;
        default: break;
    }
}

float TubeAmpEffect::getParameterValue(uint32_t paramId) const {
    switch (paramId) {
        case 0: return m_gain.load(std::memory_order_relaxed);
        case 1: return m_bass.load(std::memory_order_relaxed);
        case 2: return m_treble.load(std::memory_order_relaxed);
        case 3: return m_master.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

void TubeAmpEffect::process(audio::AudioProcessContext& ctx) {
    const uint32_t numSamples = ctx.numSamples;
    const uint32_t numCh = ctx.output.numChannels();

    const float gain   = m_gain.load(std::memory_order_relaxed);
    const float bass   = m_bass.load(std::memory_order_relaxed);
    const float treble = m_treble.load(std::memory_order_relaxed);
    const float master = m_master.load(std::memory_order_relaxed);
    const float sr     = static_cast<float>((ctx.sampleRate > 8000.0) ? ctx.sampleRate : m_sampleRate);

    // Sample-rate-invariant 2-band tone stack & 4x12 cabinet roll-off
    const float bassAlpha = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * 240.0f / sr);
    const float bassShelf = (bass - 0.5f) * 1.2f;
    const float cabCutoff = 2200.0f + (treble * 4600.0f);
    const float cabAlpha  = 1.0f - std::exp(-2.0f * std::numbers::pi_v<float> * cabCutoff / sr);

    for (uint32_t ch = 0; ch < numCh; ++ch) {
        const float* in = ctx.input.channel(ch);
        float* out = ctx.output.channel(ch);
        float& bassLp = (ch == 0) ? m_bassFilterL : m_bassFilterR;
        float& cab    = (ch == 0) ? m_cabFilterL  : m_cabFilterR;

        for (uint32_t s = 0; s < numSamples; ++s) {
            float x = in[s] * gain;
            // Multi-stage non-linear tube curve
            float stage1 = audio::DspUtils::fastTanh(x * 1.5f);
            float stage2 = audio::DspUtils::fastTanh(stage1 * 1.2f - (stage1 * stage1 * 0.15f));
            // Low-shelf Bass control + Treble-modulated cabinet low-pass filter
            bassLp += bassAlpha * (stage2 - bassLp);
            float toned = stage2 + (bassLp * bassShelf);
            cab += cabAlpha * (toned - cab);
            out[s] = cab * master;
        }
    }
}

std::vector<uint8_t> TubeAmpEffect::saveState() const {
    std::vector<uint8_t> data(4 * sizeof(float));
    float vals[4] = {
        m_gain.load(std::memory_order_relaxed),
        m_bass.load(std::memory_order_relaxed),
        m_treble.load(std::memory_order_relaxed),
        m_master.load(std::memory_order_relaxed)
    };
    std::memcpy(data.data(), vals, sizeof(vals));
    return data;
}

bool TubeAmpEffect::loadState(const std::vector<uint8_t>& state) {
    if (state.size() < 4 * sizeof(float)) return false;
    float vals[4];
    std::memcpy(vals, state.data(), sizeof(vals));
    setParameterValue(0, vals[0]);
    setParameterValue(1, vals[1]);
    setParameterValue(2, vals[2]);
    setParameterValue(3, vals[3]);
    return true;
}

// ==========================================
// StereoDelayEffect Implementation
// ==========================================

StereoDelayEffect::StereoDelayEffect() = default;

void StereoDelayEffect::prepare(double sampleRate, uint32_t /*maxBlockSize*/) {
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
        case 0: return {0, "Time (ms)", 20.0f, 1500.0f, 350.0f, m_timeMs.load(std::memory_order_relaxed)};
        case 1: return {1, "Feedback", 0.0f, 0.95f, 0.45f, m_feedback.load(std::memory_order_relaxed)};
        case 2: return {2, "Mix", 0.0f, 1.0f, 0.35f, m_mix.load(std::memory_order_relaxed)};
        default: return {};
    }
}

void StereoDelayEffect::setParameterValue(uint32_t paramId, float value) {
    switch (paramId) {
        case 0: m_timeMs.store(std::clamp(value, 20.0f, 1500.0f), std::memory_order_relaxed); break;
        case 1: m_feedback.store(std::clamp(value, 0.0f, 0.95f), std::memory_order_relaxed); break;
        case 2: m_mix.store(std::clamp(value, 0.0f, 1.0f), std::memory_order_relaxed); break;
        default: break;
    }
}

float StereoDelayEffect::getParameterValue(uint32_t paramId) const {
    switch (paramId) {
        case 0: return m_timeMs.load(std::memory_order_relaxed);
        case 1: return m_feedback.load(std::memory_order_relaxed);
        case 2: return m_mix.load(std::memory_order_relaxed);
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
    const float timeMs = m_timeMs.load(std::memory_order_relaxed);
    const uint32_t delaySamplesL = static_cast<uint32_t>((timeMs / 1000.0f) * static_cast<float>(m_sampleRate));
    // Ping-pong delay: right channel slightly offset (3/4 time)
    const uint32_t delaySamplesR = (delaySamplesL * 3u) / 4u;

    const float fb = m_feedback.load(std::memory_order_relaxed);
    const float mix = m_mix.load(std::memory_order_relaxed);

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

std::vector<uint8_t> StereoDelayEffect::saveState() const {
    std::vector<uint8_t> data(3 * sizeof(float));
    float vals[3] = {
        m_timeMs.load(std::memory_order_relaxed),
        m_feedback.load(std::memory_order_relaxed),
        m_mix.load(std::memory_order_relaxed)
    };
    std::memcpy(data.data(), vals, sizeof(vals));
    return data;
}

bool StereoDelayEffect::loadState(const std::vector<uint8_t>& state) {
    if (state.size() < 3 * sizeof(float)) return false;
    float vals[3];
    std::memcpy(vals, state.data(), sizeof(vals));
    setParameterValue(0, vals[0]);
    setParameterValue(1, vals[1]);
    setParameterValue(2, vals[2]);
    return true;
}

} // namespace praccy::plugins
