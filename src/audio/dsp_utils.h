#pragma once

#include <cmath>
#include <algorithm>
#include <numbers>
#include <atomic>

namespace praccy::audio {

/**
 * @brief Fast DSP utility math and crossfading functions.
 */
class DspUtils {
public:
    static constexpr float PI = std::numbers::pi_v<float>;
    static constexpr float HALF_PI = PI * 0.5f;
    static constexpr float MINUS_96_DB = -96.0f;
    static constexpr float EPSILON = 1e-6f;

    /**
     * @brief Converts decibels to linear amplitude.
     */
    [[nodiscard]] static inline float dbToGain(float db) noexcept {
        if (db <= MINUS_96_DB) return 0.0f;
        return std::pow(10.0f, db * 0.05f);
    }

    /**
     * @brief Converts linear amplitude to decibels.
     */
    [[nodiscard]] static inline float gainToDb(float gain) noexcept {
        if (gain <= EPSILON) return MINUS_96_DB;
        return 20.0f * std::log10(gain);
    }

    /**
     * @brief Computes equal-power crossfade gains for dry/wet blending (0.0 = 100% dry, 1.0 = 100% wet).
     * Maintains constant perceived loudness across the mix range.
     */
    static inline void calculateEqualPowerCrossfade(float mix, float& dryGain, float& wetGain) noexcept {
        mix = std::clamp(mix, 0.0f, 1.0f);
        const float angle = mix * HALF_PI;
        dryGain = std::cos(angle);
        wetGain = std::sin(angle);
    }

    /**
     * @brief Constant-power stereo panning law (-3 dB center).
     * pan: -1.0 (Full Left) to +1.0 (Full Right).
     */
    static inline void calculateStereoPan(float pan, float& leftGain, float& rightGain) noexcept {
        pan = std::clamp(pan, -1.0f, 1.0f);
        // Normalize pan from [-1.0, 1.0] to [0.0, 1.0]
        const float norm = (pan + 1.0f) * 0.5f;
        const float angle = norm * HALF_PI;
        leftGain = std::cos(angle);
        rightGain = std::sin(angle);
    }
};

/**
 * @brief Click-free crossfader used when bypassing/activating plugins or switching scenes.
 * Ramps smoothly over a configurable number of samples (typically ~10ms).
 */
class EqualPowerRamp {
public:
    EqualPowerRamp() = default;

    void reset(uint32_t rampSamples) noexcept {
        m_totalSamples = std::max(1u, rampSamples);
        m_currentSample = 0;
        m_active = false;
        m_targetState = false;
    }

    void startTransition(bool activating) noexcept {
        m_targetState = activating;
        m_currentSample = 0;
        m_active = true;
    }

    [[nodiscard]] bool isTransitioning() const noexcept { return m_active; }
    [[nodiscard]] bool targetState() const noexcept { return m_targetState; }

    /**
     * @brief Advances one sample and returns (gainOld, gainNew).
     */
    inline void getNextGains(float& gainOld, float& gainNew) noexcept {
        if (!m_active) {
            gainOld = m_targetState ? 0.0f : 1.0f;
            gainNew = m_targetState ? 1.0f : 0.0f;
            return;
        }

        const float progress = static_cast<float>(m_currentSample) / static_cast<float>(m_totalSamples);
        const float angle = progress * DspUtils::HALF_PI;

        if (m_targetState) {
            // Fading in new, fading out old
            gainOld = std::cos(angle);
            gainNew = std::sin(angle);
        } else {
            // Fading out new, fading in old (bypassing)
            gainOld = std::sin(angle);
            gainNew = std::cos(angle);
        }

        m_currentSample++;
        if (m_currentSample >= m_totalSamples) {
            m_active = false;
        }
    }

private:
    uint32_t m_totalSamples{480}; // Default 10ms @ 48kHz
    uint32_t m_currentSample{0};
    bool m_active{false};
    bool m_targetState{false};
};

/**
 * @brief Lock-free atomic peak level meter for UI feedback.
 */
class LevelMeter {
public:
    LevelMeter() : m_peakLeft(0.0f), m_peakRight(0.0f) {}

    void process(const float* left, const float* right, uint32_t numSamples) noexcept {
        float maxL = 0.0f;
        float maxR = 0.0f;
        for (uint32_t i = 0; i < numSamples; ++i) {
            maxL = std::max(maxL, std::abs(left[i]));
            if (right) {
                maxR = std::max(maxR, std::abs(right[i]));
            }
        }
        // Update atomics if new peak is higher, otherwise gently decay
        float curL = m_peakLeft.load(std::memory_order_relaxed);
        m_peakLeft.store(std::max(maxL, curL * 0.95f), std::memory_order_relaxed);

        float curR = m_peakRight.load(std::memory_order_relaxed);
        m_peakRight.store(std::max(maxR ? maxR : maxL, curR * 0.95f), std::memory_order_relaxed);
    }

    [[nodiscard]] float peakLeft() const noexcept { return m_peakLeft.load(std::memory_order_relaxed); }
    [[nodiscard]] float peakRight() const noexcept { return m_peakRight.load(std::memory_order_relaxed); }

private:
    std::atomic<float> m_peakLeft{0.0f};
    std::atomic<float> m_peakRight{0.0f};
};

} // namespace praccy::audio
