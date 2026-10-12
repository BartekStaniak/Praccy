#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <array>
#include <utility>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace praccy::audio {

/**
 * @brief Lock-free, wait-free, zero-allocation parameter automation register.
 *
 * Implements a flat table of 1024 atomic floats indexed by parameter ID, backed by
 * an atomic 64-bit dirty mask array (16 x uint64_t words). UI and MIDI threads write
 * parameter updates wait-free. The real-time audio thread drains dirty parameters at
 * block boundaries wait-free with zero dynamic memory allocations, automatically
 * coalescing rapid parameter sweeps.
 */
class ParamAutomationRegister {
public:
    static constexpr size_t MAX_PARAMS = 1024;
    static constexpr size_t BITMASK_WORDS = MAX_PARAMS / 64; // 16 words

    ParamAutomationRegister() noexcept {
        reset();
    }

    ~ParamAutomationRegister() = default;

    ParamAutomationRegister(const ParamAutomationRegister&) = delete;
    ParamAutomationRegister& operator=(const ParamAutomationRegister&) = delete;
    ParamAutomationRegister(ParamAutomationRegister&&) = delete;
    ParamAutomationRegister& operator=(ParamAutomationRegister&&) = delete;

    /**
     * @brief Resets all parameter values to 0.0f and clears the dirty bitmask.
     */
    void reset() noexcept {
        for (size_t w = 0; w < BITMASK_WORDS; ++w) {
            m_dirtyMask[w].store(0ULL, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < MAX_PARAMS; ++i) {
            m_values[i].store(0.0f, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Writes a parameter value and marks it dirty.
     * Called from UI or MIDI thread. Wait-free, lock-free.
     */
    inline void setParameter(uint32_t paramId, float value) noexcept {
        if (paramId >= MAX_PARAMS) return;

        m_values[paramId].store(value, std::memory_order_release);

        const size_t wordIdx = paramId / 64;
        const uint64_t bitMask = 1ULL << (paramId % 64);
        m_dirtyMask[wordIdx].fetch_or(bitMask, std::memory_order_release);
    }

    /**
     * @brief Retrieves the current value of a parameter.
     * Wait-free, lock-free.
     */
    [[nodiscard]] inline float getParameter(uint32_t paramId) const noexcept {
        if (paramId >= MAX_PARAMS) return 0.0f;
        return m_values[paramId].load(std::memory_order_relaxed);
    }

    /**
     * @brief Checks if any parameter is marked dirty.
     */
    [[nodiscard]] inline bool isDirty() const noexcept {
        for (size_t w = 0; w < BITMASK_WORDS; ++w) {
            if (m_dirtyMask[w].load(std::memory_order_relaxed) != 0ULL) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Checks if a specific parameter is marked dirty.
     */
    [[nodiscard]] inline bool isParamDirty(uint32_t paramId) const noexcept {
        if (paramId >= MAX_PARAMS) return false;
        const size_t wordIdx = paramId / 64;
        const uint64_t bitMask = 1ULL << (paramId % 64);
        return (m_dirtyMask[wordIdx].load(std::memory_order_relaxed) & bitMask) != 0ULL;
    }

    /**
     * @brief Drains all dirty parameters wait-free, resetting the dirty mask atomically.
     * Called on the real-time audio thread at audio block boundaries.
     * Guaranteed 0 heap allocations and 0 mutex locks.
     *
     * @tparam Fn Callback callable with signature void(uint32_t paramId, float value)
     */
    template <typename Fn>
    inline void drainDirty(Fn&& callback) noexcept {
        for (size_t w = 0; w < BITMASK_WORDS; ++w) {
            uint64_t dirty = m_dirtyMask[w].exchange(0ULL, std::memory_order_acq_rel);
            while (dirty != 0ULL) {
                const unsigned long bitIdx = countTrailingZeros64(dirty);
                const uint32_t paramId = static_cast<uint32_t>(w * 64 + bitIdx);
                const float val = m_values[paramId].load(std::memory_order_acquire);

                callback(paramId, val);

                dirty &= dirty - 1ULL; // Clear lowest set bit
            }
        }
    }

private:
    static inline unsigned long countTrailingZeros64(uint64_t val) noexcept {
#if defined(_MSC_VER)
        unsigned long index = 0;
        _BitScanForward64(&index, val);
        return index;
#elif defined(__GNUC__) || defined(__clang__)
        return static_cast<unsigned long>(__builtin_ctzll(val));
#else
        unsigned long count = 0;
        while ((val & 1ULL) == 0ULL) {
            val >>= 1;
            count++;
        }
        return count;
#endif
    }

    // Cache-line aligned members to eliminate false sharing between cores
    alignas(64) std::array<std::atomic<uint64_t>, BITMASK_WORDS> m_dirtyMask;
    alignas(64) std::array<std::atomic<float>, MAX_PARAMS> m_values;
};

} // namespace praccy::audio
