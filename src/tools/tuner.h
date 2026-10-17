#pragma once

#include <vector>
#include <string>
#include <atomic>
#include <cmath>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstring>
#include <algorithm>
#include <array>
#include <string_view>
#include <ostream>
#include "tuner_settings.h"

namespace praccy::tools {

/**
 * @brief Zero-heap-allocation fixed buffer for note name representation (e.g. "E2", "A#4", "--").
 * Fully compatible with std::string_view, const char*, and std::string callers.
 */
struct NoteNameBuffer {
    std::array<char, 8> noteNameBuf{'-', '-', '\0', '\0', '\0', '\0', '\0', '\0'};

    constexpr NoteNameBuffer() noexcept {
        noteNameBuf[0] = '-';
        noteNameBuf[1] = '-';
        noteNameBuf[2] = '\0';
    }

    NoteNameBuffer(const char* str) noexcept {
        assign(str);
    }

    NoteNameBuffer(std::string_view sv) noexcept {
        assign(sv);
    }

    NoteNameBuffer(const std::string& str) noexcept {
        assign(std::string_view(str));
    }

    NoteNameBuffer& operator=(const char* str) noexcept {
        assign(str);
        return *this;
    }

    NoteNameBuffer& operator=(std::string_view sv) noexcept {
        assign(sv);
        return *this;
    }

    NoteNameBuffer& operator=(const std::string& str) noexcept {
        assign(std::string_view(str));
        return *this;
    }

    void assign(const char* str) noexcept {
        if (!str) {
            noteNameBuf[0] = '\0';
            return;
        }
        size_t i = 0;
        while (i < 7 && str[i] != '\0') {
            noteNameBuf[i] = str[i];
            ++i;
        }
        noteNameBuf[i] = '\0';
    }

    void assign(std::string_view sv) noexcept {
        const size_t len = std::min(sv.size(), size_t{7});
        if (len > 0) {
            std::memcpy(noteNameBuf.data(), sv.data(), len);
        }
        noteNameBuf[len] = '\0';
    }

    void setNote(const char* note, int octave) noexcept {
        size_t i = 0;
        if (note) {
            while (i < 4 && note[i] != '\0') {
                noteNameBuf[i] = note[i];
                ++i;
            }
        }
        if (octave < 0) {
            if (i < 7) noteNameBuf[i++] = '-';
            octave = -octave;
        }
        if (octave >= 10 && i < 6) {
            noteNameBuf[i++] = static_cast<char>('0' + (octave / 10));
            octave %= 10;
        }
        if (i < 7) {
            noteNameBuf[i++] = static_cast<char>('0' + octave);
        }
        noteNameBuf[i] = '\0';
    }

    void clear() noexcept {
        noteNameBuf[0] = '\0';
    }

    [[nodiscard]] const char* c_str() const noexcept { return noteNameBuf.data(); }
    [[nodiscard]] const char* data() const noexcept { return noteNameBuf.data(); }
    [[nodiscard]] bool empty() const noexcept { return noteNameBuf[0] == '\0'; }
    [[nodiscard]] size_t size() const noexcept { return std::strlen(noteNameBuf.data()); }
    [[nodiscard]] size_t length() const noexcept { return size(); }

    [[nodiscard]] char operator[](size_t idx) const noexcept { return noteNameBuf[idx]; }
    [[nodiscard]] char& operator[](size_t idx) noexcept { return noteNameBuf[idx]; }

    [[nodiscard]] const char* begin() const noexcept { return noteNameBuf.data(); }
    [[nodiscard]] const char* end() const noexcept { return noteNameBuf.data() + size(); }

    operator std::string_view() const noexcept { return std::string_view(noteNameBuf.data(), size()); }
    operator std::string() const { return std::string(noteNameBuf.data(), size()); }

    bool operator==(const char* other) const noexcept {
        if (!other) return empty();
        return std::strcmp(noteNameBuf.data(), other) == 0;
    }
    bool operator!=(const char* other) const noexcept {
        return !(*this == other);
    }

    bool operator==(std::string_view other) const noexcept {
        return std::string_view(noteNameBuf.data(), size()) == other;
    }
    bool operator!=(std::string_view other) const noexcept {
        return !(*this == other);
    }

    bool operator==(const std::string& other) const noexcept {
        return std::string_view(noteNameBuf.data(), size()) == other;
    }
    bool operator!=(const std::string& other) const noexcept {
        return !(*this == other);
    }

    bool operator==(const NoteNameBuffer& other) const noexcept {
        return std::strcmp(noteNameBuf.data(), other.noteNameBuf.data()) == 0;
    }
    bool operator!=(const NoteNameBuffer& other) const noexcept {
        return !(*this == other);
    }

    friend bool operator==(const char* lhs, const NoteNameBuffer& rhs) noexcept {
        return rhs == lhs;
    }
    friend bool operator!=(const char* lhs, const NoteNameBuffer& rhs) noexcept {
        return rhs != lhs;
    }
    friend bool operator==(std::string_view lhs, const NoteNameBuffer& rhs) noexcept {
        return rhs == lhs;
    }
    friend bool operator!=(std::string_view lhs, const NoteNameBuffer& rhs) noexcept {
        return rhs != lhs;
    }
    friend bool operator==(const std::string& lhs, const NoteNameBuffer& rhs) noexcept {
        return rhs == lhs;
    }
    friend bool operator!=(const std::string& lhs, const NoteNameBuffer& rhs) noexcept {
        return rhs != lhs;
    }

    friend std::ostream& operator<<(std::ostream& os, const NoteNameBuffer& n) {
        return os << n.c_str();
    }
};

struct TunerResult {
    float frequencyHz{0.0f};
    int noteNumber{0};         // MIDI note number (69 = A4 = 440Hz)
    NoteNameBuffer noteName;   // e.g. "E2", "A2", "D3", "G3", "B3", "E4"
    float centDeviation{0.0f}; // [-50.0, +50.0] cents
    bool confidence{false};
    int matchedStringIndex{-1}; // 0-indexed string (0 = String 1), -1 if none
    float targetFrequencyHz{0.0f};
    float stringCentDeviation{0.0f};
};

/**
 * @brief Direct Form II Transposed biquad filter with NaN/Inf poisoning recovery.
 */
class BiquadFilter {
public:
    void reset() noexcept {
        m_z1 = 0.0f;
        m_z2 = 0.0f;
    }

    void setCoefficients(float b0, float b1, float b2, float a1, float a2) noexcept {
        m_b0 = b0;
        m_b1 = b1;
        m_b2 = b2;
        m_a1 = a1;
        m_a2 = a2;
    }

    float process(float in) noexcept;

private:
    float m_b0{1.0f}, m_b1{0.0f}, m_b2{0.0f};
    float m_a1{0.0f}, m_a2{0.0f};
    float m_z1{0.0f}, m_z2{0.0f};
};

/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) circular ring buffer for raw audio samples.
 * Bounded power-of-2 capacity, wait-free operations on both producer and consumer threads.
 */
class AudioRingBuffer {
public:
    AudioRingBuffer() = default;
    explicit AudioRingBuffer(size_t capacityPowerOf2);

    void resize(size_t capacityPowerOf2);
    void reset() noexcept;

    // Single-producer write methods (Audio Callback Thread)
    uint32_t push(const float* in, uint32_t count) noexcept;
    uint32_t pushStereo(const float* left, const float* right, uint32_t count) noexcept;

    // Single-consumer read methods (Tuner Background Worker Thread)
    uint32_t read(float* out, uint32_t count) noexcept;
    void skip(size_t count) noexcept;

    [[nodiscard]] size_t availableToRead() const noexcept;
    [[nodiscard]] size_t availableToWrite() const noexcept;
    [[nodiscard]] size_t capacity() const noexcept { return m_capacity; }

private:
    std::vector<float> m_buffer;
    size_t m_capacity{0};
    size_t m_mask{0};

    // Cache-line aligned write and read heads to eliminate false sharing
    alignas(64) std::atomic<size_t> m_writeHead{0};
    alignas(64) std::atomic<size_t> m_readHead{0};
};

/**
 * @brief High-precision chromatic instrument tuner using decoupled YIN pitch detection.
 * Audio thread ingests samples wait-free; 60 Hz worker thread computes pitch detection.
 */
class InstrumentTuner {
public:
    explicit InstrumentTuner(uint32_t bufferSize = 2048);
    ~InstrumentTuner();

    // Prevent copying and moving (contains thread and atomic primitives)
    InstrumentTuner(const InstrumentTuner&) = delete;
    InstrumentTuner& operator=(const InstrumentTuner&) = delete;
    InstrumentTuner(InstrumentTuner&&) = delete;
    InstrumentTuner& operator=(InstrumentTuner&&) = delete;

    // Initialization & Lifecycle
    void prepare(double sampleRate, uint32_t maxBlockSize = 4096);
    void start();
    void stop();

    // Audio Callback Ingestion (Wait-free, Lock-free, Zero dynamic allocation)
    void pushSamples(const float* monoInput, uint32_t numSamples) noexcept;
    void pushSamples(const float* left, const float* right, uint32_t numSamples) noexcept;

    // Synchronous evaluation fallback (for unit tests and deterministic offline processing)
    void process(const float* monoInput, uint32_t numSamples);

    // UI Result Polling (Wait-free atomic snapshot)
    [[nodiscard]] TunerResult currentResult() const;

    [[nodiscard]] bool isEnabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }
    void setEnabled(bool enabled) noexcept { m_enabled.store(enabled, std::memory_order_relaxed); }

    void setA4Reference(float freqHz) noexcept {
        float clamped = std::clamp(freqHz, 300.0f, 600.0f);
        m_a4Reference.store(clamped, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config.a4ReferenceHz = clamped;
    }
    [[nodiscard]] float a4Reference() const noexcept {
        return m_a4Reference.load(std::memory_order_relaxed);
    }

    void setConfig(const TunerConfig& cfg) {
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            m_config = cfg;
        }
        m_a4Reference.store(cfg.a4ReferenceHz, std::memory_order_relaxed);
    }
    [[nodiscard]] TunerConfig getConfigSnapshot() const {
        std::lock_guard<std::mutex> lock(m_configMutex);
        return m_config;
    }
    [[nodiscard]] const TunerConfig& config() const noexcept { return m_config; }
    [[nodiscard]] TunerConfig& config() noexcept { return m_config; }

private:
    void workerLoop();
    void detectPitchYin();
    void publishResult(bool hasPitch, float freq, int note, float cents);

    uint32_t m_bufferSize;
    uint32_t m_maxBlockSize{4096};
    double m_sampleRate{48000.0};
    std::atomic<bool> m_enabled{true};
    std::atomic<float> m_a4Reference{440.0f};
    mutable std::mutex m_configMutex;
    TunerConfig m_config;

    // Lock-free ingestion ring buffer
    AudioRingBuffer m_ringBuffer;

    // Background worker thread state
    std::thread m_workerThread;
    std::atomic<bool> m_workerRunning{false};
    std::condition_variable m_workerCv;
    std::mutex m_workerCvMutex; // Used exclusively for worker cv sleep/shutdown, NEVER on audio thread

    // Internal scratch and history buffers (pre-allocated in prepare())
    std::vector<float> m_inputHistory;
    uint32_t m_writeIndex{0};
    std::vector<float> m_drainBuffer;
    std::vector<float> m_differenceBuffer;
    std::vector<float> m_cumulativeDiffBuffer;
    std::vector<float> m_linearWindow;

    // Thread-safe atomic result snapshotting (Seqlock + Atomics)
    alignas(64) std::atomic<uint32_t> m_resultSeq{0};
    std::atomic<float> m_detectedFreq{0.0f};
    std::atomic<float> m_detectedCents{0.0f};
    std::atomic<int> m_detectedMidiNote{0};
    std::atomic<bool> m_hasPitch{false};

    float m_yinThreshold{0.15f};
};

} // namespace praccy::tools
