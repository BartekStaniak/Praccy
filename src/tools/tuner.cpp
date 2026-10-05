#include "tuner.h"
#include <array>
#include <algorithm>

namespace praccy::tools {

static const std::array<const char*, 12> NOTE_NAMES = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

InstrumentTuner::InstrumentTuner(uint32_t bufferSize)
    : m_bufferSize(bufferSize),
      m_inputHistory(bufferSize, 0.0f),
      m_differenceBuffer(bufferSize / 2, 0.0f),
      m_cumulativeDiffBuffer(bufferSize / 2, 0.0f) {}

void InstrumentTuner::prepare(double sampleRate) {
    m_sampleRate = sampleRate;
    std::fill(m_inputHistory.begin(), m_inputHistory.end(), 0.0f);
    m_writeIndex = 0;
}

void InstrumentTuner::process(const float* monoInput, uint32_t numSamples) {
    if (!m_enabled.load(std::memory_order_relaxed) || !monoInput || numSamples == 0) return;

    for (uint32_t i = 0; i < numSamples; ++i) {
        m_inputHistory[m_writeIndex] = monoInput[i];
        m_writeIndex = (m_writeIndex + 1) % m_bufferSize;
    }

    detectPitchYin();
}

void InstrumentTuner::detectPitchYin() {
    const uint32_t halfBuffer = m_bufferSize / 2;

    // Check minimum signal energy (RMS threshold) to avoid tracking background noise
    float energy = 0.0f;
    for (uint32_t i = 0; i < m_bufferSize; ++i) {
        energy += m_inputHistory[i] * m_inputHistory[i];
    }
    const float rms = std::sqrt(energy / static_cast<float>(m_bufferSize));
    if (rms < 0.005f) { // -46 dB noise gate for tuner
        m_hasPitch.store(false, std::memory_order_relaxed);
        return;
    }

    // Step 1: Difference function
    for (uint32_t tau = 0; tau < halfBuffer; ++tau) {
        float sum = 0.0f;
        for (uint32_t j = 0; j < halfBuffer; ++j) {
            const uint32_t idx1 = (m_writeIndex + j) % m_bufferSize;
            const uint32_t idx2 = (m_writeIndex + j + tau) % m_bufferSize;
            const float diff = m_inputHistory[idx1] - m_inputHistory[idx2];
            sum += diff * diff;
        }
        m_differenceBuffer[tau] = sum;
    }

    // Step 2: Cumulative mean normalized difference
    m_cumulativeDiffBuffer[0] = 1.0f;
    float runningSum = 0.0f;
    for (uint32_t tau = 1; tau < halfBuffer; ++tau) {
        runningSum += m_differenceBuffer[tau];
        if (runningSum > 0.0f) {
            m_cumulativeDiffBuffer[tau] = m_differenceBuffer[tau] / (runningSum / static_cast<float>(tau));
        } else {
            m_cumulativeDiffBuffer[tau] = 1.0f;
        }
    }

    // Step 3: Absolute threshold
    uint32_t tauFound = 0;
    for (uint32_t tau = 2; tau < halfBuffer; ++tau) {
        if (m_cumulativeDiffBuffer[tau] < m_yinThreshold) {
            while (tau + 1 < halfBuffer && m_cumulativeDiffBuffer[tau + 1] < m_cumulativeDiffBuffer[tau]) {
                tau++;
            }
            tauFound = tau;
            break;
        }
    }

    if (tauFound == 0) {
        m_hasPitch.store(false, std::memory_order_relaxed);
        return;
    }

    // Step 4: Parabolic interpolation
    float betterTau = static_cast<float>(tauFound);
    if (tauFound > 0 && tauFound + 1 < halfBuffer) {
        const float s0 = m_cumulativeDiffBuffer[tauFound - 1];
        const float s1 = m_cumulativeDiffBuffer[tauFound];
        const float s2 = m_cumulativeDiffBuffer[tauFound + 1];
        const float denominator = (2.0f * s1) - s0 - s2;
        if (std::abs(denominator) > 1e-6f) {
            betterTau += (s2 - s0) / (2.0f * denominator);
        }
    }

    if (betterTau <= 0.0f) {
        m_hasPitch.store(false, std::memory_order_relaxed);
        return;
    }

    // Step 5: Frequency calculation
    const float freq = static_cast<float>(m_sampleRate) / betterTau;
    if (freq < 20.0f || freq > 2000.0f) {
        m_hasPitch.store(false, std::memory_order_relaxed);
        return;
    }

    // Convert to MIDI Note number (A4 = 440 Hz = note 69)
    const float midiNoteExact = 69.0f + 12.0f * std::log2(freq / 440.0f);
    const int roundedNote = static_cast<int>(std::round(midiNoteExact));
    const float cents = (midiNoteExact - static_cast<float>(roundedNote)) * 100.0f;

    m_detectedFreq.store(freq, std::memory_order_relaxed);
    m_detectedMidiNote.store(roundedNote, std::memory_order_relaxed);
    m_detectedCents.store(cents, std::memory_order_relaxed);
    m_hasPitch.store(true, std::memory_order_relaxed);
}

TunerResult InstrumentTuner::currentResult() const {
    TunerResult result;
    result.confidence = m_hasPitch.load(std::memory_order_relaxed);
    if (!result.confidence) {
        result.noteName = "--";
        return result;
    }

    result.frequencyHz = m_detectedFreq.load(std::memory_order_relaxed);
    result.noteNumber = m_detectedMidiNote.load(std::memory_order_relaxed);
    result.centDeviation = m_detectedCents.load(std::memory_order_relaxed);

    const int noteIndex = (result.noteNumber % 12 + 12) % 12;
    const int octave = (result.noteNumber / 12) - 1;
    result.noteName = std::string(NOTE_NAMES[noteIndex]) + std::to_string(octave);

    return result;
}

} // namespace praccy::tools
