#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
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
     * @brief Branchless C^2-continuous Padé [3/2] rational tanh saturator.
     * Evaluates x * (27.0f + x2) / (27.0f + 9.0f * x2) clamped to [-1.0f, +1.0f] for |x| >= 3.0f.
     */
    [[nodiscard]] static inline float fastTanh(float x) noexcept {
        const float xc = std::clamp(x, -3.0f, 3.0f);
        const float x2 = xc * xc;
        return xc * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    /**
     * @brief Fast 9th-degree polynomial approximation for sin(t * pi/2), t in [0.0f, 1.0f].
     * Guarantees exact endpoints (0.0f at t=0, 1.0f at t=1) and strict monotonicity.
     */
    [[nodiscard]] static inline float fastSinQuarterPi(float t) noexcept {
        if (t <= 0.0f) return 0.0f;
        if (t >= 1.0f) return 1.0f;
        const float x = t * HALF_PI;
        const float x2 = x * x;
        const float val = x * (1.0f + x2 * (-0.16666666666666666f + x2 * (0.008333333333333333f + x2 * (-0.0001984126984126984f + x2 * 0.00000275573192239859f))));
        return std::min(1.0f, std::max(0.0f, val));
    }

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

    /**
     * @brief Scrub IEEE-754 NaN/Inf values and hard-clamp runaway plugin gain spikes (+18 dBFS)
     * before they can poison downstream IIR filters, delay feedback loops, or hardware outputs.
     */
    static inline bool sanitizeChannel(float* data, uint32_t numSamples, float hardClamp = 8.0f) noexcept {
        if (!data) return false;
        bool faulted = false;
        for (uint32_t s = 0; s < numSamples; ++s) {
            uint32_t bits = 0;
            std::memcpy(&bits, &data[s], sizeof(uint32_t));
            if ((bits & 0x7F800000u) == 0x7F800000u) {
                data[s] = 0.0f;
                faulted = true;
            } else if (data[s] > hardClamp) {
                data[s] = hardClamp;
            } else if (data[s] < -hardClamp) {
                data[s] = -hardClamp;
            }
        }
        return faulted;
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
        const float s = DspUtils::fastSinQuarterPi(progress);
        const float c = DspUtils::fastSinQuarterPi(1.0f - progress);

        if (m_targetState) {
            // Fading in new, fading out old
            gainOld = c;
            gainNew = s;
        } else {
            // Fading out new, fading in old (bypassing)
            gainOld = s;
            gainNew = c;
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

    void reset() noexcept {
        m_peakLeft.store(0.0f, std::memory_order_relaxed);
        m_peakRight.store(0.0f, std::memory_order_relaxed);
        m_holdLeft.store(0.0f, std::memory_order_relaxed);
        m_holdRight.store(0.0f, std::memory_order_relaxed);
        m_clipped.store(false, std::memory_order_relaxed);
    }

    void clearClip() noexcept {
        m_clipped.store(false, std::memory_order_relaxed);
    }

    void resetClip() noexcept {
        clearClip();
    }

    void process(const float* left, const float* right, uint32_t numSamples, double sampleRate = 48000.0) noexcept {
        float maxL = 0.0f;
        float maxR = 0.0f;
        for (uint32_t i = 0; i < numSamples; ++i) {
            if (left) maxL = std::max(maxL, std::abs(left[i]));
            if (right) {
                maxR = std::max(maxR, std::abs(right[i]));
            }
        }
        const float targetR = right ? maxR : maxL;

        if (maxL >= 0.998f || targetR >= 0.998f) {
            m_clipped.store(true, std::memory_order_relaxed);
        }

        // Responsive peak ballistics: instantaneous peak capture, snappy 120 ms exponential decay
        const float sr = (sampleRate > 8000.0) ? static_cast<float>(sampleRate) : 48000.0f;
        const float dt = static_cast<float>(numSamples) / sr;
        const float releaseCoeff = 1.0f - std::exp(-dt / 0.120f);
        const float holdReleaseCoeff = 1.0f - std::exp(-dt / 1.500f);

        float curL = m_peakLeft.load(std::memory_order_relaxed);
        curL = (maxL >= curL) ? maxL : (curL + releaseCoeff * (maxL - curL));
        m_peakLeft.store(curL < 1e-6f ? 0.0f : curL, std::memory_order_relaxed);

        float curR = m_peakRight.load(std::memory_order_relaxed);
        curR = (targetR >= curR) ? targetR : (curR + releaseCoeff * (targetR - curR));
        m_peakRight.store(curR < 1e-6f ? 0.0f : curR, std::memory_order_relaxed);

        float holdL = m_holdLeft.load(std::memory_order_relaxed);
        holdL = (maxL >= holdL) ? maxL : (holdL + holdReleaseCoeff * (maxL - holdL));
        m_holdLeft.store(holdL < 1e-6f ? 0.0f : holdL, std::memory_order_relaxed);

        float holdR = m_holdRight.load(std::memory_order_relaxed);
        holdR = (targetR >= holdR) ? targetR : (holdR + holdReleaseCoeff * (targetR - holdR));
        m_holdRight.store(holdR < 1e-6f ? 0.0f : holdR, std::memory_order_relaxed);
    }

    [[nodiscard]] float peakLeft() const noexcept { return m_peakLeft.load(std::memory_order_relaxed); }
    [[nodiscard]] float peakRight() const noexcept { return m_peakRight.load(std::memory_order_relaxed); }
    [[nodiscard]] float holdLeft() const noexcept { return m_holdLeft.load(std::memory_order_relaxed); }
    [[nodiscard]] float holdRight() const noexcept { return m_holdRight.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isClipped() const noexcept { return m_clipped.load(std::memory_order_relaxed); }

private:
    std::atomic<float> m_peakLeft{0.0f};
    std::atomic<float> m_peakRight{0.0f};
    std::atomic<float> m_holdLeft{0.0f};
    std::atomic<float> m_holdRight{0.0f};
    std::atomic<bool> m_clipped{false};
};

/**
 * @brief Fast branchless Padé rational tanh saturator.
 */
[[nodiscard]] inline float fastTanh(float x) noexcept {
    return DspUtils::fastTanh(x);
}

} // namespace praccy::audio
