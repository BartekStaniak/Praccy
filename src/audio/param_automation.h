#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <array>
#include <utility>
#include <bit>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace praccy::audio {

/**
 * @brief Lock-free, wait-free, zero-allocation parameter automation register.
 *
 * Implements a flat table of 1024 atomic floats indexed by parameter ID, backed by
 * an atomic 64-bit dirty mask array (16 x uint64_t words). UI and MIDI threads write
 * parameter updates wait-free. Arbitrary 32-bit parameter IDs (>= 1024) are handled
 * wait-free via a 512-slot lock-free bounded-CAS overflow hash table with write-in-progress
 * fencing and clean-slot reclamation so high parameter IDs never race or exhaust capacity.
 */
class ParamAutomationRegister {
public:
    struct OverflowEntry {
        std::atomic<uint64_t> packed{0};
    };

    static constexpr size_t MAX_PARAMS = 1024;
    static constexpr size_t BITMASK_WORDS = MAX_PARAMS / 64; // 16 words
    static constexpr size_t OVERFLOW_CAPACITY = 512;
    static constexpr size_t OVERFLOW_WORDS = OVERFLOW_CAPACITY / 64; // 8 words
    static constexpr size_t OVERFLOW_MASK = OVERFLOW_CAPACITY - 1;
    static constexpr size_t MAX_CAS_RETRIES = 8;

    ParamAutomationRegister() noexcept {
        resetBeforePrepareOnly();
    }

    ~ParamAutomationRegister() = default;

    ParamAutomationRegister(const ParamAutomationRegister&) = delete;
    ParamAutomationRegister& operator=(const ParamAutomationRegister&) = delete;
    ParamAutomationRegister(ParamAutomationRegister&&) = delete;
    ParamAutomationRegister& operator=(ParamAutomationRegister&&) = delete;

    /**
     * @brief Resets all parameter values to 0.0f and clears dirty bitmasks.
     * MUST only be called when the audio engine is stopped or before prepare().
     * Never invoke concurrently with process() or setParameter().
     */
    void resetBeforePrepareOnly() noexcept {
        for (size_t w = 0; w < BITMASK_WORDS; ++w) {
            m_dirtyMask[w].store(0ULL, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < MAX_PARAMS; ++i) {
            m_values[i].store(0.0f, std::memory_order_relaxed);
        }
        for (size_t w = 0; w < OVERFLOW_WORDS; ++w) {
            m_overflowDirtyMask[w].store(0ULL, std::memory_order_relaxed);
            m_overflowInFlightMask[w].store(0ULL, std::memory_order_relaxed);
        }
        for (size_t s = 0; s < OVERFLOW_CAPACITY; ++s) {
            m_overflowTable[s].packed.store(0ULL, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Alias for resetBeforePrepareOnly for backward compatibility.
     */
    void reset() noexcept {
        resetBeforePrepareOnly();
    }

    /**
     * @brief Writes a parameter value and marks it dirty.
     * Handles both direct table (< 1024) and arbitrary 32-bit IDs (>= 1024) wait-free with zero allocations.
     * Called from UI or MIDI thread. Lock-free, bounded wait-free.
     */
    inline void setParameter(uint32_t paramId, float value) noexcept {
        if (paramId < MAX_PARAMS) {
            m_values[paramId].store(value, std::memory_order_release);
            const size_t wordIdx = paramId / 64;
            const uint64_t bitMask = 1ULL << (paramId % 64);
            m_dirtyMask[wordIdx].fetch_or(bitMask, std::memory_order_release);
            return;
        }

        const uint64_t newPacked = packEntry(paramId, value);
        const size_t startSlot = hashParamId(paramId);
        size_t reclaimableSlot = OVERFLOW_CAPACITY;
        uint64_t reclaimableExpected = 0ULL;

        for (size_t probe = 0; probe < OVERFLOW_CAPACITY; ++probe) {
            const size_t slot = (startSlot + probe) & OVERFLOW_MASK;
            const size_t wordIdx = slot / 64;
            const uint64_t bitMask = 1ULL << (slot % 64);

            uint64_t cur = m_overflowTable[slot].packed.load(std::memory_order_acquire);
            for (size_t retry = 0; retry < MAX_CAS_RETRIES; ++retry) {
                if (cur != 0ULL && unpackParamId(cur) != paramId) {
                    if (reclaimableSlot == OVERFLOW_CAPACITY) {
                        const uint64_t busyWord =
                            m_overflowDirtyMask[wordIdx].load(std::memory_order_acquire) |
                            m_overflowInFlightMask[wordIdx].load(std::memory_order_acquire);
                        if ((busyWord & bitMask) == 0ULL) {
                            const uint64_t verifyCur = m_overflowTable[slot].packed.load(std::memory_order_acquire);
                            if (verifyCur == cur) {
                                reclaimableSlot = slot;
                                reclaimableExpected = cur;
                            }
                        }
                    }
                    break;
                }

                m_overflowInFlightMask[wordIdx].fetch_or(bitMask, std::memory_order_acq_rel);
                if (m_overflowTable[slot].packed.compare_exchange_weak(
                        cur, newPacked, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    m_overflowDirtyMask[wordIdx].fetch_or(bitMask, std::memory_order_release);
                    m_overflowInFlightMask[wordIdx].fetch_and(~bitMask, std::memory_order_release);
                    return;
                }
                m_overflowInFlightMask[wordIdx].fetch_and(~bitMask, std::memory_order_release);
            }
        }

        // If all 512 slots were occupied by distinct historical IDs (and paramId was not present),
        // atomically claim the first drained, non-in-flight slot.
        if (reclaimableSlot < OVERFLOW_CAPACITY) {
            const size_t wordIdx = reclaimableSlot / 64;
            const uint64_t bitMask = 1ULL << (reclaimableSlot % 64);
            const uint64_t prevInFlight = m_overflowInFlightMask[wordIdx].fetch_or(bitMask, std::memory_order_acq_rel);
            if ((prevInFlight & bitMask) == 0ULL) {
                if ((m_overflowDirtyMask[wordIdx].load(std::memory_order_acquire) & bitMask) == 0ULL) {
                    if (m_overflowTable[reclaimableSlot].packed.compare_exchange_strong(
                            reclaimableExpected, newPacked, std::memory_order_acq_rel, std::memory_order_acquire)) {
                        m_overflowDirtyMask[wordIdx].fetch_or(bitMask, std::memory_order_release);
                    }
                }
                m_overflowInFlightMask[wordIdx].fetch_and(~bitMask, std::memory_order_release);
            }
        }
    }

    /**
     * @brief Retrieves the current value of a parameter.
     * Wait-free, lock-free.
     */
    [[nodiscard]] inline float getParameter(uint32_t paramId) const noexcept {
        if (paramId < MAX_PARAMS) {
            return m_values[paramId].load(std::memory_order_relaxed);
        }

        const size_t startSlot = hashParamId(paramId);
        for (size_t probe = 0; probe < OVERFLOW_CAPACITY; ++probe) {
            const size_t slot = (startSlot + probe) & OVERFLOW_MASK;
            const uint64_t entry = m_overflowTable[slot].packed.load(std::memory_order_acquire);
            if (entry == 0ULL) {
                break;
            }
            if (unpackParamId(entry) == paramId) {
                return unpackValue(entry);
            }
        }
        return 0.0f;
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
        for (size_t w = 0; w < OVERFLOW_WORDS; ++w) {
            if (m_overflowDirtyMask[w].load(std::memory_order_relaxed) != 0ULL) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Checks if a specific parameter is marked dirty.
     */
    [[nodiscard]] inline bool isParamDirty(uint32_t paramId) const noexcept {
        if (paramId < MAX_PARAMS) {
            const size_t wordIdx = paramId / 64;
            const uint64_t bitMask = 1ULL << (paramId % 64);
            return (m_dirtyMask[wordIdx].load(std::memory_order_relaxed) & bitMask) != 0ULL;
        }

        const size_t startSlot = hashParamId(paramId);
        for (size_t probe = 0; probe < OVERFLOW_CAPACITY; ++probe) {
            const size_t slot = (startSlot + probe) & OVERFLOW_MASK;
            const uint64_t entry = m_overflowTable[slot].packed.load(std::memory_order_acquire);
            if (entry == 0ULL) {
                break;
            }
            if (unpackParamId(entry) == paramId) {
                const size_t wordIdx = slot / 64;
                const uint64_t bitMask = 1ULL << (slot % 64);
                return (m_overflowDirtyMask[wordIdx].load(std::memory_order_relaxed) & bitMask) != 0ULL;
            }
        }
        return false;
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

        for (size_t w = 0; w < OVERFLOW_WORDS; ++w) {
            uint64_t overflowDirty = m_overflowDirtyMask[w].exchange(0ULL, std::memory_order_acq_rel);
            while (overflowDirty != 0ULL) {
                const unsigned long bitIdx = countTrailingZeros64(overflowDirty);
                const size_t slot = w * 64 + bitIdx;
                const uint64_t entry = m_overflowTable[slot].packed.load(std::memory_order_acquire);
                if (entry != 0ULL) {
                    const uint32_t paramId = unpackParamId(entry);
                    const float val = unpackValue(entry);
                    callback(paramId, val);
                }
                overflowDirty &= overflowDirty - 1ULL;
            }
        }
    }

private:
    static inline uint64_t packEntry(uint32_t paramId, float value) noexcept {
        const uint32_t floatBits = std::bit_cast<uint32_t>(value);
        return (static_cast<uint64_t>(paramId) << 32) | static_cast<uint64_t>(floatBits);
    }

    static inline uint32_t unpackParamId(uint64_t packed) noexcept {
        return static_cast<uint32_t>(packed >> 32);
    }

    static inline float unpackValue(uint64_t packed) noexcept {
        const uint32_t floatBits = static_cast<uint32_t>(packed & 0xFFFFFFFFULL);
        return std::bit_cast<float>(floatBits);
    }

    static inline size_t hashParamId(uint32_t paramId) noexcept {
        return static_cast<size_t>((static_cast<uint64_t>(paramId) * 2654435761ULL) & OVERFLOW_MASK);
    }

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

    alignas(64) std::array<std::atomic<uint64_t>, BITMASK_WORDS> m_dirtyMask{};
    alignas(64) std::array<std::atomic<float>, MAX_PARAMS> m_values{};
    alignas(64) std::array<std::atomic<uint64_t>, OVERFLOW_WORDS> m_overflowDirtyMask{};
    alignas(64) std::array<std::atomic<uint64_t>, OVERFLOW_WORDS> m_overflowInFlightMask{};
    alignas(64) std::array<OverflowEntry, OVERFLOW_CAPACITY> m_overflowTable{};
};

} // namespace praccy::audio
