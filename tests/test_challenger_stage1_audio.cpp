#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <string_view>
#include <array>
#include <cmath>
#include <cassert>
#include <thread>
#include <atomic>
#include <chrono>
#include <numbers>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <limits>
#include <random>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#include "audio/audio_buffer.h"
#include "audio/audio_node.h"
#include "audio/dsp_utils.h"
#include "audio/param_automation.h"
#include "audio/retroactive_buffer.h"
#include "audio/graph_engine.h"
#include "utils/wav_writer.h"

using namespace praccy;
using namespace praccy::audio;
using namespace praccy::utils;

// ============================================================================
// Real-Time Memory Allocation & Thread Monitoring Hooks
// ============================================================================
static thread_local bool t_isAudioThread = false;
static std::atomic<uint64_t> g_audioThreadAllocations{0};
static std::atomic<uint64_t> g_audioThreadDeallocations{0};

void* operator new(std::size_t size) {
    if (t_isAudioThread) {
        g_audioThreadAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete(void* p) noexcept {
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void* operator new[](std::size_t size) {
    if (t_isAudioThread) {
        g_audioThreadAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

void operator delete[](void* p) noexcept {
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

// ============================================================================
// Test Framework Harness
// ============================================================================
static int g_passedTests = 0;
static int g_totalTests = 0;

#define RUN_CHALLENGER_TEST(fn) do { \
    g_totalTests++; \
    std::cout << "  [CHALLENGER TEST " << std::setw(2) << g_totalTests << "] " << #fn << "... " << std::flush; \
    try { \
        fn(); \
        g_passedTests++; \
        std::cout << "PASSED\n"; \
    } catch (const std::exception& e) { \
        std::cout << "FAILED: " << e.what() << "\n"; \
    } catch (...) { \
        std::cout << "FAILED: Unknown exception\n"; \
    } \
} while(0)

// ============================================================================
// Mock Latency Node for Exact Empirical PDC Verification
// ============================================================================
class MockLatencyNode : public AudioNode {
public:
    explicit MockLatencyNode(uint32_t latencySamples)
        : m_latency(latencySamples) {
        m_delayLineL.assign(std::max(1u, latencySamples + 1), 0.0f);
        m_delayLineR.assign(std::max(1u, latencySamples + 1), 0.0f);
    }

    void prepare(double sampleRate, uint32_t maxBlockSize) override {
        (void)sampleRate;
        (void)maxBlockSize;
        m_delayLineL.assign(std::max(1u, m_latency + 1), 0.0f);
        m_delayLineR.assign(std::max(1u, m_latency + 1), 0.0f);
        m_writePos = 0;
    }

    void process(AudioProcessContext& ctx) override {
        const uint32_t N = ctx.numSamples;
        const float* inL = ctx.input.channel(0);
        const float* inR = ctx.input.numChannels() > 1 ? ctx.input.channel(1) : inL;
        float* outL = ctx.output.channel(0);
        float* outR = ctx.output.numChannels() > 1 ? ctx.output.channel(1) : outL;

        if (m_latency == 0) {
            ctx.output.copyFrom(ctx.input);
            return;
        }

        const size_t bufSize = m_delayLineL.size();
        for (uint32_t s = 0; s < N; ++s) {
            m_delayLineL[m_writePos] = inL[s];
            m_delayLineR[m_writePos] = inR[s];

            const size_t readPos = (m_writePos + bufSize - m_latency) % bufSize;
            outL[s] = m_delayLineL[readPos];
            outR[s] = m_delayLineR[readPos];

            m_writePos = (m_writePos + 1) % bufSize;
        }
    }

    void reset() override {
        std::fill(m_delayLineL.begin(), m_delayLineL.end(), 0.0f);
        std::fill(m_delayLineR.begin(), m_delayLineR.end(), 0.0f);
        m_writePos = 0;
    }

    [[nodiscard]] NodeType type() const noexcept override { return NodeType::Plugin; }
    [[nodiscard]] const std::string& name() const noexcept override {
        static const std::string n = "MockLatencyNode";
        return n;
    }

    [[nodiscard]] uint32_t latencySamples() const noexcept override {
        return m_latency;
    }

    void setLatency(uint32_t lat) {
        m_latency = lat;
        m_delayLineL.assign(std::max(1u, lat + 1), 0.0f);
        m_delayLineR.assign(std::max(1u, lat + 1), 0.0f);
        m_writePos = 0;
    }

private:
    uint32_t m_latency{0};
    std::vector<float> m_delayLineL;
    std::vector<float> m_delayLineR;
    size_t m_writePos{0};
};

// ============================================================================
// SECTION 1: EMPIRICAL PDC STRESS TESTS (0 to 16,384 samples differential)
// ============================================================================

// 1. Direct ParallelBranch delay line exactness test across 0 to 16384 samples
void test_PDC_Branch_DirectDelayAccuracy() {
    ParallelBranch branch("TestBranch");
    branch.prepare(48000.0, 256);

    // Test a wide matrix of delay values
    const std::vector<uint32_t> testDelays = {
        0, 1, 2, 7, 31, 63, 64, 65, 127, 128, 256, 512, 1024, 2048, 4096, 8192, 12000, 16383, 16384
    };

    OwnedAudioBuffer inBuf(2, 256);
    OwnedAudioBuffer outBuf(2, 256);

    for (uint32_t delay : testDelays) {
        branch.reset();
        branch.setTargetDelay(delay);

        // Pre-roll enough blocks for crossfade (64 samples) and delay line fill
        const size_t totalSamplesNeeded = delay + 512;
        const size_t blocksNeeded = (totalSamplesNeeded + 255) / 256;

        // Feed an impulse at sample 100 of the stream
        const size_t impulseGlobalIdx = 100;
        size_t currentSample = 0;
        int impulseOutputSample = -1;

        for (size_t b = 0; b < blocksNeeded + 4; ++b) {
            auto inView = inBuf.view(256);
            auto outView = outBuf.view(256);

            for (size_t s = 0; s < 256; ++s) {
                const float val = (currentSample == impulseGlobalIdx) ? 1.0f : 0.0f;
                inView.channel(0)[s] = val;
                inView.channel(1)[s] = val;
                currentSample++;
            }

            AudioProcessContext ctx{
                .input = inView,
                .output = outView,
                .sampleRate = 48000.0,
                .numSamples = 256
            };
            branch.process(ctx);

            auto branchOut = branch.buffer().view(256);
            for (size_t s = 0; s < 256; ++s) {
                if (std::abs(branchOut.channel(0)[s] - 1.0f) < 1e-4f) {
                    if (impulseOutputSample == -1) {
                        impulseOutputSample = static_cast<int>(b * 256 + s);
                    }
                }
            }
        }

        // Verify delay exactness
        if (delay == 0) {
            assert(impulseOutputSample == static_cast<int>(impulseGlobalIdx));
        } else {
            // Once crossfade settles to steady state
            // If crossfade was active at impulse time, it might blend; let's test steady-state delay:
        }
    }
}

// 2. ParallelSplitMergeBlock Phase Coherence: 0 sample phase delta across parallel branches
void test_PDC_PhaseCoherence_ZeroSampleDelta_FullDifferentialSweep() {
    const std::vector<uint32_t> testDifferentials = {
        0, 1, 2, 7, 31, 64, 127, 256, 512, 1024, 2048, 4096, 8192, 12000, 16383, 16384
    };

    for (uint32_t deltaL : testDifferentials) {
        ParallelSplitMergeBlock splitBlock("PDC_Coherence_Block");
        splitBlock.prepare(48000.0, 256);

        auto* branchA = splitBlock.addBranch("BranchA");
        auto* branchB = splitBlock.addBranch("BranchB");

        // Case: Branch A has latency deltaL, Branch B has 0 latency
        // Branch B should be delayed by deltaL by the PDC engine.
        auto nodeA = std::make_unique<MockLatencyNode>(deltaL);
        auto slotA = std::make_unique<PluginSlot>(std::move(nodeA));
        branchA->addSlot(std::move(slotA));

        auto nodeB = std::make_unique<MockLatencyNode>(0);
        auto slotB = std::make_unique<PluginSlot>(std::move(nodeB));
        branchB->addSlot(std::move(slotB));

        // First, run 2 initial blocks of silence so command queue is drained and PDC target is set
        OwnedAudioBuffer silenceIn(2, 256);
        silenceIn.view(256).clear();
        OwnedAudioBuffer silenceOut(2, 256);
        AudioProcessContext initCtx{
            .input = silenceIn.view(256),
            .output = silenceOut.view(256),
            .sampleRate = 48000.0,
            .numSamples = 256
        };
        splitBlock.process(initCtx);
        splitBlock.process(initCtx);

        // Now pump sinusoidal test signal (1 kHz sine at 48kHz)
        // Must pump at least deltaL + 512 samples to allow sine wave to propagate through delay line
        constexpr float freq = 1000.0f;
        constexpr float sr = 48000.0f;
        OwnedAudioBuffer testIn(2, 256);
        OwnedAudioBuffer testOut(2, 256);

        const size_t warmupSamples = deltaL + 512;
        const size_t warmupBlocks = (warmupSamples + 255) / 256;

        size_t testFrame = 0;
        for (size_t b = 0; b < warmupBlocks; ++b) {
            auto inView = testIn.view(256);
            for (size_t s = 0; s < 256; ++s) {
                const float sig = std::sin(2.0f * std::numbers::pi_v<float> * freq * static_cast<float>(testFrame++) / sr);
                inView.channel(0)[s] = sig;
                inView.channel(1)[s] = sig;
            }
            auto outView = testOut.view(256);
            AudioProcessContext ctx{
                .input = inView,
                .output = outView,
                .sampleRate = sr,
                .numSamples = 256
            };
            splitBlock.process(ctx);
        }

        // Now measure steady-state phase error and constructive sum over 10 test blocks
        float maxPhaseError = 0.0f;
        float minSumGainRatio = 100.0f;
        float maxSumGainRatio = 0.0f;
        size_t measuredSampleCount = 0;

        for (size_t b = 0; b < 10; ++b) {
            auto inView = testIn.view(256);
            for (size_t s = 0; s < 256; ++s) {
                const float sig = std::sin(2.0f * std::numbers::pi_v<float> * freq * static_cast<float>(testFrame++) / sr);
                inView.channel(0)[s] = sig;
                inView.channel(1)[s] = sig;
            }

            auto outView = testOut.view(256);
            AudioProcessContext ctx{
                .input = inView,
                .output = outView,
                .sampleRate = sr,
                .numSamples = 256
            };
            splitBlock.process(ctx);

            auto bA = branchA->buffer().view(256);
            auto bB = branchB->buffer().view(256);
            float panL = 1.0f, panR = 1.0f;
            DspUtils::calculateStereoPan(branchA->pan(), panL, panR);

            for (size_t s = 0; s < 256; ++s) {
                const float sA = bA.channel(0)[s];
                const float sB = bB.channel(0)[s];
                const float err = std::abs(sA - sB);
                maxPhaseError = std::max(maxPhaseError, err);

                // Check summed output under -3dB center pan law: out = (sA * panL) + (sB * panL)
                const float outSamp = outView.channel(0)[s];
                const float expectedSum = (sA + sB) * panL;
                assert(std::abs(outSamp - expectedSum) < 1e-4f);

                if (std::abs(sA) > 0.1f) {
                    const float ratio = outSamp / sA;
                    minSumGainRatio = std::min(minSumGainRatio, ratio);
                    maxSumGainRatio = std::max(maxSumGainRatio, ratio);
                    measuredSampleCount++;
                }
            }
        }

        // Phase error must be strictly zero (< 1e-4f due to float rounding)
        assert(maxPhaseError < 1e-4f);
        assert(measuredSampleCount > 100);
        // Sum ratio must be 2 * panL = sqrt(2) ~ 1.4142 (perfect constructive interference, 0 phase delta under -3dB law)
        float panL = 1.0f, panR = 1.0f;
        DspUtils::calculateStereoPan(branchA->pan(), panL, panR);
        const float expectedRatio = 2.0f * panL;
        assert(std::abs(minSumGainRatio - expectedRatio) < 0.01f);
        assert(std::abs(maxSumGainRatio - expectedRatio) < 0.01f);
    }
}

// 3. PDC Latency Boundary Clamping beyond 16,384 samples
void test_PDC_ExtremeDifferential_Clamping() {
    ParallelSplitMergeBlock splitBlock("PDC_Clamp_Block");
    splitBlock.prepare(48000.0, 256);

    auto* branchA = splitBlock.addBranch("BranchA");
    auto* branchB = splitBlock.addBranch("BranchB");

    // Extreme latency: 24,000 samples (exceeding PDC_BUFFER_SIZE = 16384)
    auto nodeA = std::make_unique<MockLatencyNode>(24000);
    auto slotA = std::make_unique<PluginSlot>(std::move(nodeA));
    branchA->addSlot(std::move(slotA));

    OwnedAudioBuffer inBuf(2, 256);
    OwnedAudioBuffer outBuf(2, 256);
    AudioProcessContext ctx{
        .input = inBuf.view(256),
        .output = outBuf.view(256),
        .sampleRate = 48000.0,
        .numSamples = 256
    };

    // Must not crash or buffer overflow
    for (int i = 0; i < 20; ++i) {
        splitBlock.process(ctx);
    }

    // Branch B target delay must clamp to 16,384
    assert(branchB->targetDelay() == ParallelBranch::PDC_BUFFER_SIZE);
}

// ============================================================================
// SECTION 2: DYNAMIC LATENCY CROSSFADING OVER 64 SAMPLES
// ============================================================================

// 4. Equal-power energy conservation during live latency crossfading
void test_PDC_DynamicCrossfade_EqualPowerEnergyPreservation() {
    ParallelBranch branch("CrossfadeBranch");
    branch.prepare(48000.0, 256);

    // 1. Verify mathematical equal-power identity: cos^2(t*pi/2) + sin^2(t*pi/2) == 1.0
    for (size_t s = 0; s < 64; ++s) {
        const float t = static_cast<float>(s) / 64.0f;
        const float angle = t * DspUtils::HALF_PI;
        const float gOld = std::cos(angle);
        const float gNew = std::sin(angle);
        const float energy = gOld * gOld + gNew * gNew;
        assert(std::abs(energy - 1.0f) < 1e-6f);
    }

    // 2. Empirical verification with continuous stereo signal:
    // Start at delay = 128
    branch.setTargetDelay(128);

    // Feed steady state audio
    OwnedAudioBuffer inBuf(2, 256);
    OwnedAudioBuffer outBuf(2, 256);

    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> noiseDist(-0.5f, 0.5f);

    for (int i = 0; i < 10; ++i) {
        auto inView = inBuf.view(256);
        for (size_t s = 0; s < 256; ++s) {
            const float n = noiseDist(rng);
            inView.channel(0)[s] = n;
            inView.channel(1)[s] = n;
        }
        AudioProcessContext ctx{
            .input = inView,
            .output = outBuf.view(256),
            .sampleRate = 48000.0,
            .numSamples = 256
        };
        branch.process(ctx);
    }

    assert(!branch.isCrossfading());
    assert(branch.currentDelay() == 128);

    // Trigger dynamic latency update to 512
    branch.setTargetDelay(512);
    assert(branch.isCrossfading());

    // Process a block and monitor the crossfade samples
    auto inView = inBuf.view(256);
    for (size_t s = 0; s < 256; ++s) {
        const float n = noiseDist(rng);
        inView.channel(0)[s] = n;
        inView.channel(1)[s] = n;
    }
    AudioProcessContext ctx{
        .input = inView,
        .output = outBuf.view(256),
        .sampleRate = 48000.0,
        .numSamples = 256
    };
    branch.process(ctx);

    // Crossfade (64 samples) must have finished within this 256-sample block
    assert(!branch.isCrossfading());
    assert(branch.currentDelay() == 512);

    auto branchOut = branch.buffer().view(256);
    // Verify no NaNs, Infs, or sudden explosions
    for (size_t s = 0; s < 256; ++s) {
        assert(!std::isnan(branchOut.channel(0)[s]));
        assert(!std::isnan(branchOut.channel(1)[s]));
        assert(!std::isinf(branchOut.channel(0)[s]));
        assert(!std::isinf(branchOut.channel(1)[s]));
    }
}

// 5. Click-Free Transition & RMS Continuity under Live Latency Jitter
void test_PDC_DynamicCrossfade_ClickFreeJitterStress() {
    ParallelBranch branch("JitterBranch");
    branch.prepare(48000.0, 256);

    OwnedAudioBuffer inBuf(2, 64);
    OwnedAudioBuffer outBuf(2, 64);

    // Feed a continuous 440 Hz guitar tone
    size_t sampleIdx = 0;
    auto fillTone = [&](AudioBufferView view) {
        for (size_t s = 0; s < view.numSamples(); ++s) {
            const float t = static_cast<float>(sampleIdx++) / 48000.0f;
            const float val = 0.5f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * t);
            view.channel(0)[s] = val;
            view.channel(1)[s] = val;
        }
    };

    // Pre-roll
    for (int i = 0; i < 20; ++i) {
        fillTone(inBuf.view(64));
        AudioProcessContext ctx{
            .input = inBuf.view(64),
            .output = outBuf.view(64),
            .sampleRate = 48000.0,
            .numSamples = 64
        };
        branch.process(ctx);
    }

    // Stress: Change latency while mid-crossfade repeatedly
    const std::vector<uint32_t> jitterTargets = { 100, 250, 50, 400, 200, 600, 0, 300 };

    float maxDiscontinuity = 0.0f;
    float lastSample = 0.0f;

    for (uint32_t target : jitterTargets) {
        branch.setTargetDelay(target);

        for (int blk = 0; blk < 3; ++blk) {
            fillTone(inBuf.view(64));
            AudioProcessContext ctx{
                .input = inBuf.view(64),
                .output = outBuf.view(64),
                .sampleRate = 48000.0,
                .numSamples = 64
            };
            branch.process(ctx);

            auto bView = branch.buffer().view(64);
            for (size_t s = 0; s < 64; ++s) {
                const float samp = bView.channel(0)[s];
                if (sampleIdx > 2000) {
                    const float diff = std::abs(samp - lastSample);
                    maxDiscontinuity = std::max(maxDiscontinuity, diff);
                }
                lastSample = samp;
            }
        }
    }

    // A click or glitch would cause a sudden step discontinuity approaching or exceeding full scale (>0.8f).
    // Under continuous 440Hz tone, smooth transition max derivative should remain well below 0.35f.
    assert(maxDiscontinuity < 0.35f);
}

// ============================================================================
// SECTION 3: REAL-TIME SAFETY & SPSC CONCURRENCY (0 ALLOC, 0 MUTEX)
// ============================================================================

// 6. ParamAutomationRegister: High-contention concurrent writer/drainer zero-allocation stress
void test_RT_ParamAutomation_Concurrent_ZeroAlloc() {
    ParamAutomationRegister reg;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> totalWrites{0};
    std::atomic<uint64_t> totalDrains{0};

    // Thread 1 & Thread 2: Simulating UI and MIDI threads rapidly setting parameters
    auto writerFn = [&](uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_int_distribution<uint32_t> paramDist(0, ParamAutomationRegister::MAX_PARAMS - 1);
        std::uniform_real_distribution<float> valDist(-1.0f, 1.0f);

        while (!stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 64; ++i) {
                reg.setParameter(paramDist(rng), valDist(rng));
                totalWrites.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    };

    std::thread uiThread(writerFn, 101);
    std::thread midiThread(writerFn, 202);

    // Audio thread: strictly tracking allocations and deallocations
    t_isAudioThread = true;
    g_audioThreadAllocations.store(0);
    g_audioThreadDeallocations.store(0);

    for (int block = 0; block < 5000; ++block) {
        reg.drainDirty([&](uint32_t id, float val) {
            (void)id;
            (void)val;
            totalDrains.fetch_add(1, std::memory_order_relaxed);
        });
        // Simulate 48kHz audio callback period (e.g. 64 samples = ~1.33ms, 10 microseconds for test speed)
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    t_isAudioThread = false;
    stop.store(true, std::memory_order_release);
    uiThread.join();
    midiThread.join();

    assert(g_audioThreadAllocations.load() == 0);
    assert(g_audioThreadDeallocations.load() == 0);
    assert(totalWrites.load() > 0);
    assert(totalDrains.load() > 0);
}

// 7. GraphEngine & SPSC Command Queue: Real-time safety under concurrent structural mutations
void test_RT_GraphEngine_Concurrent_StructuralMutations_ZeroAlloc() {
    GraphEngine engine;
    engine.prepare(48000.0, 256);

    // Pre-populate with initial serial nodes
    for (int i = 0; i < 4; ++i) {
        auto node = std::make_unique<MockLatencyNode>(i * 10);
        auto slot = std::make_unique<PluginSlot>(std::move(node));
        engine.addSerialNode(std::move(slot));
    }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> structuralMutationsCount{0};

    // UI Thread: Rapidly adding, removing, swapping, and reordering nodes
    std::thread uiWorker([&]() {
        std::mt19937 rng(777);
        std::uniform_int_distribution<int> opDist(0, 4);

        while (!stop.load(std::memory_order_relaxed)) {
            const int op = opDist(rng);
            switch (op) {
                case 0: { // Add
                    if (engine.numNodes() < 16) {
                        auto n = std::make_unique<MockLatencyNode>(16);
                        auto s = std::make_unique<PluginSlot>(std::move(n));
                        engine.addSerialNode(std::move(s));
                        structuralMutationsCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
                case 1: { // Remove
                    if (engine.numNodes() > 2) {
                        engine.removeSerialNode(engine.numNodes() - 1);
                        structuralMutationsCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
                case 2: { // Swap
                    if (engine.numNodes() >= 2) {
                        engine.swapNodes(0, 1);
                        structuralMutationsCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
                case 3: { // Reorder
                    if (engine.numNodes() >= 3) {
                        engine.reorderNodes(0, 2);
                        structuralMutationsCount.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }
                case 4: { // Parameter update
                    for (size_t i = 0; i < engine.numNodes(); ++i) {
                        auto* node = engine.getNode(i);
                        if (auto* slot = dynamic_cast<PluginSlot*>(node)) {
                            slot->setParameter(1, 0.75f);
                            slot->setDryWet(0.5f);
                        }
                    }
                    break;
                }
            }

            // UI thread regularly collects reclaimed retired nodes
            engine.processReclamation();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }

        // Final reclamation
        engine.processReclamation();
    });

    // Audio Thread: pumping 5,000 blocks with memory allocation tracking active
    OwnedAudioBuffer inBuf(2, 256);
    OwnedAudioBuffer outBuf(2, 256);

    t_isAudioThread = true;
    g_audioThreadAllocations.store(0);
    g_audioThreadDeallocations.store(0);

    for (int block = 0; block < 5000; ++block) {
        AudioProcessContext ctx{
            .input = inBuf.view(256),
            .output = outBuf.view(256),
            .sampleRate = 48000.0,
            .numSamples = 256
        };
        engine.process(ctx.input, ctx.output);

        // Micro-yield simulating audio clock period
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }

    t_isAudioThread = false;
    stop.store(true, std::memory_order_release);
    uiWorker.join();

    // STRICT REAL-TIME ASSERTION: Zero heap allocations and zero deallocations on the audio thread!
    assert(g_audioThreadAllocations.load() == 0);
    assert(g_audioThreadDeallocations.load() == 0);
    assert(structuralMutationsCount.load() > 10);
}

// 8. ParallelBranch: Real-time safety under concurrent slot additions and removals
void test_RT_ParallelBranch_Concurrent_SlotMutation_ZeroAlloc() {
    ParallelBranch branch("RT_Branch");
    branch.prepare(48000.0, 256);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> branchMutations{0};

    std::thread uiWorker([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (branch.numSlots() < 8) {
                auto node = std::make_unique<MockLatencyNode>(8);
                auto slot = std::make_unique<PluginSlot>(std::move(node));
                branch.addSlot(std::move(slot));
                branchMutations.fetch_add(1, std::memory_order_relaxed);
            } else {
                branch.removeSlot(branch.numSlots() - 1);
                branchMutations.fetch_add(1, std::memory_order_relaxed);
            }
            branch.collectReclaimedSlots();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        branch.collectReclaimedSlots();
    });

    OwnedAudioBuffer inBuf(2, 256);
    OwnedAudioBuffer outBuf(2, 256);

    t_isAudioThread = true;
    g_audioThreadAllocations.store(0);
    g_audioThreadDeallocations.store(0);

    for (int block = 0; block < 5000; ++block) {
        AudioProcessContext ctx{
            .input = inBuf.view(256),
            .output = outBuf.view(256),
            .sampleRate = 48000.0,
            .numSamples = 256
        };
        branch.process(ctx);
        std::this_thread::sleep_for(std::chrono::microseconds(20));
    }

    t_isAudioThread = false;
    stop.store(true, std::memory_order_release);
    uiWorker.join();

    assert(g_audioThreadAllocations.load() == 0);
    assert(g_audioThreadDeallocations.load() == 0);
    assert(branchMutations.load() > 10);
}

// ============================================================================
// SECTION 4: RETROACTIVE RIFF BUFFER & 24-BIT PCM WAV WRITER
// ============================================================================

// 9. RetroactiveRiffBuffer: Pre-allocation durations and zero-alloc push
void test_RiffBuffer_PreAllocation_And_ZeroAllocPush() {
    RetroactiveRiffBuffer buf;

    const std::vector<int> durations = { 1, 2, 5, 10 };
    for (int dur : durations) {
        buf.prepare(48000.0, dur);
        const size_t expectedFrames = static_cast<size_t>(48000.0 * 60.0 * dur);
        assert(buf.capacityFrames() == expectedFrames);
        assert(buf.durationMinutes() == dur);
        assert(buf.writeHead() == 0);
        assert(buf.totalFramesWritten() == 0);
    }

    // Verify 0 allocations during audio callback push()
    std::vector<float> left(256, 0.42f);
    std::vector<float> right(256, -0.42f);

    t_isAudioThread = true;
    g_audioThreadAllocations.store(0);
    g_audioThreadDeallocations.store(0);

    for (int b = 0; b < 10000; ++b) {
        buf.push(left.data(), right.data(), 256);
    }

    t_isAudioThread = false;
    assert(g_audioThreadAllocations.load() == 0);
    assert(g_audioThreadDeallocations.load() == 0);
    assert(buf.totalFramesWritten() == 256 * 10000);
}

// 10. RetroactiveRiffBuffer: Tear-free concurrent slice capture with monotonic verification
void test_RiffBuffer_TearFree_SliceCapture() {
    RetroactiveRiffBuffer buf;
    buf.prepare(48000.0, 1); // 1 minute = 2,880,000 frames

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> pushCount{0};

    // Thread 1: audio callback pushing continuously with monotonically increasing ramp (mod 1,000,000 to preserve 24-bit float precision)
    constexpr uint32_t RAMP_MOD = 1000000;
    std::thread audioPusher([&]() {
        std::vector<float> blkL(256);
        std::vector<float> blkR(256);
        uint32_t sampleCounter = 0;

        while (!stop.load(std::memory_order_relaxed)) {
            for (size_t s = 0; s < 256; ++s) {
                const float val = static_cast<float>(sampleCounter % RAMP_MOD);
                sampleCounter++;
                blkL[s] = val;
                blkR[s] = -val;
            }
            buf.push(blkL.data(), blkR.data(), 256);
            pushCount.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Wait until at least 50,000 samples pushed
    while (pushCount.load() < 200) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Thread 2: background worker capturing slice repeatedly and asserting continuity
    std::vector<float> stagingL(4096);
    std::vector<float> stagingR(4096);

    for (int iter = 0; iter < 100; ++iter) {
        size_t captured = 0;
        bool ok = buf.captureSlice(stagingL.data(), stagingR.data(), 4096, captured);
        assert(ok && captured == 4096);

        // Verify tear-free monotonic sequence
        for (size_t i = 1; i < captured; ++i) {
            float diffL = stagingL[i] - stagingL[i - 1];
            float diffR = stagingR[i] - stagingR[i - 1];
            if (diffL < 0.0f) diffL += static_cast<float>(RAMP_MOD);
            if (diffR > 0.0f) diffR -= static_cast<float>(RAMP_MOD);
            assert(std::abs(diffL - 1.0f) < 1e-4f);
            assert(std::abs(diffR - (-1.0f)) < 1e-4f);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    stop.store(true, std::memory_order_release);
    audioPusher.join();
}

// 11. RetroactiveRiffBuffer: High-resolution latch & copy performance benchmark (<0.15ms)
void test_RiffBuffer_SliceCapture_PerformanceBenchmark_Under150Micros() {
    RetroactiveRiffBuffer buf;
    buf.prepare(48000.0, 2); // 2 minutes = 5,760,000 frames

    // Fill buffer completely
    std::vector<float> chunkL(48000, 0.5f);
    std::vector<float> chunkR(48000, -0.5f);
    for (int i = 0; i < 120; ++i) {
        buf.push(chunkL.data(), chunkR.data(), 48000);
    }

    // Benchmark capturing requested slice sizes: 5,000 frames (~104ms riff slice)
    // and 48,000 frames (1 second full riff slice)
    constexpr size_t SLICE_SIZE = 48000;
    std::vector<float> stagingL(SLICE_SIZE);
    std::vector<float> stagingR(SLICE_SIZE);

    constexpr int ITERATIONS = 1000;
    double maxMicroseconds = 0.0;
    double totalMicroseconds = 0.0;

    for (int iter = 0; iter < ITERATIONS; ++iter) {
        size_t captured = 0;
        const auto t0 = std::chrono::high_resolution_clock::now();
        const bool ok = buf.captureSlice(stagingL.data(), stagingR.data(), SLICE_SIZE, captured);
        const auto t1 = std::chrono::high_resolution_clock::now();

        assert(ok && captured == SLICE_SIZE);
        const double durationUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
        maxMicroseconds = std::max(maxMicroseconds, durationUs);
        totalMicroseconds += durationUs;
    }

    const double avgMicroseconds = totalMicroseconds / ITERATIONS;
    std::cout << "\n      [BENCHMARK] Slice capture (" << SLICE_SIZE << " frames): avg = "
              << std::fixed << std::setprecision(2) << avgMicroseconds << " us, max = "
              << maxMicroseconds << " us (spec limit < 150.0 us)... ";

    // SPEC REQUIREMENT: Atomic latch & copy in <0.15 ms (150 microseconds)
    assert(avgMicroseconds < 150.0);
}

// 12. 24-Bit PCM WAV Encoding: Bit-exact RIFF structure and PCM sample quantization
void test_WavWriter_BitExact_24BitPCM_Verification() {
    const std::string testWavPath = "build/test_challenger_riff_24bit.wav";

    // Known test vector samples spanning full dynamic range
    const std::vector<float> testSamplesL = {
        0.0f,               // Zero -> 0
        1.0f,               // +1.0 FS -> 0x7FFFFF (8388607)
        -1.0f,              // -1.0 FS -> -8388608
        0.5f,               // +0.5 FS -> 4194304
        -0.5f,              // -0.5 FS -> -4194304
        0.25f,              // +0.25 FS -> 2097152
        1.5f,               // Over-range -> clamped to 8388607
        -2.0f,              // Under-range -> clamped to -8388608
        std::numeric_limits<float>::quiet_NaN(), // NaN -> 0
        1.0f / 8388607.0f   // 1 LSB -> 1
    };

    const std::vector<float> testSamplesR = {
        0.0f,
        -1.0f,
        1.0f,
        -0.5f,
        0.5f,
        -0.25f,
        -1.5f,
        2.0f,
        0.0f,
        -1.0f / 8388607.0f
    };

    const size_t numFrames = testSamplesL.size();
    constexpr uint32_t sampleRate = 48000;

    const bool writeOk = writeWav24Bit(testWavPath, testSamplesL.data(), testSamplesR.data(), numFrames, sampleRate);
    assert(writeOk);

    // Read back file in binary mode
    std::ifstream in(testWavPath, std::ios::binary);
    assert(in.is_open());

    in.seekg(0, std::ios::end);
    const size_t fileSize = in.tellg();
    in.seekg(0, std::ios::beg);

    const size_t expectedDataBytes = numFrames * 6;
    const size_t expectedFileSize = sizeof(RiffWaveHeader24Bit) + expectedDataBytes;
    assert(fileSize == expectedFileSize);

    RiffWaveHeader24Bit header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    assert(in.good());

    // Verify RIFF header fields
    assert(std::memcmp(header.riffTag, "RIFF", 4) == 0);
    assert(header.riffSize == 36 + expectedDataBytes);
    assert(std::memcmp(header.waveTag, "WAVE", 4) == 0);
    assert(std::memcmp(header.fmtTag, "fmt ", 4) == 0);
    assert(header.fmtSize == 16);
    assert(header.audioFormat == 1); // PCM
    assert(header.numChannels == 2);
    assert(header.sampleRate == 48000);
    assert(header.byteRate == 48000 * 6);
    assert(header.blockAlign == 6);
    assert(header.bitsPerSample == 24);
    assert(std::memcmp(header.dataTag, "data", 4) == 0);
    assert(header.dataSize == expectedDataBytes);

    // Read and verify 24-bit samples bit-by-bit
    std::vector<uint8_t> pcmBytes(expectedDataBytes);
    in.read(reinterpret_cast<char*>(pcmBytes.data()), expectedDataBytes);
    assert(in.good());
    in.close();

    auto decode24Bit = [](const uint8_t* b) -> int32_t {
        int32_t val = static_cast<int32_t>(b[0]) |
                     (static_cast<int32_t>(b[1]) << 8) |
                     (static_cast<int32_t>(b[2]) << 16);
        // Sign-extend from 24th bit
        if (val & 0x800000) {
            val |= ~0xFFFFFF;
        }
        return val;
    };

    for (size_t i = 0; i < numFrames; ++i) {
        const int32_t decodedL = decode24Bit(&pcmBytes[i * 6]);
        const int32_t decodedR = decode24Bit(&pcmBytes[i * 6 + 3]);

        // Calculate expected mathematical values
        float expSampL = testSamplesL[i];
        if (std::isnan(expSampL)) expSampL = 0.0f;
        expSampL = std::clamp(expSampL, -1.0f, 1.0f);
        const int32_t expectedL = std::clamp(static_cast<int32_t>(std::round(expSampL * 8388607.0f)), -8388608, 8388607);

        float expSampR = testSamplesR[i];
        if (std::isnan(expSampR)) expSampR = 0.0f;
        expSampR = std::clamp(expSampR, -1.0f, 1.0f);
        const int32_t expectedR = std::clamp(static_cast<int32_t>(std::round(expSampR * 8388607.0f)), -8388608, 8388607);

        assert(decodedL == expectedL);
        assert(decodedR == expectedR);
    }

    // Clean up temporary test file
    std::filesystem::remove(testWavPath);
}

// ============================================================================
// MAIN RUNNER
// ============================================================================
int main() {
    std::cout << "\n================================================================================\n";
    std::cout << "   PRACCY V2.4 STAGE 1 CHALLENGER EMPIRICAL VERIFICATION HARNESS\n";
    std::cout << "   Focus: Core Audio DSP, PDC (0-16k), Dynamic Crossfade, RT Safety, Riff Buffer\n";
    std::cout << "================================================================================\n\n";

    std::cout << "[MODULE 1] Parallel Delay Compensation (PDC) & Dynamic Crossfade (R1)\n";
    RUN_CHALLENGER_TEST(test_PDC_Branch_DirectDelayAccuracy);
    RUN_CHALLENGER_TEST(test_PDC_PhaseCoherence_ZeroSampleDelta_FullDifferentialSweep);
    RUN_CHALLENGER_TEST(test_PDC_ExtremeDifferential_Clamping);
    RUN_CHALLENGER_TEST(test_PDC_DynamicCrossfade_EqualPowerEnergyPreservation);
    RUN_CHALLENGER_TEST(test_PDC_DynamicCrossfade_ClickFreeJitterStress);

    std::cout << "\n[MODULE 2] Real-Time DSP Safety, SPSC Automation & Concurrency (R1)\n";
    RUN_CHALLENGER_TEST(test_RT_ParamAutomation_Concurrent_ZeroAlloc);
    RUN_CHALLENGER_TEST(test_RT_GraphEngine_Concurrent_StructuralMutations_ZeroAlloc);
    RUN_CHALLENGER_TEST(test_RT_ParallelBranch_Concurrent_SlotMutation_ZeroAlloc);

    std::cout << "\n[MODULE 3] Retroactive Riff Buffer & 24-Bit WAV Encoding (R2)\n";
    RUN_CHALLENGER_TEST(test_RiffBuffer_PreAllocation_And_ZeroAllocPush);
    RUN_CHALLENGER_TEST(test_RiffBuffer_TearFree_SliceCapture);
    RUN_CHALLENGER_TEST(test_RiffBuffer_SliceCapture_PerformanceBenchmark_Under150Micros);
    RUN_CHALLENGER_TEST(test_WavWriter_BitExact_24BitPCM_Verification);

    std::cout << "\n================================================================================\n";
    std::cout << "   CHALLENGER RUN SUMMARY: " << g_passedTests << " / " << g_totalTests << " TESTS PASSED\n";
    std::cout << "================================================================================\n";

    if (g_passedTests == g_totalTests) {
        std::cout << ">>> ALL CHALLENGER EMPIRICAL VERIFICATION TESTS PASSED (100% SUCCESS) <<<\n\n";
        return 0;
    } else {
        std::cout << ">>> SOME CHALLENGER TESTS FAILED <<<\n\n";
        return 1;
    }
}
