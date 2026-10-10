#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include <numbers>
#include <thread>
#include <atomic>
#include <chrono>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <random>

#include "audio/audio_buffer.h"
#include "audio/dsp_utils.h"
#include "audio/graph_engine.h"
#include "audio/asio_manager.h"
#include "plugins/builtin_dsp.h"
#include "tools/tuner.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

using namespace praccy;

// ============================================================================
// Real-time Heap Allocation Tracking Hooks
// ============================================================================
thread_local bool t_isAudioThread = false;
static std::atomic<uint64_t> g_audioThreadAllocations{0};
static std::atomic<uint64_t> g_audioThreadDeallocations{0};

void* operator new(size_t size) {
    if (t_isAudioThread) {
        g_audioThreadAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept {
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t size) noexcept {
    (void)size;
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void* operator new[](size_t size) {
    if (t_isAudioThread) {
        g_audioThreadAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete[](void* p) noexcept {
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, std::size_t size) noexcept {
    (void)size;
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

// Tracked effect node for destructor verification
class DtorTrackedNode : public audio::AudioNode {
public:
    DtorTrackedNode(std::atomic<uint32_t>& badDtorCount, const std::atomic<std::thread::id>& audioThreadId, std::atomic<uint32_t>& totalDtorCount)
        : m_badDtor(badDtorCount), m_audioId(audioThreadId), m_totalDtor(totalDtorCount) {}

    ~DtorTrackedNode() override {
        m_totalDtor.fetch_add(1, std::memory_order_relaxed);
        if (std::this_thread::get_id() == m_audioId.load(std::memory_order_relaxed)) {
            m_badDtor.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void prepare(double, uint32_t) override {}
    void process(audio::AudioProcessContext& ctx) override { ctx.output.copyFrom(ctx.input); }
    void reset() override {}
    [[nodiscard]] audio::NodeType type() const noexcept override { return audio::NodeType::Plugin; }
    [[nodiscard]] const std::string& name() const noexcept override {
        static const std::string n = "DtorTrackedNode";
        return n;
    }

private:
    std::atomic<uint32_t>& m_badDtor;
    const std::atomic<std::thread::id>& m_audioId;
    std::atomic<uint32_t>& m_totalDtor;
};

// ============================================================================
// Challenge 1: Concurrency, High-Contention, Queue Overflow & Deadlock Testing
// ============================================================================

void stressTestParallelBranchConcurrency() {
    std::cout << "[CHALLENGE 1A] High-contention ParallelBranch concurrency stress test... \n";

    audio::GraphEngine engine;
    constexpr uint32_t blockSize = 128;
    constexpr double sampleRate = 48000.0;
    engine.prepare(sampleRate, blockSize);

    auto splitBlock = std::make_unique<audio::ParallelSplitMergeBlock>("Stress Split");
    auto* branchA = splitBlock->addBranch("Branch A");
    auto* branchB = splitBlock->addBranch("Branch B");
    engine.addSerialNode(std::move(splitBlock));

    std::atomic<bool> running{true};
    std::atomic<uint32_t> badDtorCount{0};
    std::atomic<uint32_t> totalDtorCount{0};
    std::atomic<std::thread::id> audioThreadId{};
    std::atomic<uint64_t> audioBlocksProcessed{0};

    // Audio thread
    std::thread audioThread([&]() {
        audioThreadId.store(std::this_thread::get_id(), std::memory_order_release);
        t_isAudioThread = true;

        audio::OwnedAudioBuffer inBuf(2, blockSize);
        audio::OwnedAudioBuffer outBuf(2, blockSize);
        auto inView = inBuf.view(blockSize);
        auto outView = outBuf.view(blockSize);

        for (uint32_t s = 0; s < blockSize; ++s) {
            inView.channel(0)[s] = 0.1f;
            inView.channel(1)[s] = -0.1f;
        }

        while (running.load(std::memory_order_relaxed)) {
            outView.clear();
            engine.process(inView, outView);
            audioBlocksProcessed.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }

        t_isAudioThread = false;
    });

    // Wait until audio thread is running
    while (audioThreadId.load(std::memory_order_acquire) == std::thread::id{}) {
        std::this_thread::yield();
    }

    // UI thread: 1500 iterations of rapid add/remove/mutation
    constexpr int iterations = 1500;
    for (int i = 0; i < iterations; ++i) {
        auto trackedA = std::make_unique<DtorTrackedNode>(badDtorCount, audioThreadId, totalDtorCount);
        auto trackedB = std::make_unique<DtorTrackedNode>(badDtorCount, audioThreadId, totalDtorCount);

        branchA->addSlot(std::make_unique<audio::PluginSlot>(std::move(trackedA)));
        branchB->addSlot(std::make_unique<audio::PluginSlot>(std::move(trackedB)));

        // Readouts
        size_t na = branchA->numSlots();
        size_t nb = branchB->numSlots();
        if (na > 0) {
            auto* s = branchA->getSlot(na - 1);
            if (s) {
                s->setDryWet(0.7f);
                s->setBypassed((i % 4) == 0);
            }
        }
        if (nb > 0) {
            auto* s = branchB->getSlot(nb - 1);
            if (s) {
                s->setInputGainDb(static_cast<float>((i % 6) - 3));
            }
        }

        branchA->setGainDb(static_cast<float>((i % 12) - 6));
        branchB->setPan(static_cast<float>((i % 10) - 5) / 5.0f);

        // Every 3 iterations, remove slots
        if (branchA->numSlots() > 2) branchA->removeSlot(0);
        if (branchB->numSlots() > 2) branchB->removeSlot(0);

        // Periodically reclaim
        if ((i % 10) == 0) {
            engine.processReclamation();
        }
    }

    // Cleanup
    while (branchA->numSlots() > 0) branchA->removeSlot(0);
    while (branchB->numSlots() > 0) branchB->removeSlot(0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_release);
    audioThread.join();

    // Final reclamation sweep
    engine.processReclamation();

    std::cout << "  Audio blocks processed: " << audioBlocksProcessed.load() << "\n";
    std::cout << "  Total destructors run: " << totalDtorCount.load() << "\n";
    std::cout << "  Audio-thread destructors: " << badDtorCount.load() << "\n";

    assert(badDtorCount.load() == 0 && "CRITICAL BUG: PluginSlot destructor executed on audio thread!");
    assert(audioBlocksProcessed.load() > 1000);
    std::cout << "  -> PASSED: Zero plugin destructions on audio thread under 1500 contention cycles.\n";
}

void testCommandQueueSaturationDanglingPointer() {
    std::cout << "[CHALLENGE 1B] Command Queue Saturation & Dangling Pointer Attack... \n";

    // Create a branch directly without an audio consumer draining it
    audio::ParallelBranch branch("Isolated Branch");
    branch.prepare(48000.0, 128);

    std::atomic<uint32_t> badDtorCount{0};
    std::atomic<uint32_t> totalDtorCount{0};
    std::atomic<std::thread::id> dummyId{std::thread::id{}};

    // SPSC Command Queue capacity is 64 (block size 128).
    // Try adding 300 slots without calling branch.process()
    std::vector<audio::PluginSlot*> rawPointersAdded;
    for (int i = 0; i < 300; ++i) {
        auto node = std::make_unique<DtorTrackedNode>(badDtorCount, dummyId, totalDtorCount);
        auto slot = std::make_unique<audio::PluginSlot>(std::move(node));
        audio::PluginSlot* raw = slot.get();
        rawPointersAdded.push_back(raw);
        branch.addSlot(std::move(slot));
    }

    size_t reportedSlots = branch.numSlots();
    uint32_t dtorsRunSoFar = totalDtorCount.load();
    std::cout << "  Added 300 slots to isolated branch (capacity 64/128).\n";
    std::cout << "  branch.numSlots() reported: " << reportedSlots << "\n";
    std::cout << "  Slots destructed immediately on queue overflow: " << dtorsRunSoFar << "\n";

    // Check if branch.numSlots() counts slots that failed to enqueue:
    if (reportedSlots > 64 && dtorsRunSoFar > 0) {
        std::cout << "  [EMPIRICALLY CONFIRMED BUG]: " << dtorsRunSoFar 
                  << " slots were DESTRUCTED due to command queue overflow, but branch.numSlots() reports "
                  << reportedSlots << "! m_uiSlots holds dangling pointers!\n";

        // Check if getSlot returns a dangling pointer
        for (size_t i = 64; i < reportedSlots; ++i) {
            audio::PluginSlot* dangling = branch.getSlot(i);
            std::cout << "  Slot[" << i << "] pointer in m_uiSlots: " << dangling << " (already freed!)\n";
        }
    } else {
        std::cout << "  Queue did not overflow as expected or queue capacity is larger.\n";
    }
}

void testReclamationQueueSaturationLeakedSlots() {
    std::cout << "[CHALLENGE 1C] Reclamation Queue Saturation & Leak Testing... \n";

    audio::ParallelBranch branch("Reclaim Test Branch");
    branch.prepare(48000.0, 128);

    audio::OwnedAudioBuffer inBuf(2, 128);
    audio::OwnedAudioBuffer outBuf(2, 128);
    audio::AudioProcessContext ctx{
        .input = inBuf.view(128),
        .output = outBuf.view(128),
        .sampleRate = 48000.0,
        .numSamples = 128
    };

    // Add 30 slots (under max branch slots 32)
    for (int i = 0; i < 30; ++i) {
        branch.addSlot(std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>()));
    }

    // Drain into active slots
    branch.process(ctx);

    // Now remove 30 slots
    for (int i = 0; i < 30; ++i) {
        branch.removeSlot(0);
    }

    // Process removal on audio thread
    branch.process(ctx);

    // Call reclamation
    branch.collectReclaimedSlots();

    std::cout << "  Removed 30 slots and processed reclamation. UI slots: " << branch.numSlots() << "\n";
    assert(branch.numSlots() == 0);
    std::cout << "  -> PASSED.\n";
}

// ============================================================================
// Challenge 2: Sample Unpack/Pack Numerical Fidelity & Edge Cases
// ============================================================================

void testFormat24BitNumericalFidelityAndEdgeCases() {
    std::cout << "[CHALLENGE 2] Numerical Edge Case & Fidelity Testing for 24-Bit Formats... \n";

    // 1. Bit-Exact Round-Trip Exhaustive Vector Sweep for ASIOSTInt24LSB
    {
        std::cout << "  Part 1: ASIOSTInt24LSB Round-Trip Sweep... ";
        // Test key boundaries
        std::vector<int32_t> testInts = {
            0,
            1, -1,
            2, -2,
            8388607,  // Max positive: 0x7FFFFF
            -8388608, // Max negative: 0x800000
            4194304,  // +0.5
            -4194304, // -0.5
            2097152,  // +0.25
            -2097152, // -0.25
            1234567,
            -1234567,
            0x007FFF,
            static_cast<int32_t>(0xFFFF8000),
            0x0000FF,
            static_cast<int32_t>(0xFFFFFF00)
        };

        // Add 10,000 pseudo-random 24-bit signed integers
        std::mt19937 rng(42);
        std::uniform_int_distribution<int32_t> dist(-8388608, 8388607);
        for (int i = 0; i < 10000; ++i) {
            testInts.push_back(dist(rng));
        }

        const uint32_t N = static_cast<uint32_t>(testInts.size());
        std::vector<uint8_t> rawInput(N * 3);
        for (uint32_t i = 0; i < N; ++i) {
            int32_t val = testInts[i];
            rawInput[i * 3 + 0] = static_cast<uint8_t>(val & 0xFF);
            rawInput[i * 3 + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
            rawInput[i * 3 + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
        }

        std::vector<float> unpacked(N);
        audio::AsioManager::unpackInt24LSB(rawInput.data(), unpacked.data(), N);

        std::vector<uint8_t> packedOutput(N * 3);
        audio::AsioManager::packInt24LSB(unpacked.data(), packedOutput.data(), N);

        // Verify 100% BIT-EXACT roundtrip
        uint32_t mismatches = 0;
        for (uint32_t i = 0; i < N; ++i) {
            uint8_t in0 = rawInput[i * 3 + 0];
            uint8_t in1 = rawInput[i * 3 + 1];
            uint8_t in2 = rawInput[i * 3 + 2];
            uint8_t out0 = packedOutput[i * 3 + 0];
            uint8_t out1 = packedOutput[i * 3 + 1];
            uint8_t out2 = packedOutput[i * 3 + 2];

            if (in0 != out0 || in1 != out1 || in2 != out2) {
                if (++mismatches <= 5) {
                    std::cerr << "\n    Mismatch at sample " << i << ": original (" 
                              << (int)in0 << "," << (int)in1 << "," << (int)in2 << ") != packed ("
                              << (int)out0 << "," << (int)out1 << "," << (int)out2 << ") float=" << unpacked[i];
                }
            }
        }
        assert(mismatches == 0 && "Bit-exact round trip failed for ASIOSTInt24LSB!");
        std::cout << "PASSED (" << N << " vectors 100% bit-exact)\n";
    }

    // 2. Bit-Exact Round-Trip Exhaustive Vector Sweep for ASIOSTInt32LSB24
    {
        std::cout << "  Part 2: ASIOSTInt32LSB24 Round-Trip Sweep with High-Byte DMA Noise... ";
        std::vector<int32_t> testInts = {
            0, 1, -1, 8388607, -8388608, 4194304, -4194304
        };

        std::mt19937 rng(1337);
        std::uniform_int_distribution<int32_t> dist(-8388608, 8388607);
        std::uniform_int_distribution<int32_t> noiseDist(0, 255);
        for (int i = 0; i < 10000; ++i) {
            testInts.push_back(dist(rng));
        }

        const uint32_t N = static_cast<uint32_t>(testInts.size());
        std::vector<int32_t> rawInput(N);
        for (uint32_t i = 0; i < N; ++i) {
            int32_t val24 = testInts[i] & 0x00FFFFFF;
            int32_t noise = (noiseDist(rng) << 24);
            rawInput[i] = val24 | noise; // inject garbage into high byte
        }

        std::vector<float> unpacked(N);
        audio::AsioManager::unpackInt32LSB24(rawInput.data(), unpacked.data(), N);

        std::vector<int32_t> packedOutput(N);
        audio::AsioManager::packInt32LSB24(unpacked.data(), packedOutput.data(), N);

        uint32_t mismatches = 0;
        for (uint32_t i = 0; i < N; ++i) {
            int32_t expected24 = testInts[i] & 0x00FFFFFF;
            int32_t actual24 = packedOutput[i] & 0x00FFFFFF;
            if (expected24 != actual24) {
                if (++mismatches <= 5) {
                    std::cerr << "\n    Mismatch at sample " << i << ": expected 0x" << std::hex << expected24
                              << " != actual 0x" << actual24 << std::dec;
                }
            }
        }
        assert(mismatches == 0 && "Bit-exact round trip failed for ASIOSTInt32LSB24!");
        std::cout << "PASSED (" << N << " vectors 100% bit-exact with noise immunity)\n";
    }

    // 3. Numerical Extreme Inputs (Clamping, Denormals, Infinities, NaNs)
    {
        std::cout << "  Part 3: Extreme Inputs, Denormals, Infs, and NaNs... \n";

        constexpr uint32_t count = 10;
        float extremeInputs[count] = {
            +1000.0f,
            -1000.0f,
            +1.5f,
            -1.5f,
            std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity(),
            std::numeric_limits<float>::denorm_min(), // Denormal / subnormal
            -std::numeric_limits<float>::denorm_min(),
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::signaling_NaN()
        };

        // Test packInt24LSB
        uint8_t packed24[count * 3] = {};
        audio::AsioManager::packInt24LSB(extremeInputs, packed24, count);

        // Verify +1000.0f and +1.5f clamped to +8,388,607 (0xFF, 0xFF, 0x7F)
        assert(packed24[0] == 0xFF && packed24[1] == 0xFF && packed24[2] == 0x7F);
        assert(packed24[6] == 0xFF && packed24[7] == 0xFF && packed24[8] == 0x7F);

        // Verify -1000.0f and -1.5f clamped to -8,388,608 (0x00, 0x00, 0x80)
        assert(packed24[3] == 0x00 && packed24[4] == 0x00 && packed24[5] == 0x80);
        assert(packed24[9] == 0x00 && packed24[10] == 0x00 && packed24[11] == 0x80);

        // Verify +Inf and -Inf clamped
        assert(packed24[12] == 0xFF && packed24[13] == 0xFF && packed24[14] == 0x7F);
        assert(packed24[15] == 0x00 && packed24[16] == 0x00 && packed24[17] == 0x80);

        // Verify Denormals round to 0
        assert(packed24[18] == 0x00 && packed24[19] == 0x00 && packed24[20] == 0x00);
        assert(packed24[21] == 0x00 && packed24[22] == 0x00 && packed24[23] == 0x00);

        // Observe NaN behavior
        std::cout << "    Quiet NaN packed bytes: 0x" << std::hex
                  << (int)packed24[24] << " 0x" << (int)packed24[25] << " 0x" << (int)packed24[26] << std::dec << "\n";
        std::cout << "    Signaling NaN packed bytes: 0x" << std::hex
                  << (int)packed24[27] << " 0x" << (int)packed24[28] << " 0x" << (int)packed24[29] << std::dec << "\n";

        // Test packInt32LSB24
        int32_t packed32_24[count] = {};
        audio::AsioManager::packInt32LSB24(extremeInputs, packed32_24, count);
        assert((packed32_24[0] & 0x00FFFFFF) == 0x007FFFFF); // +1000 clamped
        assert((packed32_24[1] & 0x00FFFFFF) == 0x00800000); // -1000 clamped
        assert((packed32_24[4] & 0x00FFFFFF) == 0x007FFFFF); // +Inf clamped
        assert((packed32_24[5] & 0x00FFFFFF) == 0x00800000); // -Inf clamped
        assert(packed32_24[6] == 0);                         // Denorm rounds to 0
        assert(packed32_24[7] == 0);

        std::cout << "    Quiet NaN in Int32LSB24: 0x" << std::hex << (packed32_24[8] & 0x00FFFFFF) << std::dec << "\n";

        std::cout << "  -> PASSED: Clamping, denormals, and infinities handled safely without crash or arithmetic overflow.\n";
    }
}

// ============================================================================
// Challenge 3: Real-Time Audio Callback Thread Zero-Allocation Verification
// ============================================================================

void testAudioThreadZeroHeapAllocations() {
    std::cout << "[CHALLENGE 3] Real-Time Zero Heap Allocation Verification... \n";

    constexpr uint32_t blockSize = 128;
    constexpr double sampleRate = 48000.0;

    // 1. Prepare entire graph and all subsystems ON THE CONTROL THREAD
    audio::GraphEngine engine;
    engine.prepare(sampleRate, blockSize);

    auto splitBlock = std::make_unique<audio::ParallelSplitMergeBlock>("ZeroAlloc Split");
    auto* branchA = splitBlock->addBranch("Branch A");
    auto* branchB = splitBlock->addBranch("Branch B");

    branchA->addSlot(std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>()));
    branchB->addSlot(std::make_unique<audio::PluginSlot>(std::make_unique<plugins::StereoDelayEffect>()));

    engine.addSerialNode(std::move(splitBlock));

    tools::InstrumentTuner tuner(2048);
    tuner.prepare(sampleRate, blockSize);

    audio::OwnedAudioBuffer inBuf(2, blockSize);
    audio::OwnedAudioBuffer outBuf(2, blockSize);
    auto inView = inBuf.view(blockSize);
    auto outView = outBuf.view(blockSize);

    // Scratch conversion buffers for ASIO pack/unpack
    std::vector<uint8_t> raw24In(blockSize * 3, 0x12);
    std::vector<uint8_t> raw24Out(blockSize * 3, 0);
    std::vector<int32_t> raw32_24In(blockSize, 0x00123456);
    std::vector<int32_t> raw32_24Out(blockSize, 0);

    // Initial warm-up block on main thread to ensure any lazy static init is done
    engine.process(inView, outView);

    // Reset allocation counters
    g_audioThreadAllocations.store(0, std::memory_order_relaxed);
    g_audioThreadDeallocations.store(0, std::memory_order_relaxed);

    std::atomic<bool> audioStreaming{true};
    std::atomic<uint64_t> streamBlocks{0};

    // Spawn dedicated simulated audio callback thread
    std::thread audioThread([&]() {
        t_isAudioThread = true;

        while (audioStreaming.load(std::memory_order_relaxed)) {
            // Unpack 24-bit inputs
            audio::AsioManager::unpackInt24LSB(raw24In.data(), inView.channel(0), blockSize);
            audio::AsioManager::unpackInt32LSB24(raw32_24In.data(), inView.channel(1), blockSize);

            // Ingest into Tuner (Mono & Stereo)
            tuner.pushSamples(inView.channel(0), blockSize);
            tuner.pushSamples(inView.channel(0), inView.channel(1), blockSize);

            // Process full audio DSP graph (noise gate, serial chain, parallel branches, limiter)
            outView.clear();
            engine.process(inView, outView);

            // Pack 24-bit outputs
            audio::AsioManager::packInt24LSB(outView.channel(0), raw24Out.data(), blockSize);
            audio::AsioManager::packInt32LSB24(outView.channel(1), raw32_24Out.data(), blockSize);

            streamBlocks.fetch_add(1, std::memory_order_relaxed);
        }

        t_isAudioThread = false;
    });

    // Let the audio thread stream 10,000 full DSP blocks
    while (streamBlocks.load(std::memory_order_relaxed) < 10000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    audioStreaming.store(false, std::memory_order_release);
    audioThread.join();

    uint64_t allocs = g_audioThreadAllocations.load();
    uint64_t deallocs = g_audioThreadDeallocations.load();

    std::cout << "  Audio blocks streamed: " << streamBlocks.load() << "\n";
    std::cout << "  Heap allocations on audio thread: " << allocs << "\n";
    std::cout << "  Heap deallocations on audio thread: " << deallocs << "\n";

    assert(allocs == 0 && "CRITICAL VIOLATION: Dynamic heap allocation detected on real-time audio thread!");
    assert(deallocs == 0 && "CRITICAL VIOLATION: Dynamic heap deallocation detected on real-time audio thread!");

    std::cout << "  -> PASSED: ZERO heap allocations and ZERO heap deallocations across 10,000 audio blocks.\n";
}

// ============================================================================
// Challenge 4: InstrumentTuner Seqlock & Multi-threaded Stress
// ============================================================================

void testInstrumentTunerSeqlockStress() {
    std::cout << "[CHALLENGE 4] InstrumentTuner Multi-threaded Seqlock Stress Test... \n";

    tools::InstrumentTuner tuner(2048);
    tuner.prepare(48000.0, 256);

    std::atomic<bool> running{true};
    std::atomic<uint64_t> readsCompleted{0};
    std::atomic<uint64_t> invalidStates{0};

    // Thread 1: Ingest continuous 440 Hz sine wave
    std::thread ingester([&]() {
        std::vector<float> sine(256);
        uint64_t sampleCount = 0;
        while (running.load(std::memory_order_relaxed)) {
            for (size_t i = 0; i < sine.size(); ++i) {
                double t = static_cast<double>(sampleCount++) / 48000.0;
                sine[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * t) * 0.7);
            }
            tuner.pushSamples(sine.data(), 256);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    });

    // Threads 2 & 3: Concurrent UI readers polling currentResult() at high frequency
    auto readerFn = [&]() {
        while (running.load(std::memory_order_relaxed)) {
            auto r = tuner.currentResult();
            readsCompleted.fetch_add(1, std::memory_order_relaxed);
            if (r.confidence) {
                // If confident, cents deviation must be in valid range [-50, +50]
                if (r.centDeviation < -50.1f || r.centDeviation > 50.1f || r.noteName.empty()) {
                    invalidStates.fetch_add(1, std::memory_order_relaxed);
                }
            }
            std::this_thread::yield();
        }
    };

    std::thread reader1(readerFn);
    std::thread reader2(readerFn);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    running.store(false, std::memory_order_release);

    ingester.join();
    reader1.join();
    reader2.join();

    std::cout << "  Total UI reads completed: " << readsCompleted.load() << "\n";
    std::cout << "  Invalid/torn states observed: " << invalidStates.load() << "\n";

    assert(invalidStates.load() == 0 && "CRITICAL: Torn or inconsistent result read from tuner seqlock!");
    assert(readsCompleted.load() > 10000);
    std::cout << "  -> PASSED: Seqlock snapshotting is tear-free and wait-free under high reader contention.\n";
}

void testTunerPrepareRace() {
    std::cout << "[CHALLENGE 5] InstrumentTuner prepare() vs workerLoop() Concurrency Race... \n";

    tools::InstrumentTuner tuner(2048);
    // Worker is already started in constructor!

    std::atomic<bool> running{true};
    std::atomic<uint64_t> pushesDone{0};

    // Thread 1: Audio callback pushing samples
    std::thread audioPusher([&]() {
        std::vector<float> block(256, 0.5f);
        while (running.load(std::memory_order_relaxed)) {
            tuner.pushSamples(block.data(), 256);
            pushesDone.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Control thread: Repeatedly calls prepare() while audio pusher and worker are active
    for (int i = 0; i < 50; ++i) {
        tuner.prepare(48000.0, 256 + (i % 4) * 64);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    running.store(false, std::memory_order_release);
    audioPusher.join();

    std::cout << "  Completed 50 prepare() calls while streaming. Pushes: " << pushesDone.load() << "\n";
    std::cout << "  -> Finished without crashing immediately, but data race exists on m_buffer reallocation.\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "   PRACCY M1 EMPIRICAL CHALLENGER STRESS HARNESS        \n";
    std::cout << "========================================================\n";

    stressTestParallelBranchConcurrency();
    testCommandQueueSaturationDanglingPointer();
    testReclamationQueueSaturationLeakedSlots();
    testFormat24BitNumericalFidelityAndEdgeCases();
    testAudioThreadZeroHeapAllocations();
    testInstrumentTunerSeqlockStress();
    testTunerPrepareRace();

    std::cout << "========================================================\n";
    std::cout << "   ALL EMPIRICAL CHALLENGE TESTS EXECUTED               \n";
    std::cout << "========================================================\n";
    return 0;
}
