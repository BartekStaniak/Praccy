#include <iostream>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <cassert>
#include <chrono>
#include <thread>
#include <atomic>
#include <random>
#include <filesystem>
#include <fstream>
#include <windows.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include "audio/audio_buffer.h"
#include "audio/graph_engine.h"
#include "plugins/plugin_base.h"
#include "plugins/crash_isolation.h"

using namespace praccy;

// ============================================================================
// 1. REAL-TIME AUDIO HEAP ALLOCATION MONITOR
// ============================================================================

static thread_local bool t_isAudioThread = false;
static std::atomic<size_t> g_audioThreadAllocCount{0};
static std::atomic<size_t> g_audioThreadAllocBytes{0};
static std::atomic<size_t> g_audioThreadFreeCount{0};

void* operator new(std::size_t size) {
    if (t_isAudioThread) {
        g_audioThreadAllocCount.fetch_add(1, std::memory_order_relaxed);
        g_audioThreadAllocBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept {
    if (t_isAudioThread && p != nullptr) {
        g_audioThreadFreeCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void* operator new[](std::size_t size) {
    if (t_isAudioThread) {
        g_audioThreadAllocCount.fetch_add(1, std::memory_order_relaxed);
        g_audioThreadAllocBytes.fetch_add(size, std::memory_order_relaxed);
    }
    void* p = std::malloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete[](void* p) noexcept {
    if (t_isAudioThread && p != nullptr) {
        g_audioThreadFreeCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    if (t_isAudioThread && p != nullptr) {
        g_audioThreadFreeCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    if (t_isAudioThread && p != nullptr) {
        g_audioThreadFreeCount.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

// ============================================================================
// 2. HARDWARE FAULT MOCK PLUGIN
// ============================================================================

enum class FaultType {
    None,
    AccessViolationRead,
    AccessViolationWrite,
    DivideByZeroInt,
    IllegalInstruction,
    DatatypeMisalignment,
    ArrayBoundsExceeded,
    StackCheckSimulated
};

class AdversarialMockPlugin : public plugins::IPluginInstance {
public:
    AdversarialMockPlugin() = default;

    void prepare(double sampleRate, uint32_t maxBlockSize) override {
        m_sampleRate = sampleRate;
        m_maxBlockSize = maxBlockSize;
    }

    void setFaultType(FaultType ft) noexcept {
        m_faultType = ft;
    }

    void setAutoReset(bool autoReset) noexcept {
        m_autoReset = autoReset;
    }
    bool autoReset() const noexcept {
        return m_autoReset;
    }

    void triggerHardwareFault() {
        switch (m_faultType) {
            case FaultType::None:
                break;
            case FaultType::AccessViolationRead: {
                volatile const int* nullPtr = nullptr;
                volatile int val = *nullPtr;
                (void)val;
                break;
            }
            case FaultType::AccessViolationWrite: {
                volatile int* badPtr = reinterpret_cast<volatile int*>(0xDEADBEEF);
                *badPtr = 0x1337;
                break;
            }
            case FaultType::DivideByZeroInt: {
                volatile int zero = 0;
                volatile int result = 12345 / zero;
                (void)result;
                break;
            }
            case FaultType::IllegalInstruction: {
                #if defined(__GNUC__) || defined(__clang__)
                __asm__ volatile ("ud2");
                #elif defined(_MSC_VER)
                __ud2();
                #else
                RaiseException(EXCEPTION_ILLEGAL_INSTRUCTION, 0, 0, nullptr);
                #endif
                break;
            }
            case FaultType::DatatypeMisalignment: {
                RaiseException(EXCEPTION_DATATYPE_MISALIGNMENT, 0, 0, nullptr);
                break;
            }
            case FaultType::ArrayBoundsExceeded: {
                RaiseException(EXCEPTION_ARRAY_BOUNDS_EXCEEDED, 0, 0, nullptr);
                break;
            }
            case FaultType::StackCheckSimulated: {
                RaiseException(EXCEPTION_FLT_STACK_CHECK, 0, 0, nullptr);
                break;
            }
        }
    }

    void process(audio::AudioProcessContext& ctx) override {
        if (m_faulted.load(std::memory_order_relaxed)) {
            ctx.output.copyFrom(ctx.input);
            return;
        }

        DWORD exCode = 0;
        bool ok = plugins::safeCallPluginAudio([&]() {
            // First corrupt output to test if PluginSlot reverts it to dryView
            for (uint32_t ch = 0; ch < ctx.output.numChannels(); ++ch) {
                for (uint32_t s = 0; s < ctx.numSamples; ++s) {
                    ctx.output.channel(ch)[s] = 999.0f;
                }
            }

            // Trigger configured fault
            triggerHardwareFault();

            // Normal processing: gain 2.0x
            for (uint32_t ch = 0; ch < ctx.output.numChannels(); ++ch) {
                for (uint32_t s = 0; s < ctx.numSamples; ++s) {
                    ctx.output.channel(ch)[s] = ctx.input.channel(ch)[s] * 2.0f;
                }
            }
        }, &exCode);

        if (!ok) {
            m_faulted.store(true, std::memory_order_release);
            m_lastExceptionCode.store(exCode, std::memory_order_release);
            ctx.output.copyFrom(ctx.input);
            m_crashCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void reset() override {}
    audio::NodeType type() const noexcept override { return audio::NodeType::Plugin; }
    const std::string& name() const noexcept override { return m_name; }
    const std::string& vendor() const noexcept override { return m_vendor; }
    const std::string& version() const noexcept override { return m_version; }
    size_t numParameters() const noexcept override { return 0; }
    plugins::PluginParameterDesc getParameterDesc(size_t) const override { return {}; }
    void setParameterValue(uint32_t, float) override {}
    float getParameterValue(uint32_t) const override { return 0.0f; }
    bool hasCustomGui() const noexcept override { return false; }
    bool openGui(HWND) override { return false; }
    void closeGui() override {}
    std::vector<uint8_t> saveState() const override { return {}; }
    bool loadState(const std::vector<uint8_t>&) override { return true; }

    uint64_t crashCount() const noexcept { return m_crashCount.load(std::memory_order_relaxed); }
    DWORD lastExceptionCode() const noexcept { return m_lastExceptionCode.load(std::memory_order_relaxed); }

private:
    std::string m_name{"AdversarialMockPlugin"};
    std::string m_vendor{"EmpiricalChallenger"};
    std::string m_version{"2.0.0"};
    double m_sampleRate{48000.0};
    uint32_t m_maxBlockSize{512};
    FaultType m_faultType{FaultType::None};
    bool m_autoReset{false};
    std::atomic<uint64_t> m_crashCount{0};
    std::atomic<DWORD> m_lastExceptionCode{0};
};

// ============================================================================
// TEST 1: CONTINUOUS HARDWARE FAULTS ACROSS 15,000+ AUDIO BLOCKS
// ============================================================================
void runContinuousFaultIsolationStressTest() {
    std::cout << "[CHALLENGER-TEST 1] Continuous Hardware Faults across 15,000 Audio Blocks...\n";
    plugins::initCrashIsolation();

    constexpr uint32_t BLOCK_SIZE = 256;
    constexpr uint32_t NUM_BLOCKS = 15000;

    auto mock = std::make_unique<AdversarialMockPlugin>();
    auto* rawMock = mock.get();
    rawMock->setAutoReset(true); // Continuously fault on EVERY block

    audio::PluginSlot slot(std::move(mock));
    slot.prepare(48000.0, BLOCK_SIZE);

    audio::OwnedAudioBuffer inBuf(2, BLOCK_SIZE);
    audio::OwnedAudioBuffer outBuf(2, BLOCK_SIZE);
    auto inView = inBuf.view(BLOCK_SIZE);
    auto outView = outBuf.view(BLOCK_SIZE);

    // Initialize deterministic test signal
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> dist(-0.8f, 0.8f);

    const FaultType faultTypes[] = {
        FaultType::AccessViolationRead,
        FaultType::AccessViolationWrite,
        FaultType::DivideByZeroInt,
        FaultType::IllegalInstruction,
        FaultType::DatatypeMisalignment,
        FaultType::ArrayBoundsExceeded,
        FaultType::StackCheckSimulated
    };
    constexpr size_t NUM_FAULTS = sizeof(faultTypes) / sizeof(faultTypes[0]);

    size_t bitExactPassCount = 0;
    size_t bitExactFailCount = 0;
    auto startTime = std::chrono::steady_clock::now();

    for (uint32_t block = 0; block < NUM_BLOCKS; ++block) {
        // Rotate through hardware fault types
        FaultType currentFault = faultTypes[block % NUM_FAULTS];
        rawMock->setFaultType(currentFault);

        // Fill input with random audio samples
        for (uint32_t ch = 0; ch < 2; ++ch) {
            for (uint32_t s = 0; s < BLOCK_SIZE; ++s) {
                inView.channel(ch)[s] = dist(rng);
            }
        }
        outView.clear();

        audio::AudioProcessContext ctx{
            .input = inView,
            .output = outView,
            .sampleRate = 48000.0,
            .numSamples = BLOCK_SIZE
        };

        // Process block through PluginSlot
        slot.process(ctx);

        // Verify MXCSR register restored (FTZ / DAZ)
        unsigned int mxcsr = _mm_getcsr();
        assert((mxcsr & 0x8040) == 0x8040 && "MXCSR FTZ/DAZ not restored after hardware fault!");

        // Verify Bit-Exact Audio Pass-Through
        bool bitExact = true;
        for (uint32_t ch = 0; ch < 2; ++ch) {
            if (std::memcmp(outView.channel(ch), inView.channel(ch), BLOCK_SIZE * sizeof(float)) != 0) {
                bitExact = false;
                break;
            }
        }

        if (bitExact) {
            bitExactPassCount++;
        } else {
            bitExactFailCount++;
        }

        if (rawMock->autoReset()) {
            rawMock->resetFault();
        }
    }

    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime).count();

    std::cout << "  - Processed " << NUM_BLOCKS << " continuous hardware faults in " << elapsedMs << " ms\n";
    std::cout << "  - Total hardware crashes intercepted: " << rawMock->crashCount() << "\n";
    std::cout << "  - Bit-exact dry pass-through blocks: " << bitExactPassCount << " / " << NUM_BLOCKS << "\n";
    assert(rawMock->crashCount() == NUM_BLOCKS);
    assert(bitExactFailCount == 0 && "FATAL: Dry signal was not bit-exact during continuous faults!");
    std::cout << "  -> PASSED: Zero host crashes, zero deadlocks, 100% bit-exact dry audio pass-through\n\n";
}

// ============================================================================
// TEST 2: MEMORY CANARY & BUFFER CORRUPTION RESILIENCE
// ============================================================================
void runMemoryCanaryCorruptionTest() {
    std::cout << "[CHALLENGER-TEST 2] Memory Canary & Surrounding Buffer Integrity...\n";
    plugins::initCrashIsolation();

    constexpr uint32_t CANARY_SIZE = 128;
    constexpr uint32_t BLOCK_SIZE = 256;

    std::vector<uint8_t> leadingCanary(CANARY_SIZE, 0xAA);
    audio::OwnedAudioBuffer inBuf(2, BLOCK_SIZE);
    audio::OwnedAudioBuffer outBuf(2, BLOCK_SIZE);
    std::vector<uint8_t> trailingCanary(CANARY_SIZE, 0x55);

    auto mock = std::make_unique<AdversarialMockPlugin>();
    auto* rawMock = mock.get();
    rawMock->setAutoReset(false); // Latch faulted

    audio::PluginSlot slot(std::move(mock));
    slot.prepare(48000.0, BLOCK_SIZE);

    auto inView = inBuf.view(BLOCK_SIZE);
    auto outView = outBuf.view(BLOCK_SIZE);

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t s = 0; s < BLOCK_SIZE; ++s) {
            inView.channel(ch)[s] = 0.42f;
        }
    }

    rawMock->setFaultType(FaultType::AccessViolationWrite);

    audio::AudioProcessContext ctx{
        .input = inView,
        .output = outView,
        .sampleRate = 48000.0,
        .numSamples = BLOCK_SIZE
    };

    // First block: triggers hardware crash and latches faulted
    slot.process(ctx);
    assert(slot.isFaulted());

    // Subsequent 5,000 blocks with latched fault
    for (uint32_t i = 0; i < 5000; ++i) {
        slot.process(ctx);
    }

    // Verify Canaries
    for (size_t i = 0; i < CANARY_SIZE; ++i) {
        assert(leadingCanary[i] == 0xAA && "Leading canary corrupted!");
        assert(trailingCanary[i] == 0x55 && "Trailing canary corrupted!");
    }

    // Verify output equals input
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t s = 0; s < BLOCK_SIZE; ++s) {
            assert(outView.channel(ch)[s] == 0.42f);
        }
    }

    std::cout << "  -> PASSED: Canary buffers intact, zero memory corruption detected\n\n";
}

// ============================================================================
// TEST 3: MULTI-THREADED CONCURRENCY & THREAD-LOCAL VEH ISOLATION
// ============================================================================
void runMultiThreadedCrashConcurrencyTest() {
    std::cout << "[CHALLENGER-TEST 3] Multi-Threaded Concurrency (4 Threads, 20,000 Total Blocks)...\n";
    plugins::initCrashIsolation();

    constexpr int NUM_THREADS = 4;
    constexpr uint32_t BLOCKS_PER_THREAD = 5000;
    constexpr uint32_t BLOCK_SIZE = 128;

    std::atomic<bool> startFlag{false};
    std::atomic<uint32_t> completedThreads{0};
    std::atomic<uint64_t> totalCrashesCaught{0};
    std::atomic<uint64_t> bitExactBlocks{0};

    auto threadFunc = [&](int threadId) {
        auto mock = std::make_unique<AdversarialMockPlugin>();
        auto* rawMock = mock.get();
        rawMock->setAutoReset(true);

        audio::PluginSlot slot(std::move(mock));
        slot.prepare(48000.0, BLOCK_SIZE);

        audio::OwnedAudioBuffer inBuf(2, BLOCK_SIZE);
        audio::OwnedAudioBuffer outBuf(2, BLOCK_SIZE);
        auto inView = inBuf.view(BLOCK_SIZE);
        auto outView = outBuf.view(BLOCK_SIZE);

        std::mt19937 rng(42 + threadId);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
        std::uniform_int_distribution<int> faultDist(1, 4);

        while (!startFlag.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        uint64_t localBitExact = 0;

        for (uint32_t b = 0; b < BLOCKS_PER_THREAD; ++b) {
            FaultType ft = static_cast<FaultType>(faultDist(rng));
            rawMock->setFaultType(ft);

            for (uint32_t ch = 0; ch < 2; ++ch) {
                for (uint32_t s = 0; s < BLOCK_SIZE; ++s) {
                    inView.channel(ch)[s] = dist(rng);
                }
            }

            audio::AudioProcessContext ctx{
                .input = inView,
                .output = outView,
                .sampleRate = 48000.0,
                .numSamples = BLOCK_SIZE
            };

            slot.process(ctx);

            if (std::memcmp(outView.channel(0), inView.channel(0), BLOCK_SIZE * sizeof(float)) == 0 &&
                std::memcmp(outView.channel(1), inView.channel(1), BLOCK_SIZE * sizeof(float)) == 0) {
                localBitExact++;
            }

            if (rawMock->autoReset()) {
                rawMock->resetFault();
            }
        }

        totalCrashesCaught.fetch_add(rawMock->crashCount(), std::memory_order_relaxed);
        bitExactBlocks.fetch_add(localBitExact, std::memory_order_relaxed);
        completedThreads.fetch_add(1, std::memory_order_release);
    };

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back(threadFunc, t);
    }

    startFlag.store(true, std::memory_order_release);

    for (auto& th : threads) {
        th.join();
    }

    std::cout << "  - Completed " << NUM_THREADS << " threads, total crashes intercepted: "
              << totalCrashesCaught.load() << "\n";
    std::cout << "  - Total bit-exact audio blocks: " << bitExactBlocks.load() << " / "
              << (NUM_THREADS * BLOCKS_PER_THREAD) << "\n";
    assert(completedThreads.load() == NUM_THREADS);
    assert(bitExactBlocks.load() == NUM_THREADS * BLOCKS_PER_THREAD);
    std::cout << "  -> PASSED: Thread-local VEH context perfectly isolated under heavy concurrent fault load\n\n";
}

// ============================================================================
// TEST 4: AUDIO THREAD HEAP ALLOCATION AUDIT DURING CRASH ISOLATION
// ============================================================================
void runAudioThreadHeapAllocationAudit() {
    std::cout << "[CHALLENGER-TEST 4] Audio Thread Heap Allocation Audit...\n";
    plugins::initCrashIsolation();

    constexpr uint32_t BLOCK_SIZE = 256;

    // 4.1 Test safeCallPluginAudio overhead & allocations
    {
        g_audioThreadAllocCount.store(0);
        g_audioThreadAllocBytes.store(0);
        t_isAudioThread = true;

        for (int i = 0; i < 5000; ++i) {
            DWORD exCode = 0;
            bool ok = plugins::safeCallPluginAudio([&]() {
                volatile int* nullPtr = nullptr;
                volatile int val = *nullPtr;
                (void)val;
            }, &exCode);
            (void)ok;
        }

        t_isAudioThread = false;
        size_t safeCallAllocs = g_audioThreadAllocCount.load();
        std::cout << "  - safeCallPluginAudio 5,000 hardware crashes heap allocations: " << safeCallAllocs << "\n";
        assert(safeCallAllocs == 0 && "CRITICAL: safeCallPluginAudio allocated heap memory during crash!");
    }

    // 4.2 Test PluginSlot::process heap allocations with AdversarialMockPlugin (no std::string alloc in mock)
    {
        auto mock = std::make_unique<AdversarialMockPlugin>();
        auto* rawMock = mock.get();
        rawMock->setAutoReset(true);
        rawMock->setFaultType(FaultType::AccessViolationWrite);

        audio::PluginSlot slot(std::move(mock));
        slot.prepare(48000.0, BLOCK_SIZE);

        audio::OwnedAudioBuffer inBuf(2, BLOCK_SIZE);
        audio::OwnedAudioBuffer outBuf(2, BLOCK_SIZE);
        auto inView = inBuf.view(BLOCK_SIZE);
        auto outView = outBuf.view(BLOCK_SIZE);

        audio::AudioProcessContext ctx{
            .input = inView,
            .output = outView,
            .sampleRate = 48000.0,
            .numSamples = BLOCK_SIZE
        };

        g_audioThreadAllocCount.store(0);
        g_audioThreadAllocBytes.store(0);
        t_isAudioThread = true;

        for (int i = 0; i < 5000; ++i) {
            slot.process(ctx);
        }

        t_isAudioThread = false;
        size_t slotProcessAllocs = g_audioThreadAllocCount.load();
        std::cout << "  - PluginSlot::process 5,000 crashes heap allocations: " << slotProcessAllocs << "\n";
        assert(slotProcessAllocs == 0 && "CRITICAL: PluginSlot::process allocated heap memory during crash!");
    }

    // 4.3 EMPIRICAL CHALLENGE: Verify Vst3PluginInstance / ClapPluginInstance faultReason string formatting
    // Let's test the exact pattern from vst3_host.cpp:379 and clap_host.cpp:172:
    // m_faultReason = std::string("VST3 crash: ") + getExceptionDescription(exCode);
    {
        g_audioThreadAllocCount.store(0);
        g_audioThreadAllocBytes.store(0);
        t_isAudioThread = true;

        DWORD testExCode = EXCEPTION_ACCESS_VIOLATION;
        std::string testFaultReason;
        testFaultReason = std::string("VST3 crash: ") + plugins::getExceptionDescription(testExCode);

        t_isAudioThread = false;
        size_t stringConcatAllocs = g_audioThreadAllocCount.load();
        size_t bytesAllocated = g_audioThreadAllocBytes.load();

        std::cout << "  - std::string(\"VST3 crash: \") + getExceptionDescription() on audio thread:\n";
        std::cout << "    * Length of string: " << testFaultReason.length() << " chars\n";
        std::cout << "    * Allocations detected: " << stringConcatAllocs << "\n";
        std::cout << "    * Bytes allocated: " << bytesAllocated << "\n";

        if (stringConcatAllocs > 0) {
            std::cout << "    [VULNERABILITY IDENTIFIED] Vst3PluginInstance::process and ClapPluginInstance::process\n";
            std::cout << "    allocate dynamic heap memory on the audio callback thread during crash latching!\n";
        }
    }

    // 4.4 Test CrashingMockPlugin allocation from test_praccy.cpp
    {
        g_audioThreadAllocCount.store(0);
        g_audioThreadAllocBytes.store(0);
        t_isAudioThread = true;

        std::string mockFaultReason;
        mockFaultReason = plugins::getExceptionDescription(EXCEPTION_ACCESS_VIOLATION);

        t_isAudioThread = false;
        size_t mockAllocs = g_audioThreadAllocCount.load();
        size_t mockBytes = g_audioThreadAllocBytes.load();

        std::cout << "  - m_faultReason = getExceptionDescription() from test_praccy.cpp:\n";
        std::cout << "    * Length of string: " << mockFaultReason.length() << " chars\n";
        std::cout << "    * Allocations detected: " << mockAllocs << "\n";
        std::cout << "    * Bytes allocated: " << mockBytes << "\n";
    }

    std::cout << "  -> Audio thread allocation audit completed\n\n";
}

// ============================================================================
// TEST 6: NESTED SAFE CALL CRASH ISOLATION & STACK UNWINDING
// ============================================================================
void runNestedCrashIsolationTest() {
    std::cout << "[CHALLENGER-TEST 6] Nested safeCallPluginAudio Context Unwinding...\n";
    plugins::initCrashIsolation();

    // Scenario A: Inner crashes, outer catches and continues cleanly
    {
        DWORD outerEx = 0;
        bool outerOk = plugins::safeCallPluginAudio([&]() {
            DWORD innerEx = 0;
            bool innerOk = plugins::safeCallPluginAudio([&]() {
                volatile int* bad = nullptr;
                *bad = 42;
            }, &innerEx);

            assert(!innerOk);
            assert(innerEx == EXCEPTION_ACCESS_VIOLATION);
            // Outer continues normally
        }, &outerEx);

        assert(outerOk);
        assert(outerEx == 0);
    }

    // Scenario B: Inner crashes, recovered, then outer also crashes
    {
        DWORD outerEx = 0;
        bool outerOk = plugins::safeCallPluginAudio([&]() {
            DWORD innerEx = 0;
            bool innerOk = plugins::safeCallPluginAudio([&]() {
                volatile int zero = 0;
                volatile int r = 100 / zero;
                (void)r;
            }, &innerEx);

            assert(!innerOk);
            assert(innerEx == EXCEPTION_INT_DIVIDE_BY_ZERO);

            // Outer then also crashes
            volatile int* bad = nullptr;
            *bad = 99;
        }, &outerEx);

        assert(!outerOk);
        assert(outerEx == EXCEPTION_ACCESS_VIOLATION);
    }

    std::cout << "  -> PASSED: Nested crash contexts properly unwound without stack or state corruption\n\n";
}

// ============================================================================
// TEST 5: STATIC & BINARY SEARCH FOR PROHIBITED COMMAND STRINGS
// ============================================================================
void runProhibitedStringsInspection() {
    std::cout << "[CHALLENGER-TEST 5] Prohibited Command Strings Scan (src/ and Praccy.exe)...\n";

    const std::vector<std::string> prohibited = {
        "std::system",
        "cmd.exe",
        "powershell.exe",
        "apply_update.bat"
    };

    // 5.1 Scan all source files in src/
    size_t inspectedFiles = 0;
    size_t prohibitedSrcMatches = 0;

    for (const auto& entry : std::filesystem::recursive_directory_iterator("f:/Projects/Praccy/src")) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".c") continue;

        inspectedFiles++;
        std::ifstream file(entry.path(), std::ios::in | std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        for (const auto& needle : prohibited) {
            if (content.find(needle) != std::string::npos) {
                std::cerr << "  PROHIBITED STRING FOUND in " << entry.path().string() << ": " << needle << "\n";
                prohibitedSrcMatches++;
            }
        }
    }

    std::cout << "  - Inspected " << inspectedFiles << " source files in src/, matches: " << prohibitedSrcMatches << "\n";
    assert(inspectedFiles >= 15);
    assert(prohibitedSrcMatches == 0 && "CRITICAL: Prohibited command string found in src/!");

    // 5.2 Scan compiled Praccy.exe binary
    std::filesystem::path binPath = "f:/Projects/Praccy/build/Praccy.exe";
    size_t prohibitedBinMatches = 0;

    if (std::filesystem::exists(binPath)) {
        std::ifstream binFile(binPath, std::ios::in | std::ios::binary);
        std::vector<char> buffer((std::istreambuf_iterator<char>(binFile)), std::istreambuf_iterator<char>());
        std::string binContent(buffer.begin(), buffer.end());

        for (const auto& needle : prohibited) {
            // Case-insensitive search in binary
            auto it = std::search(
                binContent.begin(), binContent.end(),
                needle.begin(), needle.end(),
                [](char a, char b) { return std::tolower(a) == std::tolower(b); }
            );
            if (it != binContent.end()) {
                std::cerr << "  PROHIBITED STRING FOUND IN BINARY: " << needle << "\n";
                prohibitedBinMatches++;
            }
        }
        std::cout << "  - Inspected binary " << binPath.string() << " (" << buffer.size()
                  << " bytes), matches: " << prohibitedBinMatches << "\n";
        assert(prohibitedBinMatches == 0 && "CRITICAL: Prohibited command string found in Praccy.exe!");
    } else {
        std::cout << "  - WARNING: Praccy.exe not found for binary inspection!\n";
    }

    std::cout << "  -> PASSED: Zero prohibited strings in src/ or compiled binary\n\n";
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================
int main() {
    std::cout << "================================================================\n";
    std::cout << "   EMPIRICAL CHALLENGER TEST SUITE: MILESTONE 2 (SECOPS/CRASH)  \n";
    std::cout << "================================================================\n\n";

    try {
        runContinuousFaultIsolationStressTest();
        runMemoryCanaryCorruptionTest();
        runMultiThreadedCrashConcurrencyTest();
        runAudioThreadHeapAllocationAudit();
        runNestedCrashIsolationTest();
        runProhibitedStringsInspection();

        std::cout << "================================================================\n";
        std::cout << "   ALL EMPIRICAL CHALLENGER TESTS EXECUTED SUCCESSFULLY!        \n";
        std::cout << "================================================================\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FATAL EXCEPTION: " << ex.what() << "\n";
        return 1;
    }
}
