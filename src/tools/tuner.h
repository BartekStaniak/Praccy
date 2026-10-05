#pragma once

#include <vector>
#include <string>
#include <atomic>
#include <cmath>

namespace praccy::tools {

struct TunerResult {
    float frequencyHz{0.0f};
    int noteNumber{0};         // MIDI note number (69 = A4 = 440Hz)
    std::string noteName;      // e.g. "E2", "A2", "D3", "G3", "B3", "E4"
    float centDeviation{0.0f}; // [-50.0, +50.0] cents
    bool confidence{false};
};

/**
 * @brief High-precision chromatic instrument tuner using the YIN pitch detection algorithm.
 * Operates on input signal buffer with sub-cent resolution.
 */
class InstrumentTuner {
public:
    explicit InstrumentTuner(uint32_t bufferSize = 2048);

    void prepare(double sampleRate);
    void process(const float* monoInput, uint32_t numSamples);

    [[nodiscard]] TunerResult currentResult() const;
    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }
    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }

private:
    void detectPitchYin();

    uint32_t m_bufferSize;
    double m_sampleRate{48000.0};
    std::atomic<bool> m_enabled{true};

    std::vector<float> m_inputHistory;
    uint32_t m_writeIndex{0};

    std::vector<float> m_differenceBuffer;
    std::vector<float> m_cumulativeDiffBuffer;

    std::atomic<float> m_detectedFreq{0.0f};
    std::atomic<float> m_detectedCents{0.0f};
    std::atomic<int> m_detectedMidiNote{0};
    std::atomic<bool> m_hasPitch{false};

    float m_yinThreshold{0.15f};
};

} // namespace praccy::tools
