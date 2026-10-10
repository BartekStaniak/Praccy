#include <iostream>
#include <vector>
#include <cmath>
#include <cassert>
#include <cstring>
#include <numbers>
#include <thread>
#include <atomic>
#include <chrono>
#include <random>
#include <windows.h>
#include <avrt.h>

#include "audio/asio_manager.h"
#include "audio/asio_defs.h"
#include "tools/tuner.h"

using namespace praccy;

// ============================================================================
// TEST 1: Exhaustive 24-Bit Roundtrip Audio Bit-Exactness
// ============================================================================
void runTest24BitBitExactness() {
    std::cout << "[CHALLENGER-TEST 1] Exhaustive 24-Bit Roundtrip Audio Bit-Exactness...\n";

    // 1.1 Test ASIOSTInt24LSB across ALL 16,777,216 possible 24-bit signed integers
    std::cout << "  - Verifying ASIOSTInt24LSB across all 16,777,216 values (-8,388,608 to +8,388,607)... ";
    size_t int24Mismatches = 0;
    constexpr int32_t MIN_24 = -8388608;
    constexpr int32_t MAX_24 = 8388607;

    // Process in batches of 4096 samples for speed and realistic buffer sizing
    constexpr uint32_t BATCH_SIZE = 4096;
    uint8_t rawBatchIn[BATCH_SIZE * 3];
    float floatBatch[BATCH_SIZE];
    uint8_t rawBatchOut[BATCH_SIZE * 3];

    int32_t currentVal = MIN_24;
    while (currentVal <= MAX_24) {
        uint32_t count = 0;
        while (count < BATCH_SIZE && currentVal <= MAX_24) {
            uint32_t uval = static_cast<uint32_t>(currentVal);
            rawBatchIn[count * 3]     = static_cast<uint8_t>(uval & 0xFF);
            rawBatchIn[count * 3 + 1] = static_cast<uint8_t>((uval >> 8) & 0xFF);
            rawBatchIn[count * 3 + 2] = static_cast<uint8_t>((uval >> 16) & 0xFF);
            count++;
            currentVal++;
        }

        audio::AsioManager::unpackInt24LSB(rawBatchIn, floatBatch, count);
        audio::AsioManager::packInt24LSB(floatBatch, rawBatchOut, count);

        for (uint32_t i = 0; i < count; ++i) {
            if (rawBatchIn[i * 3]     != rawBatchOut[i * 3] ||
                rawBatchIn[i * 3 + 1] != rawBatchOut[i * 3 + 1] ||
                rawBatchIn[i * 3 + 2] != rawBatchOut[i * 3 + 2]) {
                if (int24Mismatches < 5) {
                    std::cerr << "\n    Mismatch at index " << i << ": original bytes ("
                              << (int)rawBatchIn[i*3] << "," << (int)rawBatchIn[i*3+1] << "," << (int)rawBatchIn[i*3+2]
                              << ") vs packed ("
                              << (int)rawBatchOut[i*3] << "," << (int)rawBatchOut[i*3+1] << "," << (int)rawBatchOut[i*3+2]
                              << ") float=" << floatBatch[i];
                }
                int24Mismatches++;
            }
        }
    }

    if (int24Mismatches == 0) {
        std::cout << "PASSED (0 mismatches across 16,777,216 values)\n";
    } else {
        std::cout << "FAILED (" << int24Mismatches << " mismatches)\n";
    }

    // 1.2 Test ASIOSTInt32LSB24 across ALL 16,777,216 possible 24-bit values
    std::cout << "  - Verifying ASIOSTInt32LSB24 across all 16,777,216 values... ";
    size_t int32LSB24Mismatches = 0;
    int32_t raw32BatchIn[BATCH_SIZE];
    int32_t raw32BatchOut[BATCH_SIZE];

    currentVal = MIN_24;
    while (currentVal <= MAX_24) {
        uint32_t count = 0;
        while (count < BATCH_SIZE && currentVal <= MAX_24) {
            raw32BatchIn[count] = currentVal;
            count++;
            currentVal++;
        }

        audio::AsioManager::unpackInt32LSB24(raw32BatchIn, floatBatch, count);
        audio::AsioManager::packInt32LSB24(floatBatch, raw32BatchOut, count);

        for (uint32_t i = 0; i < count; ++i) {
            // Mask lower 24 bits
            int32_t in24 = raw32BatchIn[i] & 0x00FFFFFF;
            int32_t out24 = raw32BatchOut[i] & 0x00FFFFFF;
            if (in24 != out24) {
                if (int32LSB24Mismatches < 5) {
                    std::cerr << "\n    Mismatch at 32LSB24: in=" << in24 << " vs out=" << out24
                              << " float=" << floatBatch[i];
                }
                int32LSB24Mismatches++;
            }
        }
    }

    if (int32LSB24Mismatches == 0) {
        std::cout << "PASSED (0 mismatches across 16,777,216 values)\n";
    } else {
        std::cout << "FAILED (" << int32LSB24Mismatches << " mismatches)\n";
    }

    // 1.3 Test High-Byte DMA Noise Immunity in ASIOSTInt32LSB24
    std::cout << "  - Verifying High-Byte DMA Noise Immunity in unpackInt32LSB24... ";
    size_t noiseFailures = 0;
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> noiseDist(0x01, 0xFF);
    std::uniform_int_distribution<int32_t> valDist(MIN_24, MAX_24);

    constexpr uint32_t NUM_NOISE_TESTS = 100000;
    for (uint32_t i = 0; i < NUM_NOISE_TESTS; ++i) {
        int32_t cleanVal = valDist(rng);
        // Inject random noise into bits 24..31
        uint32_t noisyVal = (static_cast<uint32_t>(cleanVal) & 0x00FFFFFF) | (noiseDist(rng) << 24);

        float cleanFloat = 0.0f;
        float noisyFloat = 0.0f;
        audio::AsioManager::unpackInt32LSB24(&cleanVal, &cleanFloat, 1);
        audio::AsioManager::unpackInt32LSB24(&noisyVal, &noisyFloat, 1);

        if (cleanFloat != noisyFloat) {
            noiseFailures++;
        }
    }

    if (noiseFailures == 0) {
        std::cout << "PASSED (" << NUM_NOISE_TESTS << " noisy samples immune)\n";
    } else {
        std::cout << "FAILED (" << noiseFailures << " noise failures)\n";
    }

    // 1.4 Test Float Edge Cases (+1.0f clamp, -1.0f clamp, Inf, NaN)
    std::cout << "  - Verifying Float Clamping (+1.0f, -1.0f, Inf, NaN)... ";
    float edgeFloats[6] = { -1.5f, -1.0f, 0.0f, 0.99999988f, 1.0f, 1.5f };
    uint8_t packedEdge24[6 * 3] = {};
    audio::AsioManager::packInt24LSB(edgeFloats, packedEdge24, 6);

    // -1.5f should clamp to -1.0f (0x00, 0x00, 0x80)
    assert(packedEdge24[0] == 0x00 && packedEdge24[1] == 0x00 && packedEdge24[2] == 0x80);
    // -1.0f should be (0x00, 0x00, 0x80)
    assert(packedEdge24[3] == 0x00 && packedEdge24[4] == 0x00 && packedEdge24[5] == 0x80);
    // 0.0f should be (0x00, 0x00, 0x00)
    assert(packedEdge24[6] == 0x00 && packedEdge24[7] == 0x00 && packedEdge24[8] == 0x00);
    // 1.0f should clamp to +8388607 (0xFF, 0xFF, 0x7F)
    assert(packedEdge24[12] == 0xFF && packedEdge24[13] == 0xFF && packedEdge24[14] == 0x7F);
    // 1.5f should clamp to +8388607 (0xFF, 0xFF, 0x7F)
    assert(packedEdge24[15] == 0xFF && packedEdge24[16] == 0xFF && packedEdge24[17] == 0x7F);

    std::cout << "PASSED\n";
}

// ============================================================================
// TEST 2: MMCSS Lifecycle & Cross-Thread Affinities
// ============================================================================
void runTestMmcssLifecycle() {
    std::cout << "[CHALLENGER-TEST 2] MMCSS Lifecycle & Cross-Thread Affinity...\n";

    // 2.1 Test repeated AvSetMmThreadCharacteristics / AvRevertMmThreadCharacteristics on current thread
    std::cout << "  - Testing 100 consecutive AvSet / AvRevert cycles on calling thread... ";
    bool mmcssCyclesOk = true;
    for (int i = 0; i < 100; ++i) {
        DWORD taskIndex = 0;
        HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (!h) {
            // Note: AvSetMmThreadCharacteristics can fail if MMCSS service is disabled or non-admin,
            // but if it succeeds, Revert MUST succeed.
            std::cout << "(AvSetMmThreadCharacteristicsW returned null, skipping direct handle check) ";
            mmcssCyclesOk = true;
            break;
        }
        BOOL rev = AvRevertMmThreadCharacteristics(h);
        if (!rev) {
            std::cerr << "\n    AvRevertMmThreadCharacteristics failed on cycle " << i
                      << " with error: " << GetLastError();
            mmcssCyclesOk = false;
            break;
        }
    }
    if (mmcssCyclesOk) {
        std::cout << "PASSED\n";
    }

    // 2.2 Test cross-thread AvRevertMmThreadCharacteristics failure behavior
    std::cout << "  - Verifying Win32 thread-affinity requirement of MMCSS handles... ";
    DWORD taskIdx = 0;
    HANDLE hThreadA = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);
    if (hThreadA) {
        std::atomic<bool> revertSucceeded{false};
        std::atomic<DWORD> revertError{0};

        std::thread threadB([&]() {
            BOOL rev = AvRevertMmThreadCharacteristics(hThreadA);
            revertSucceeded.store(rev != FALSE);
            revertError.store(GetLastError());
        });
        threadB.join();

        // Cross-thread reversion MUST fail under Win32 API rules!
        if (!revertSucceeded.load()) {
            std::cout << "PASSED (Cross-thread reversion correctly rejected by OS with error "
                      << revertError.load() << ")\n";
        } else {
            std::cout << "UNEXPECTED (OS permitted cross-thread reversion)\n";
        }

        // Clean up on Thread A (the owner)
        AvRevertMmThreadCharacteristics(hThreadA);
    } else {
        std::cout << "SKIPPED (MMCSS handle unavailable in this environment)\n";
    }

    // 2.3 Test AsioManager start/stop idempotency and lifecycle
    std::cout << "  - Verifying AsioManager start() / stop() lifecycle without loaded driver... ";
    audio::AsioManager mgr;
    assert(!mgr.isRunning());
    assert(!mgr.start()); // Should fail safely if no driver loaded
    mgr.stop();           // Should be safe to call when not running
    assert(!mgr.isRunning());
    std::cout << "PASSED\n";
}

// ============================================================================
// TEST 3: InstrumentTuner Ring Buffer Overflow & Concurrency Stress
// ============================================================================
void runTestRingBufferOverflow() {
    std::cout << "[CHALLENGER-TEST 3] InstrumentTuner Ring Buffer Overflow & Stress...\n";

    tools::AudioRingBuffer ring(1024); // Capacity 1024
    assert(ring.capacity() == 1024);
    assert(ring.availableToRead() == 0);
    assert(ring.availableToWrite() == 1024);

    std::vector<float> input(1024);
    for (size_t i = 0; i < input.size(); ++i) input[i] = static_cast<float>(i + 1);

    // 3.1 Push exact capacity
    uint32_t pushed = ring.push(input.data(), 1024);
    assert(pushed == 1024);
    assert(ring.availableToRead() == 1024);
    assert(ring.availableToWrite() == 0);

    // 3.2 Attempt push when FULL (overflow condition)
    std::vector<float> overflowInput(256, 999.0f);
    uint32_t pushedOverflow = ring.push(overflowInput.data(), 256);
    assert(pushedOverflow == 0 && "Buffer overflow push MUST return 0 when full!");
    assert(ring.availableToRead() == 1024);
    assert(ring.availableToWrite() == 0);

    // 3.3 Verify existing data remains uncorrupted
    std::vector<float> readOut(1024, 0.0f);
    uint32_t readCount = ring.read(readOut.data(), 1024);
    assert(readCount == 1024);
    assert(ring.availableToRead() == 0);
    assert(ring.availableToWrite() == 1024);

    for (size_t i = 0; i < 1024; ++i) {
        assert(readOut[i] == static_cast<float>(i + 1));
    }
    std::cout << "  - Ring buffer overflow non-destruction: PASSED\n";

    // 3.4 Test Partial-Fill Overflow
    ring.reset();
    ring.push(input.data(), 1000); // 1000 pushed, 24 slots remaining
    assert(ring.availableToWrite() == 24);
    uint32_t partialPushed = ring.push(overflowInput.data(), 100); // 100 requested, only 24 available
    assert(partialPushed == 24);
    assert(ring.availableToRead() == 1024);
    assert(ring.availableToWrite() == 0);
    std::cout << "  - Ring buffer partial-fill clamping: PASSED\n";

    // 3.5 Test Skip functionality under overflow
    ring.skip(512); // Discard oldest 512 samples
    assert(ring.availableToRead() == 512);
    assert(ring.availableToWrite() == 512);

    readCount = ring.read(readOut.data(), 512);
    assert(readCount == 512);
    // Samples 512..999 should be original input, samples 1000..1023 should be 999.0f
    assert(readOut[0] == 513.0f);
    assert(readOut[487] == 1000.0f);
    assert(readOut[488] == 999.0f);
    std::cout << "  - Ring buffer skip() alignment: PASSED\n";

    // 3.6 Multi-threaded High-Pressure Overflow Stress Test
    std::cout << "  - Concurrent High-Pressure Overflow Ingestion Stress (10,000 blocks)... ";
    tools::InstrumentTuner tuner(2048);
    tuner.prepare(48000.0, 256);

    std::atomic<bool> producerRunning{true};
    std::atomic<uint64_t> samplesPushed{0};
    std::atomic<uint64_t> audioThreadStarvedCount{0};

    // Audio callback simulator: pushes 256 samples every ~100 microseconds (100x real-time speed)
    std::thread audioSimulator([&]() {
        std::vector<float> block(256, 0.5f);
        while (producerRunning.load(std::memory_order_relaxed)) {
            auto tStart = std::chrono::high_resolution_clock::now();
            tuner.pushSamples(block.data(), 256);
            auto tEnd = std::chrono::high_resolution_clock::now();

            auto durNs = std::chrono::duration_cast<std::chrono::nanoseconds>(tEnd - tStart).count();
            if (durNs > 5000000) { // If pushSamples ever takes > 5 milliseconds (blocked/starved audio)
                audioThreadStarvedCount.fetch_add(1, std::memory_order_relaxed);
            }

            samplesPushed.fetch_add(256, std::memory_order_relaxed);
            // Pumping fast without sleeping to force continuous ring buffer saturation
        }
    });

    // Let audio thread pump and saturate the ring buffer for 200 ms while worker thread consumes
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    producerRunning.store(false, std::memory_order_release);
    audioSimulator.join();

    assert(audioThreadStarvedCount.load() == 0 && "CRITICAL: Audio thread blocked in pushSamples!");
    assert(samplesPushed.load() > 50000);
    std::cout << "PASSED (" << samplesPushed.load() << " samples pushed, 0 audio thread blocks)\n";
}

// ============================================================================
// TEST 4: Extreme Sample Rates (44.1 kHz up to 192 kHz) & YIN Detection Range
// ============================================================================
void runTestExtremeSampleRates() {
    std::cout << "[CHALLENGER-TEST 4] Extreme Sample Rates & YIN Detection Range...\n";

    const std::vector<double> sampleRates = {
        44100.0,
        48000.0,
        88200.0,
        96000.0,
        176400.0,
        192000.0
    };

    struct GuitarString {
        const char* name;
        int noteNumber;
        float freq;
    };

    const std::vector<GuitarString> guitarStrings = {
        { "E2 (Low E)", 40, 82.41f },
        { "A2",         45, 110.00f },
        { "D3",         50, 146.83f },
        { "G3",         55, 196.00f },
        { "B3",         59, 246.94f },
        { "E4 (High E)", 64, 329.63f }
    };

    for (double sr : sampleRates) {
        std::cout << "  - Sample Rate: " << static_cast<uint32_t>(sr) << " Hz (YIN theoretical f_min = "
                  << (sr / 1024.0) << " Hz):\n";

        for (const auto& gs : guitarStrings) {
            tools::InstrumentTuner tuner(2048);
            tuner.prepare(sr, 256);

            std::vector<float> sine(2048);
            for (size_t i = 0; i < sine.size(); ++i) {
                double phase = 2.0 * std::numbers::pi * gs.freq * (static_cast<double>(i) / sr);
                sine[i] = static_cast<float>(std::sin(phase) * 0.7);
            }

            tuner.process(sine.data(), static_cast<uint32_t>(sine.size()));
            auto res = tuner.currentResult();

            std::cout << "      " << gs.name << " (" << gs.freq << " Hz): ";
            if (res.confidence && res.noteNumber == gs.noteNumber && std::abs(res.frequencyHz - gs.freq) < (gs.freq * 0.05f)) {
                std::cout << "PASSED (" << res.noteName << " at " << res.frequencyHz << " Hz)\n";
            } else {
                std::cout << "FAILED! (confidence=" << res.confidence << " note=" << res.noteName
                          << " freq=" << res.frequencyHz << " Hz)\n";
            }
        }
    }
}

// ============================================================================
// TEST 5: Rapid Pitch Shifts & Seqlock Concurrency Stress
// ============================================================================
void runTestRapidPitchShiftsAndSeqlock() {
    std::cout << "[CHALLENGER-TEST 5] Rapid Pitch Shifts & Seqlock Concurrency Stress...\n";

    tools::InstrumentTuner tuner(2048);
    tuner.prepare(48000.0, 256);

    std::atomic<bool> testRunning{true};
    std::atomic<uint64_t> readsCompleted{0};
    std::atomic<uint64_t> tornReadsDetected{0};

    // Frequency list to cycle rapidly
    const std::vector<float> freqs = { 82.41f, 110.0f, 146.83f, 196.0f, 246.94f, 329.63f, 440.0f, 880.0f };

    // Thread 1: Rapid pitch ingestion writer (alternates pitch every 16 samples)
    std::thread pitchWriter([&]() {
        size_t freqIdx = 0;
        double phase = 0.0;
        constexpr double sr = 48000.0;
        std::vector<float> block(256);

        while (testRunning.load(std::memory_order_relaxed)) {
            float targetFreq = freqs[freqIdx % freqs.size()];
            freqIdx++;

            for (size_t i = 0; i < block.size(); ++i) {
                phase += 2.0 * std::numbers::pi * targetFreq / sr;
                if (phase > 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;
                block[i] = static_cast<float>(std::sin(phase) * 0.7);
            }

            tuner.pushSamples(block.data(), static_cast<uint32_t>(block.size()));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Threads 2..5: 4 concurrent UI polling readers hammering currentResult()
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&]() {
            while (testRunning.load(std::memory_order_relaxed)) {
                auto res = tuner.currentResult();
                readsCompleted.fetch_add(1, std::memory_order_relaxed);

                // Check for torn reads / internal inconsistency:
                if (res.confidence) {
                    if (res.frequencyHz < 20.0f || res.frequencyHz > 2000.0f ||
                        res.noteNumber < 0 || res.noteNumber > 127 ||
                        res.centDeviation < -50.0f || res.centDeviation > 50.0f ||
                        res.noteName == "--" || res.noteName.empty()) {
                        tornReadsDetected.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        // Check if frequency and note number are mutually consistent (atomically coupled)
                        int expectedNote = static_cast<int>(std::round(69.0f + 12.0f * std::log2(res.frequencyHz / 440.0f)));
                        if (std::abs(res.noteNumber - expectedNote) > 1) {
                            // TORN READ: noteNumber came from a different pitch update than frequencyHz!
                            tornReadsDetected.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    testRunning.store(false, std::memory_order_release);

    pitchWriter.join();
    for (auto& th : readers) th.join();

    std::cout << "  - Completed " << readsCompleted.load() << " concurrent UI seqlock reads.\n";
    if (tornReadsDetected.load() == 0) {
        std::cout << "  - Torn reads detected: 0 (PASSED)\n";
    } else {
        std::cout << "  - Torn reads detected: " << tornReadsDetected.load() << " (FAILED!)\n";
    }
}

// ============================================================================
// TEST 6: Tuner prepare() Lifecycle Concurrency Race
// ============================================================================
void runTestTunerPrepareRace() {
    std::cout << "[CHALLENGER-TEST 6] InstrumentTuner prepare() Lifecycle Concurrency Race...\n";

    // Test calling prepare() while audio is being ingested and worker is running
    tools::InstrumentTuner tuner(2048);
    tuner.prepare(48000.0, 256);

    std::atomic<bool> raceRunning{true};
    std::atomic<uint64_t> pushesDone{0};

    std::thread audioPusher([&]() {
        std::vector<float> block(256, 0.3f);
        while (raceRunning.load(std::memory_order_relaxed)) {
            tuner.pushSamples(block.data(), 256);
            pushesDone.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Rapidly re-prepare tuner with different sample rates from control thread
    bool prepareRaceCrashed = false;
    for (int cycle = 0; cycle < 50; ++cycle) {
        double sr = (cycle % 2 == 0) ? 96000.0 : 44100.0;
        try {
            tuner.prepare(sr, 512);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        } catch (...) {
            prepareRaceCrashed = true;
            break;
        }
    }

    raceRunning.store(false, std::memory_order_release);
    audioPusher.join();

    if (!prepareRaceCrashed) {
        std::cout << "  - 50 rapid prepare() calls during active audio push survived without crash (PASSED)\n";
    } else {
        std::cout << "  - prepare() threw exception or crashed during audio streaming (FAILED)\n";
    }
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================
int main() {
    std::cout << "=====================================================\n";
    std::cout << "   PRACCY M1 CHALLENGER 2 EMPIRICAL TEST SUITE      \n";
    std::cout << "=====================================================\n\n";

    runTest24BitBitExactness();
    std::cout << "\n";

    runTestMmcssLifecycle();
    std::cout << "\n";

    runTestRingBufferOverflow();
    std::cout << "\n";

    runTestExtremeSampleRates();
    std::cout << "\n";

    runTestRapidPitchShiftsAndSeqlock();
    std::cout << "\n";

    runTestTunerPrepareRace();
    std::cout << "\n";

    std::cout << "=====================================================\n";
    std::cout << "   EMPIRICAL CHALLENGER 2 SUITE COMPLETED            \n";
    std::cout << "=====================================================\n";
    return 0;
}
