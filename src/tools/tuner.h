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

namespace praccy::tools {

struct TunerResult {
    float frequencyHz{0.0f};
    int noteNumber{0};         // MIDI note number (69 = A4 = 440Hz)
    std::string noteName;      // e.g. "E2", "A2", "D3", "G3", "B3", "E4"
    float centDeviation{0.0f}; // [-50.0, +50.0] cents
    bool confidence{false};
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

private:
    void workerLoop();
    void detectPitchYin();
    void publishResult(bool hasPitch, float freq, int note, float cents);

    uint32_t m_bufferSize;
    uint32_t m_maxBlockSize{4096};
    double m_sampleRate{48000.0};
    std::atomic<bool> m_enabled{true};

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

    // Thread-safe atomic result snapshotting (Seqlock + Atomics)
    alignas(64) std::atomic<uint32_t> m_resultSeq{0};
    std::atomic<float> m_detectedFreq{0.0f};
    std::atomic<float> m_detectedCents{0.0f};
    std::atomic<int> m_detectedMidiNote{0};
    std::atomic<bool> m_hasPitch{false};

    float m_yinThreshold{0.15f};
};

} // namespace praccy::tools
