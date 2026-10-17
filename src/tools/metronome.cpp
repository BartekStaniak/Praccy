#include "metronome.h"
#include "../audio/dsp_utils.h"
#include <cmath>
#include <numbers>

namespace praccy::tools {

Metronome::Metronome() = default;

void Metronome::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
    // 20ms click length
    m_clickLengthSamples = static_cast<uint32_t>(sampleRate * 0.020);
    reset();
}

void Metronome::reset() {
    m_currentBeatSample = 0;
    m_clickSamplePosition = m_clickLengthSamples; // Inactive
    m_decay = 0.0f;
    m_decayFactor = 1.0f;
    m_phasorCos = 1.0f;
    m_phasorSin = 0.0f;
    m_rotCos = 1.0f;
    m_rotSin = 0.0f;
    m_currentBeat.store(0, std::memory_order_relaxed);
}

void Metronome::triggerClick(bool isAccent) noexcept {
    m_clickSamplePosition = 0;
    m_decay = 1.0f;
    if (m_clickLengthSamples > 0) {
        m_decayFactor = std::exp(-8.0f / static_cast<float>(m_clickLengthSamples));
    } else {
        m_decayFactor = 0.0f;
    }
    const float freq = isAccent ? 1760.0f : 880.0f; // A6 accent, A5 regular click
    const float sr = static_cast<float>((m_sampleRate > 8000.0) ? m_sampleRate : 48000.0);
    const float w = 2.0f * std::numbers::pi_v<float> * freq / sr;
    m_phasorCos = 1.0f;
    m_phasorSin = 0.0f;
    m_rotCos = std::cos(w);
    m_rotSin = std::sin(w);
}

void Metronome::process(audio::AudioBufferView& output) {
    if (m_resetRequested.exchange(false, std::memory_order_acq_rel)) {
        m_currentBeatSample = 0;
        m_currentBeat.store(0, std::memory_order_relaxed);
        m_isAccent = true;
        triggerClick(true);
    }
    if (!m_playing.load(std::memory_order_relaxed)) {
        m_currentBeatSample = 0;
        m_clickSamplePosition = m_clickLengthSamples;
        return;
    }

    const float bpm = m_bpm.load(std::memory_order_relaxed);
    const int beatsPerBar = m_beatsPerBar.load(std::memory_order_relaxed);
    const float volGain = audio::DspUtils::dbToGain(m_volumeDb.load(std::memory_order_relaxed));

    const double samplesPerBeat = (60.0 / static_cast<double>(bpm)) * m_sampleRate;
    const uint32_t numSamples = output.numSamples();
    const uint32_t numCh = output.numChannels();

    float* ch0 = output.channel(0);
    float* ch1 = numCh > 1 ? output.channel(1) : nullptr;

    for (uint32_t s = 0; s < numSamples; ++s) {
        if (m_currentBeatSample >= static_cast<uint64_t>(samplesPerBeat)) {
            m_currentBeatSample = 0;
            int nextBeat = (m_currentBeat.load(std::memory_order_relaxed) + 1) % beatsPerBar;
            m_currentBeat.store(nextBeat, std::memory_order_relaxed);
            m_isAccent = (nextBeat == 0);
            triggerClick(m_isAccent);
        }

        if (m_clickSamplePosition < m_clickLengthSamples) {
            const float clickSample = m_phasorSin * m_decay * volGain;

            ch0[s] += clickSample;
            if (ch1) ch1[s] += clickSample;

            m_decay *= m_decayFactor;
            const float nextCos = m_phasorCos * m_rotCos - m_phasorSin * m_rotSin;
            const float nextSin = m_phasorCos * m_rotSin + m_phasorSin * m_rotCos;
            m_phasorCos = nextCos;
            m_phasorSin = nextSin;

            m_clickSamplePosition++;
        }

        m_currentBeatSample++;
    }
}

} // namespace praccy::tools
