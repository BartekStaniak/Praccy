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
    m_currentBeat.store(0, std::memory_order_relaxed);
}

void Metronome::process(audio::AudioBufferView& output) {
    if (!m_playing.load(std::memory_order_relaxed)) return;

    const float bpm = m_bpm.load(std::memory_order_relaxed);
    const int beatsPerBar = m_beatsPerBar.load(std::memory_order_relaxed);
    const float volGain = audio::DspUtils::dbToGain(m_volumeDb.load(std::memory_order_relaxed));

    const double samplesPerBeat = (60.0 / bpm) * m_sampleRate;
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
            m_clickSamplePosition = 0; // Trigger click
        }

        if (m_clickSamplePosition < m_clickLengthSamples) {
            const float progress = static_cast<float>(m_clickSamplePosition) / static_cast<float>(m_clickLengthSamples);
            const float decay = std::exp(-progress * 8.0f); // Fast exponential envelope
            const float freq = m_isAccent ? 1760.0f : 880.0f; // A6 accent, A5 regular click
            const float phase = 2.0f * std::numbers::pi_v<float> * freq * (static_cast<float>(m_clickSamplePosition) / static_cast<float>(m_sampleRate));
            const float clickSample = std::sin(phase) * decay * volGain;

            ch0[s] += clickSample;
            if (ch1) ch1[s] += clickSample;

            m_clickSamplePosition++;
        }

        m_currentBeatSample++;
    }
}

} // namespace praccy::tools
