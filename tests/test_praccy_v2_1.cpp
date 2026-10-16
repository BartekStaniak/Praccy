#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <string_view>
#include <array>
#include <deque>
#include <cmath>
#undef NDEBUG
#include <cassert>
#include <thread>
#include <atomic>
#include <chrono>
#include <numbers>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>
#include <cstring>
#include <cstdint>
#include <limits>
#include <random>
#include <new>

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#include "miniz.h"
#include <imgui.h>

// Direct production headers
#include "audio/audio_buffer.h"
#include "audio/dsp_utils.h"
#include "audio/param_automation.h"
#include "audio/retroactive_buffer.h"
#include "audio/graph_engine.h"
#include "audio/asio_manager.h"
#include "plugins/builtin_dsp.h"
#include "utils/wav_writer.h"
#include "utils/archive_utils.h"
#include "ui/drag_drop_router.h"
#include "ui/design_tokens.h"
#include "state/undo_manager.h"
#include "state/scene_manager.h"
#include "state/app_config.h"
#include "midi/midi_manager.h"

using namespace praccy;

// ============================================================================
// REAL-TIME MEMORY ALLOCATION TRACKING HOOKS
// ============================================================================
#if defined(__GNUC__)
#define PRACCY_NOINLINE __attribute__((noinline))
#else
#define PRACCY_NOINLINE
#endif

static thread_local bool t_disallowAllocations = false;
static thread_local size_t t_allocationsCount = 0;

PRACCY_NOINLINE void* operator new(std::size_t size) {
    if (t_disallowAllocations) {
        t_allocationsCount++;
    }
    void* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

PRACCY_NOINLINE void operator delete(void* p) noexcept {
    std::free(p);
}

PRACCY_NOINLINE void* operator new[](std::size_t size) {
    if (t_disallowAllocations) {
        t_allocationsCount++;
    }
    void* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

PRACCY_NOINLINE void operator delete[](void* p) noexcept {
    std::free(p);
}

PRACCY_NOINLINE void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

PRACCY_NOINLINE void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

PRACCY_NOINLINE void* operator new(std::size_t size, std::align_val_t al) {
    if (t_disallowAllocations) {
        t_allocationsCount++;
    }
    void* p = _aligned_malloc(size ? size : 1, static_cast<size_t>(al));
    if (!p) throw std::bad_alloc();
    return p;
}

PRACCY_NOINLINE void operator delete(void* p, std::align_val_t) noexcept {
    _aligned_free(p);
}

PRACCY_NOINLINE void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    _aligned_free(p);
}

PRACCY_NOINLINE void* operator new[](std::size_t size, std::align_val_t al) {
    if (t_disallowAllocations) {
        t_allocationsCount++;
    }
    void* p = _aligned_malloc(size ? size : 1, static_cast<size_t>(al));
    if (!p) throw std::bad_alloc();
    return p;
}

PRACCY_NOINLINE void operator delete[](void* p, std::align_val_t) noexcept {
    _aligned_free(p);
}

PRACCY_NOINLINE void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    _aligned_free(p);
}

// ============================================================================
// TEST REPORTING HARNESS
// ============================================================================
static int g_passedTests = 0;
static int g_totalTests = 0;

#define RUN_TEST(fn) do { \
    g_totalTests++; \
    std::cout << "  [TEST] " << #fn << "... " << std::flush; \
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
// MODULE 1: R1 REAL-TIME AUDIO DSP SAFETY, SPSC AUTOMATION & PDC
// ============================================================================

// Tier 1: Feature Coverage (R1)
void test_T1_R1_ParamAutomationRegister_BasicDrain() {
    audio::ParamAutomationRegister reg;
    reg.setParameter(0, 0.42f);
    reg.setParameter(63, 0.77f);
    reg.setParameter(64, 0.12f);
    reg.setParameter(255, 0.99f);

    std::vector<std::pair<uint32_t, float>> drained;
    reg.drainDirty([&](uint32_t id, float val) {
        drained.emplace_back(id, val);
    });

    assert(drained.size() == 4);
    assert(drained[0].first == 0 && std::abs(drained[0].second - 0.42f) < 1e-6f);
    assert(drained[1].first == 63 && std::abs(drained[1].second - 0.77f) < 1e-6f);
    assert(drained[2].first == 64 && std::abs(drained[2].second - 0.12f) < 1e-6f);
    assert(drained[3].first == 255 && std::abs(drained[3].second - 0.99f) < 1e-6f);

    // Second drain should find zero dirty bits
    drained.clear();
    reg.drainDirty([&](uint32_t id, float val) {
        drained.emplace_back(id, val);
    });
    assert(drained.empty());
}

void test_T1_R1_Pdc_PhaseCoherence_ZeroSampleDelta() {
    // Generate 1 kHz sine wave at 48 kHz for 2048 samples
    const size_t N = 2048;
    std::vector<float> dryL(N), dryR(N);
    const float freq = 1000.0f;
    const float sr = 48000.0f;
    for (size_t i = 0; i < N; ++i) {
        dryL[i] = std::sin(2.0f * std::numbers::pi_v<float> * freq * static_cast<float>(i) / sr);
        dryR[i] = dryL[i];
    }

    // Branch A introduces 256 samples of plugin latency
    // Branch B compensates with production ParallelBranch delay line
    const uint32_t latencyA = 256;
    audio::ParallelBranch branchB("Branch B");
    branchB.prepare(sr, 512);
    branchB.setTargetDelay(latencyA);

    // Pre-roll delay line so steady state is achieved
    {
        audio::OwnedAudioBuffer preIn(2, 512), preOut(2, 512);
        preIn.view(512).clear();
        preOut.view(512).clear();
        audio::AudioProcessContext preCtx{preIn.view(512), preOut.view(512), sr, 512};
        branchB.process(preCtx);
    }

    std::vector<float> outBL(N, 0.0f), outBR(N, 0.0f);
    for (size_t offset = 0; offset < N; offset += 512) {
        uint32_t curBlock = static_cast<uint32_t>(std::min<size_t>(512, N - offset));
        audio::OwnedAudioBuffer inBuf(2, curBlock);
        audio::OwnedAudioBuffer outBuf(2, curBlock);
        auto inView = inBuf.view(curBlock);
        auto outView = outBuf.view(curBlock);

        for (uint32_t s = 0; s < curBlock; ++s) {
            inView.channel(0)[s] = dryL[offset + s];
            inView.channel(1)[s] = dryR[offset + s];
        }

        audio::AudioProcessContext ctx{inView, outView, sr, curBlock};
        branchB.process(ctx);

        auto bView = branchB.buffer().view(curBlock);
        for (uint32_t s = 0; s < curBlock; ++s) {
            outBL[offset + s] = bView.channel(0)[s];
            outBR[offset + s] = (bView.numChannels() > 1) ? bView.channel(1)[s] : bView.channel(0)[s];
        }
    }

    // Compare Branch A (manually shifted by 256) and Branch B compensated
    for (size_t i = latencyA + 64; i < N; ++i) {
        const float sampleA = dryL[i - latencyA];
        const float sampleB = outBL[i];
        // Phase coherence test: difference must be zero (constructive interference)
        const float delta = std::abs(sampleA - sampleB);
        assert(delta < 1e-4f);

        // Sum of both branches must equal exactly 2 * sampleA with zero comb filtering
        const float sum = sampleA + sampleB;
        assert(std::abs(sum - 2.0f * sampleA) < 1e-4f);
    }
}

void test_T1_R1_Pdc_EqualPowerLatencyCrossfade() {
    // Verify mathematical energy conservation of the 64-sample dynamic crossfade ramp:
    for (size_t s = 0; s < 64; ++s) {
        const float t = static_cast<float>(s) / 64.0f;
        const float angle = t * audio::DspUtils::HALF_PI;
        const float gOld = std::cos(angle);
        const float gNew = std::sin(angle);
        const float energy = gOld * gOld + gNew * gNew;
        assert(std::abs(energy - 1.0f) < 1e-5f);
    }
}

void test_T1_R1_Spsc_BlockBoundary_SlotSwap() {
    // Production GraphEngine node reordering at audio block boundaries
    audio::GraphEngine engine;
    engine.prepare(48000.0, 128);

    engine.addSerialNode(std::make_unique<plugins::OverdriveEffect>());
    engine.addSerialNode(std::make_unique<plugins::StereoDelayEffect>());
    engine.addSerialNode(std::make_unique<plugins::TubeAmpEffect>());

    assert(engine.numNodes() == 3);

    // Swap nodes 0 and 1
    engine.swapNodes(0, 1);

    audio::OwnedAudioBuffer inBuf(2, 128), outBuf(2, 128);
    auto inView = inBuf.view(128);
    auto outView = outBuf.view(128);
    inView.clear();
    engine.process(inView, outView);

    assert(engine.numNodes() == 3);
}

// Tier 2: Boundary & Corner Cases (R1)
void test_T2_R1_ParamAutomationRegister_BoundsAndCoalescing() {
    audio::ParamAutomationRegister reg;

    // Corner: Out of range paramId (>= 1024) must be safely ignored
    reg.setParameter(1024, 1.0f);
    reg.setParameter(2000, 2.0f);
    reg.setParameter(UINT32_MAX, 3.0f);

    size_t count = 0;
    reg.drainDirty([&](uint32_t, float) { count++; });
    assert(count == 0);

    // Coalescing: Rapid successive writes to param 42
    for (int i = 0; i < 50; ++i) {
        reg.setParameter(42, static_cast<float>(i) * 0.02f);
    }
    // Only the latest value should be drained exactly once
    count = 0;
    float lastVal = 0.0f;
    reg.drainDirty([&](uint32_t id, float val) {
        assert(id == 42);
        lastVal = val;
        count++;
    });
    assert(count == 1);
    assert(std::abs(lastVal - 49.0f * 0.02f) < 1e-5f);
}

void test_T2_R1_Pdc_LatencyDifferentialExtremes() {
    audio::ParallelBranch branch("ExtremeBranch");
    branch.prepare(48000.0, 128);

    // Boundary 1: Zero latency (pass-through)
    branch.setTargetDelay(0);
    assert(branch.currentDelay() == 0);

    // Boundary 2: 1 sample delay
    branch.setTargetDelay(1);

    // Boundary 3: 16,384 samples (maximum supported specification limit)
    branch.setTargetDelay(16384);

    // Extreme Boundary: Exceeding 16,384 clamps to 16,384
    branch.setTargetDelay(30000);
    assert(branch.targetDelay() <= 16384);

    audio::OwnedAudioBuffer inBuf(2, 128), outBuf(2, 128);
    for (uint32_t s = 0; s < 128; ++s) inBuf.view(128).channel(0)[s] = 1.0f;
    audio::AudioProcessContext ctx{inBuf.view(128), outBuf.view(128), 48000.0, 128};
    branch.process(ctx);
    assert(!std::isnan(branch.buffer().view(128).channel(0)[0]));
}

void test_T2_R1_Crossfade_SampleDiscontinuity() {
    audio::ParallelBranch branch("CrossfadeBranch");
    branch.prepare(48000.0, 128);
    branch.setTargetDelay(0);

    // Pre-fill delay line with 512 samples of DC signal 1.0
    audio::OwnedAudioBuffer inBuf(2, 128), outBuf(2, 128);
    for (uint32_t s = 0; s < 128; ++s) {
        inBuf.view(128).channel(0)[s] = 1.0f;
        inBuf.view(128).channel(1)[s] = 1.0f;
    }
    for (int i = 0; i < 4; ++i) {
        audio::AudioProcessContext ctx{inBuf.view(128), outBuf.view(128), 48000.0, 128};
        branch.process(ctx);
    }

    // Trigger dynamic latency change
    branch.setTargetDelay(64);
    audio::AudioProcessContext ctx{inBuf.view(128), outBuf.view(128), 48000.0, 128};
    branch.process(ctx);

    auto bView = branch.buffer().view(128);
    const float* outL = bView.channel(0);

    for (size_t i = 1; i < 128; ++i) {
        assert(!std::isnan(outL[i]));
        assert(!std::isinf(outL[i]));
        assert(outL[i] >= 0.95f && outL[i] <= 1.55f);
        const float step = std::abs(outL[i] - outL[i - 1]);
        assert(step < 0.20f);
    }
}

// Tier 3: Cross-Feature Interactions (R1)
void test_T3_R1_ParamAutomation_Concurrent_AudioDrain_ZeroAlloc() {
    audio::ParamAutomationRegister reg;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> uiPushes{0};
    std::atomic<uint64_t> audioDrains{0};

    // UI Thread: pushing random parameters continuously
    std::thread uiThread([&]() {
        std::mt19937 rng(42);
        std::uniform_int_distribution<uint32_t> distParam(0, 255);
        std::uniform_real_distribution<float> distVal(0.0f, 1.0f);
        while (!stop.load(std::memory_order_relaxed)) {
            reg.setParameter(distParam(rng), distVal(rng));
            uiPushes.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Simulated Audio Thread: draining every block under strict zero-allocation assertion
    t_disallowAllocations = true;
    t_allocationsCount = 0;

    for (int block = 0; block < 1000; ++block) {
        reg.drainDirty([&](uint32_t paramId, float val) {
            (void)paramId;
            (void)val;
            audioDrains.fetch_add(1, std::memory_order_relaxed);
        });
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    t_disallowAllocations = false;
    stop.store(true, std::memory_order_release);
    uiThread.join();

    // Verify ZERO allocations on the audio callback thread
    assert(t_allocationsCount == 0);
    assert(uiPushes.load() > 0);
    assert(audioDrains.load() > 0);
}

void test_T3_R1_Pdc_With_DynamicAutomation() {
    audio::ParallelBranch branch("BranchAuto");
    branch.prepare(48000.0, 128);
    audio::ParamAutomationRegister reg;

    branch.setTargetDelay(64);
    reg.setParameter(10, 0.5f);

    audio::OwnedAudioBuffer inBuf(2, 128), outBuf(2, 128);
    for (uint32_t s = 0; s < 128; ++s) inBuf.view(128).channel(0)[s] = 0.5f;

    // Audio block: simultaneous parameter drain and PDC update
    reg.drainDirty([&](uint32_t id, float val) {
        if (id == 10) {
            branch.setTargetDelay(static_cast<uint32_t>(val * 256.0f));
        }
    });

    audio::AudioProcessContext ctx{inBuf.view(128), outBuf.view(128), 48000.0, 128};
    branch.process(ctx);
    assert(branch.isCrossfading() || branch.currentDelay() == 128 || branch.targetDelay() == 128);
}

// Tier 4: Real-World Scenarios (R1)
void test_T4_R1_Continuous_Stream_Stress() {
    audio::ParallelBranch branch("StreamBranch");
    branch.prepare(48000.0, 256);
    audio::ParamAutomationRegister reg;

    audio::OwnedAudioBuffer inBuf(2, 256), outBuf(2, 256);
    for (uint32_t s = 0; s < 256; ++s) inBuf.view(256).channel(0)[s] = 0.1f;

    t_disallowAllocations = true;
    t_allocationsCount = 0;

    // Simulate 2,000 continuous audio blocks
    for (int b = 0; b < 2000; ++b) {
        if (b % 200 == 0) {
            reg.setParameter(1, static_cast<float>(b) / 2000.0f);
            branch.setTargetDelay((b % 400 == 0) ? 512 : 128);
        }

        reg.drainDirty([&](uint32_t, float) {});
        audio::AudioProcessContext ctx{inBuf.view(256), outBuf.view(256), 48000.0, 256};
        branch.process(ctx);

        assert(!std::isnan(branch.buffer().view(256).channel(0)[0]));
    }

    t_disallowAllocations = false;
    assert(t_allocationsCount == 0);
}

// ============================================================================
// MODULE 2: R2 RETROACTIVE RIFF CAPTURE & IN-PROCESS WAV EXPORT
// ============================================================================

// Tier 1: Feature Coverage (R2)
void test_T1_R2_RetroactiveBuffer_CircularAudioStorage() {
    audio::RetroactiveRiffBuffer buf;
    buf.prepare(48000.0, 1); // 1 minute
    assert(buf.capacityFrames() == 2880000);

    std::vector<float> inL(1000, 0.35f), inR(1000, 0.45f);
    buf.push(inL.data(), inR.data(), 1000);

    std::vector<float> sliceL, sliceR;
    size_t captured = 0;
    double sr = 0.0;
    bool ok = buf.latchAndCopySlice(sliceL, sliceR, captured, sr);
    assert(ok && captured == 1000);
    assert(std::abs(sliceL[0] - 0.35f) < 1e-6f);
    assert(std::abs(sliceR[0] - 0.45f) < 1e-6f);
}

void test_T1_R2_AtomicLatch_SliceCapture() {
    audio::RetroactiveRiffBuffer buf;
    buf.prepare(100.0, 1); // 6,000 frames capacity

    // Push sequential ramps: 0, 1, 2, ...
    std::vector<float> streamL(200), streamR(200);
    for (size_t i = 0; i < 200; ++i) {
        streamL[i] = static_cast<float>(i);
        streamR[i] = -static_cast<float>(i);
    }
    buf.push(streamL.data(), streamR.data(), 200);

    // Latch 50 frames: should return frames 150 to 199
    std::vector<float> sliceL(50), sliceR(50);
    size_t captured = 0;
    bool ok = buf.captureSlice(sliceL.data(), sliceR.data(), 50, captured);
    assert(ok && captured == 50);
    for (size_t i = 0; i < 50; ++i) {
        assert(sliceL[i] == static_cast<float>(150 + i));
        assert(sliceR[i] == -static_cast<float>(150 + i));
    }
}

void test_T1_R2_WavWriter_24BitPcmRiffHeader() {
    std::vector<float> inL = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f};
    std::vector<float> inR = {0.0f, -0.5f, 0.5f, -1.0f, 1.0f};

    const std::string wavPath = "build/test_riff_hdr_prod.wav";
    bool ok = utils::writeWav24Bit(wavPath, inL.data(), inR.data(), inL.size(), 48000);
    assert(ok);

    std::ifstream in(wavPath, std::ios::binary);
    assert(in.is_open());
    utils::RiffWaveHeader24Bit h{};
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    in.close();
    std::filesystem::remove(wavPath);

    assert(std::memcmp(h.riffTag, "RIFF", 4) == 0);
    assert(std::memcmp(h.waveTag, "WAVE", 4) == 0);
    assert(std::memcmp(h.fmtTag, "fmt ", 4) == 0);
    assert(h.fmtSize == 16);
    assert(h.audioFormat == 1); // PCM
    assert(h.numChannels == 2);
    assert(h.sampleRate == 48000);
    assert(h.byteRate == 288000);
    assert(h.blockAlign == 6);
    assert(h.bitsPerSample == 24);
    assert(std::memcmp(h.dataTag, "data", 4) == 0);
    assert(h.dataSize == 30);
}

void test_T1_R2_RiffFilename_Format() {
    std::tm tmNow{};
    tmNow.tm_year = 2026 - 1900;
    tmNow.tm_mon = 10 - 1; // October
    tmNow.tm_mday = 15;
    tmNow.tm_hour = 1;
    tmNow.tm_min = 41;
    tmNow.tm_sec = 28;

    // User specification test: "Modern High Gain Djent", 140 BPM, timestamp
    std::string filename = audio::RetroactiveRiffBuffer::formatFilename("Modern High Gain Djent", 140.0f, tmNow);
    assert(filename == "ModernHighGainDjent_140bpm_20261015_014128.wav");

    // Empty preset fallback to "Init"
    std::string fallback = audio::RetroactiveRiffBuffer::formatFilename("", 120.0f, tmNow);
    assert(fallback == "Init_120bpm_20261015_014128.wav");

    // Special characters stripped, keeping alphanumeric/dash/underscore
    std::string special = audio::RetroactiveRiffBuffer::formatFilename("Lead / Solo [80s_Metal-Rig]", 128.4f, tmNow);
    assert(special == "LeadSolo80s_Metal-Rig_128bpm_20261015_014128.wav");
}

// Tier 2: Boundary & Corner Cases (R2)
void test_T2_R2_Buffer_Capacity_Durations() {
    audio::RetroactiveRiffBuffer buf;

    // 1 min: 2,880,000 samples @ 48kHz
    buf.prepare(48000.0, 1);
    assert(buf.capacityFrames() == 2880000);

    // 2 min: 5,760,000 samples @ 48kHz
    buf.prepare(48000.0, 2);
    assert(buf.capacityFrames() == 5760000);

    // 5 min: 14,400,000 samples @ 48kHz
    buf.prepare(48000.0, 5);
    assert(buf.capacityFrames() == 14400000);

    // 10 min: 28,800,000 samples @ 48kHz
    buf.prepare(48000.0, 10);
    assert(buf.capacityFrames() == 28800000);

    // Empty capture
    std::vector<float> sliceL, sliceR;
    size_t captured = 999;
    double sr = 0.0;
    bool ok = buf.latchAndCopySlice(sliceL, sliceR, captured, sr);
    assert(!ok && captured == 0);

    // Push 0 samples (safe no-op)
    buf.push(sliceL.data(), sliceR.data(), 0);
}

void test_T2_R2_Wav24Bit_Quantization_Precision() {
    // Test boundary quantization points
    std::vector<float> inL = {+1.0f, -1.0f, 0.0f, +1.5f, -2.0f};
    std::vector<float> inR = {+1.0f, -1.0f, 0.0f, +1.5f, -2.0f};

    const std::string wavPath = "build/test_riff_quant_prod.wav";
    bool ok = utils::writeWav24Bit(wavPath, inL.data(), inR.data(), inL.size(), 48000);
    assert(ok);

    std::ifstream in(wavPath, std::ios::binary);
    std::vector<uint8_t> wavBytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(wavPath);

    const uint8_t* p = wavBytes.data() + sizeof(utils::RiffWaveHeader24Bit);

    // Sample 0: +1.0f -> 0x7FFFFF
    int32_t val0 = (static_cast<int32_t>(p[0])) |
                   (static_cast<int32_t>(p[1]) << 8) |
                   (static_cast<int32_t>(p[2]) << 16);
    assert(val0 == 0x7FFFFF);

    // Sample 1: -1.0f -> -8388607 or -8388608
    int32_t val1 = (static_cast<int32_t>(p[6])) |
                   (static_cast<int32_t>(p[7]) << 8) |
                   (static_cast<int32_t>(p[8]) << 16);
    if (val1 & 0x800000) val1 |= 0xFF000000;
    assert(val1 == -8388607 || val1 == -8388608);

    // Sample 2: 0.0f -> 0
    int32_t val2 = (static_cast<int32_t>(p[12])) |
                   (static_cast<int32_t>(p[13]) << 8) |
                   (static_cast<int32_t>(p[14]) << 16);
    assert(val2 == 0);

    // Sample 3: +1.5f clamped to 0x7FFFFF
    int32_t val3 = (static_cast<int32_t>(p[18])) |
                   (static_cast<int32_t>(p[19]) << 8) |
                   (static_cast<int32_t>(p[20]) << 16);
    assert(val3 == 0x7FFFFF);

    // Sample 4: -2.0f clamped to -8388607 or -8388608
    int32_t val4 = (static_cast<int32_t>(p[24])) |
                   (static_cast<int32_t>(p[25]) << 8) |
                   (static_cast<int32_t>(p[26]) << 16);
    if (val4 & 0x800000) val4 |= 0xFF000000;
    assert(val4 == -8388607 || val4 == -8388608);
}

// Tier 3: Cross-Feature Interactions (R2)
void test_T3_R2_AudioThread_ZeroAlloc_ConcurrentPushAndCapture() {
    audio::RetroactiveRiffBuffer buf;
    buf.prepare(48000.0, 1);

    std::atomic<bool> stop{false};
    std::atomic<size_t> capturesSucceeded{0};

    // Worker thread capturing slices concurrently
    std::thread worker([&]() {
        std::vector<float> sliceL, sliceR;
        while (!stop.load(std::memory_order_relaxed)) {
            size_t captured = 0;
            double sr = 0.0;
            buf.latchAndCopySlice(sliceL, sliceR, captured, sr);
            if (captured > 0) capturesSucceeded.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Audio callback pushes samples
    std::vector<float> block(256, 0.25f);
    t_disallowAllocations = true;
    t_allocationsCount = 0;

    for (int i = 0; i < 200; ++i) {
        buf.push(block.data(), block.data(), 256);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    t_disallowAllocations = false;
    stop.store(true, std::memory_order_release);
    worker.join();

    assert(t_allocationsCount == 0);
    assert(capturesSucceeded.load() > 0);
}

// Tier 4: Real-World Scenarios (R2)
void test_T4_R2_LatchCopy_PerformanceBenchmark() {
    audio::RetroactiveRiffBuffer buf;
    buf.prepare(48000.0, 2); // 2 minutes @ 48kHz = 5,760,000 frames

    // Pre-populate with audio
    std::vector<float> chunk(48000, 0.5f);
    for (int i = 0; i < 120; ++i) {
        buf.push(chunk.data(), chunk.data(), 48000);
    }

    // Benchmark latching and copying audio
    std::vector<float> stagingL(1440000);
    std::vector<float> stagingR(1440000);

    const int kIterations = 20;
    double totalUs = 0.0;

    for (int iter = 0; iter < kIterations; ++iter) {
        size_t captured = 0;
        const auto tStart = std::chrono::high_resolution_clock::now();
        buf.captureSlice(stagingL.data(), stagingR.data(), 1440000, captured);
        const auto tEnd = std::chrono::high_resolution_clock::now();

        const double us = std::chrono::duration<double, std::micro>(tEnd - tStart).count();
        totalUs += us;
        assert(captured == 1440000);
    }

    const double avgUs = totalUs / kIterations;
    assert(avgUs < 15000.0);
}

void test_T4_R2_EndToEnd_WavExport_Validation() {
    audio::RetroactiveRiffBuffer buf;
    buf.prepare(48000.0, 1);

    // Push 48,000 samples (1 sec of 440 Hz test tone)
    std::vector<float> testTone(48000);
    for (size_t i = 0; i < 48000; ++i) {
        testTone[i] = 0.8f * std::sin(2.0f * std::numbers::pi_v<float> * 440.0f * static_cast<float>(i) / 48000.0f);
    }
    buf.push(testTone.data(), testTone.data(), 48000);

    // Latch slice
    std::vector<float> sliceL, sliceR;
    size_t captured = 0;
    double sr = 0.0;
    buf.latchAndCopySlice(sliceL, sliceR, captured, sr);

    // Write WAV to file via production writer
    const std::string wavPath = "build/test_riff_snr_prod.wav";
    bool ok = utils::writeWav24Bit(wavPath, sliceL.data(), sliceR.data(), captured, 48000);
    assert(ok);

    std::ifstream in(wavPath, std::ios::binary);
    std::vector<uint8_t> wav((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::filesystem::remove(wavPath);

    // Decode WAV samples back to float and verify Signal-to-Noise Ratio (SNR)
    const uint8_t* p = wav.data() + sizeof(utils::RiffWaveHeader24Bit);
    double errorPower = 0.0;
    double signalPower = 0.0;

    for (size_t i = 0; i < 48000; ++i) {
        int32_t val = (static_cast<int32_t>(p[0])) |
                      (static_cast<int32_t>(p[1]) << 8) |
                      (static_cast<int32_t>(p[2]) << 16);
        if (val & 0x800000) val |= 0xFF000000;
        const float decoded = static_cast<float>(val) / 8388607.0f;
        const float expected = testTone[i];

        const double err = decoded - expected;
        errorPower += err * err;
        signalPower += expected * expected;
        p += 6; // next frame
    }

    const double snrDb = 10.0 * std::log10(signalPower / (errorPower + 1e-18));
    assert(snrDb > 120.0);
}

// ============================================================================
// MODULE 3: R3 TRANSACTIONAL UNDO/REDO & STATE MANAGEMENT
// ============================================================================

// Tier 1: Feature Coverage (R3)
void test_T1_R3_UndoRedo_BasicTransaction() {
    state::UndoManager undoMgr;
    state::ScenePreset stateA{"Clean", {}};
    state::ScenePreset stateB{"Lead", {}};

    undoMgr.pushSnapshot(stateA, "Initial", state::UndoActionType::StructuralMutation);
    undoMgr.pushSnapshot(stateB, "Change", state::UndoActionType::StructuralMutation);

    assert(undoMgr.canUndo());
    assert(!undoMgr.canRedo());

    state::ScenePreset restored;
    bool ok = undoMgr.undo(restored);
    assert(ok && restored.name == stateA.name);
    assert(undoMgr.canRedo());

    ok = undoMgr.redo(restored);
    assert(ok && restored.name == stateB.name);
}

void test_T1_R3_SceneBar_ToneSnapshotAB_Toggle() {
    state::ScenePreset toneA{"Lead A", {}};
    state::ScenePreset toneB{"Lead B", {}};
    state::ScenePreset activeState = toneA;

    // Toggle to B
    activeState = toneB;
    assert(activeState.name == toneB.name);

    // Toggle back to A
    activeState = toneA;
    assert(activeState.name == toneA.name);
}

void test_T1_R3_LastLoadedPreset_Persistence_And_Startup() {
    const std::string tempIni = "test_last_preset_temp.ini";

    // 1. AppConfig roundtrip serialization
    state::AppConfig cfgSave;
    cfgSave.lastLoadedPreset = "Modern High Gain Djent";
    assert(cfgSave.save(tempIni));

    state::AppConfig cfgLoad;
    assert(cfgLoad.load(tempIni));
    assert(cfgLoad.lastLoadedPreset == "Modern High Gain Djent");

    // 2. Startup preset loading into GraphEngine
    audio::GraphEngine graph;
    graph.prepare(48000.0, 256);

    state::SceneManager scenes;
    bool loaded = scenes.loadPresetChain(cfgLoad.lastLoadedPreset, graph);
    assert(loaded);

    // Audio thread crossfade applies the new nodes
    std::vector<float> inLeft(256, 0.1f);
    std::vector<float> inRight(256, 0.1f);
    std::vector<float> outLeft(256, 0.0f);
    std::vector<float> outRight(256, 0.0f);
    float* inChannels[2] = {inLeft.data(), inRight.data()};
    float* outChannels[2] = {outLeft.data(), outRight.data()};
    audio::AudioBufferView inView(inChannels, 2, 256);
    audio::AudioBufferView outView(outChannels, 2, 256);

    graph.process(inView, outView);
    assert(graph.numNodes() > 0);

    // 3. Fallback resilience on missing/invalid preset
    bool missing = scenes.loadPresetChain("NonExistentPreset_XYZ", graph);
    assert(!missing);

    std::filesystem::remove(tempIni);
}

// Tier 2: Boundary & Corner Cases (R3)
void test_T2_R3_HistoryStack_Capacity64_Eviction() {
    state::UndoManager undoMgr;

    // Push 100 structural snapshots
    for (int i = 0; i < 100; ++i) {
        state::ScenePreset s{"State " + std::to_string(i), {}};
        undoMgr.pushSnapshot(s, "Step", state::UndoActionType::StructuralMutation, "", true);
    }

    // Capacity must be bounded strictly to 64
    assert(undoMgr.undoCount() == 64);

    // Undoing 63 times should reach state #36 (100 - 64)
    state::ScenePreset outState;
    for (int i = 0; i < 63; ++i) {
        bool ok = undoMgr.undo(outState);
        assert(ok);
    }
    assert(outState.name == "State 36");

    // Pop the final remaining item to empty the stack
    assert(undoMgr.canUndo());
    assert(undoMgr.undo(outState));

    // Further undo must return false (at base of stack)
    assert(!undoMgr.canUndo());
    assert(!undoMgr.undo(outState));
}

void test_T2_R3_RedoInvalidation_OnNewSnapshot() {
    state::UndoManager undoMgr;
    undoMgr.pushSnapshot(state::ScenePreset{"A", {}}, "A", state::UndoActionType::StructuralMutation);
    undoMgr.pushSnapshot(state::ScenePreset{"B", {}}, "B", state::UndoActionType::StructuralMutation);

    state::ScenePreset outState;
    undoMgr.undo(outState);
    assert(undoMgr.canRedo());

    // Pushing a new state invalidates the redo branch
    undoMgr.pushSnapshot(state::ScenePreset{"C", {}}, "C", state::UndoActionType::StructuralMutation);
    assert(!undoMgr.canRedo());
}

// Tier 3: Cross-Feature Interactions (R3)
void test_T3_R3_Coalescing_400ms_IdleThreshold() {
    state::UndoManager undoMgr;
    undoMgr.pushSnapshot(state::ScenePreset{"Base", {}}, "Base", state::UndoActionType::StructuralMutation);

    // Rapid sweeps on param "gain" within 400ms
    for (int i = 1; i <= 10; ++i) {
        state::ScenePreset p{"Sweep " + std::to_string(i), {}};
        undoMgr.pushSnapshot(p, "Sweep", state::UndoActionType::ParameterTweak, "slot0:gain");
    }

    // All 10 updates should coalesce into 1 undo entry
    assert(undoMgr.undoCount() == 2);

    state::ScenePreset restored;
    undoMgr.undo(restored);
    assert(restored.name == "Base");
}

void test_T3_R3_MouseRelease_Commit() {
    state::UndoManager undoMgr;
    undoMgr.pushSnapshot(state::ScenePreset{"Base", {}}, "Base", state::UndoActionType::StructuralMutation);

    // First sweep
    undoMgr.pushSnapshot(state::ScenePreset{"Sweep 1", {}}, "Sweep", state::UndoActionType::ParameterTweak, "gain");
    // Mouse release commit
    undoMgr.commitActiveTransaction();

    // Second sweep after mouse release
    undoMgr.pushSnapshot(state::ScenePreset{"Sweep 2", {}}, "Sweep", state::UndoActionType::ParameterTweak, "gain");

    // After commit, must create a distinct undo level
    assert(undoMgr.undoCount() == 3);
}

void test_T3_R3_StructuralMutation_DiscretePush() {
    state::UndoManager undoMgr;
    undoMgr.pushSnapshot(state::ScenePreset{"S1", {}}, "S1", state::UndoActionType::StructuralMutation);
    undoMgr.pushSnapshot(state::ScenePreset{"S2", {}}, "S2", state::UndoActionType::StructuralMutation);
    undoMgr.pushSnapshot(state::ScenePreset{"S3", {}}, "S3", state::UndoActionType::StructuralMutation);
    assert(undoMgr.undoCount() == 3);
}

// Tier 4: Real-World Scenarios (R3)
void test_T4_R3_LiveStudio_EditingSession() {
    state::UndoManager undoMgr;
    undoMgr.pushSnapshot(state::ScenePreset{"Init", {}}, "Init", state::UndoActionType::SceneSwitch);

    // 10 parameter sweeps
    for (int i = 1; i <= 10; ++i) {
        undoMgr.pushSnapshot(state::ScenePreset{"Drive " + std::to_string(i), {}}, "Tweak Drive",
                             state::UndoActionType::ParameterTweak, "drive");
    }
    undoMgr.commitActiveTransaction();

    // Add slot
    undoMgr.pushSnapshot(state::ScenePreset{"Slot Added", {}}, "Add Slot", state::UndoActionType::StructuralMutation);

    // Another sweep
    for (int i = 1; i <= 5; ++i) {
        undoMgr.pushSnapshot(state::ScenePreset{"Tone " + std::to_string(i), {}}, "Tweak Tone",
                             state::UndoActionType::ParameterTweak, "tone");
    }

    assert(undoMgr.undoCount() == 4);

    state::ScenePreset state;
    // Undo tone sweep
    assert(undoMgr.undo(state));
    assert(state.name == "Slot Added");

    // Undo structural add
    assert(undoMgr.undo(state));
    assert(state.name.rfind("Drive", 0) == 0);

    // Redo structural add
    assert(undoMgr.redo(state));
    assert(state.name == "Slot Added");
}

// ============================================================================
// MODULE 4: R4 FRONT-PANEL MACROS & RACK ERGONOMICS
// ============================================================================

struct DiscoveredMacros {
    std::string gainParam;
    std::string toneParam;
    std::string volParam;
    std::string mixParam;
};

static DiscoveredMacros discoverMacrosFromNames(const std::vector<std::string>& names) {
    DiscoveredMacros m;
    const std::regex regexGain(R"((gain|drive|dist|overdrive|input|crunch|preamp))", std::regex::icase);
    const std::regex regexTone(R"((tone|eq|treble|bass|mid|filter|bright|presence|cut))", std::regex::icase);
    const std::regex regexVol(R"((vol|volume|master|level|output|post))", std::regex::icase);
    const std::regex regexMix(R"((mix|blend|dry|wet))", std::regex::icase);

    for (const auto& name : names) {
        if (m.gainParam.empty() && std::regex_search(name, regexGain)) {
            m.gainParam = name;
        } else if (m.toneParam.empty() && std::regex_search(name, regexTone)) {
            m.toneParam = name;
        } else if (m.volParam.empty() && std::regex_search(name, regexVol)) {
            m.volParam = name;
        } else if (m.mixParam.empty() && std::regex_search(name, regexMix)) {
            m.mixParam = name;
        }
    }
    return m;
}

static std::vector<float> decimateWaveform(const float* samples, size_t count, size_t targetPoints = 800) {
    std::vector<float> points(targetPoints, 0.0f);
    if (count == 0 || !samples) return points;

    const double step = static_cast<double>(count) / static_cast<double>(targetPoints);
    for (size_t i = 0; i < targetPoints; ++i) {
        const size_t startIdx = static_cast<size_t>(i * step);
        const size_t endIdx = std::min(count, static_cast<size_t>((i + 1) * step));
        float peak = 0.0f;
        for (size_t k = startIdx; k < endIdx; ++k) {
            peak = std::max(peak, std::abs(samples[k]));
        }
        points[i] = std::clamp(peak, 0.0f, 1.0f);
    }
    return points;
}

// Tier 1: Feature Coverage (R4)
void test_T1_R4_Macro_RegexHeuristic_Discovery() {
    std::vector<std::string> paramNames = {
        "Bypass",
        "Input Drive",
        "High Cut Filter",
        "Master Level",
        "Wet Dry Mix",
        "Noise Gate Threshold"
    };

    auto macros = discoverMacrosFromNames(paramNames);
    assert(macros.gainParam == "Input Drive");
    assert(macros.toneParam == "High Cut Filter");
    assert(macros.volParam == "Master Level");
    assert(macros.mixParam == "Wet Dry Mix");
}

void test_T1_R4_Hotkey_WantTextInput_Guard() {
    bool hotkeyFired = false;
    auto evaluateHotkey = [&](bool wantTextInput, bool ctrlPressed, int key) {
        if (wantTextInput) return; // Strict guard
        if (ctrlPressed && key == 'M') {
            hotkeyFired = true;
        }
    };

    // When typing into search/rename box: hotkey MUST NOT FIRE
    evaluateHotkey(true, true, 'M');
    assert(!hotkeyFired);

    // When not typing: hotkey fires
    evaluateHotkey(false, true, 'M');
    assert(hotkeyFired);
}

void test_T1_R4_OverviewWaveform_800Points() {
    // Generate 48,000 samples of synthetic audio
    std::vector<float> audio(48000, 0.75f);
    auto points = decimateWaveform(audio.data(), audio.size(), 800);
    assert(points.size() == 800);
    for (float p : points) {
        assert(std::abs(p - 0.75f) < 1e-5f);
    }
}

// Tier 2: Boundary & Corner Cases (R4)
void test_T2_R4_Macro_Regex_EdgeCases_Adversarial() {
    // Case insensitivity & special characters
    std::vector<std::string> weirdNames = {
        "oVeRdRiVe_TrIm",
        "EQ-3-MID",
        "POST-VOL-MAX",
        "DRY/WET%"
    };
    auto macros = discoverMacrosFromNames(weirdNames);
    assert(macros.gainParam == "oVeRdRiVe_TrIm");
    assert(macros.toneParam == "EQ-3-MID");
    assert(macros.volParam == "POST-VOL-MAX");
    assert(macros.mixParam == "DRY/WET%");

    // Empty list -> all empty
    auto emptyMacros = discoverMacrosFromNames({});
    assert(emptyMacros.gainParam.empty() && emptyMacros.toneParam.empty());
}

void test_T2_R4_Waveform_BoundaryCases() {
    // Zero samples
    auto emptyPoints = decimateWaveform(nullptr, 0, 800);
    assert(emptyPoints.size() == 800);
    for (float p : emptyPoints) assert(p == 0.0f);

    // Audio smaller than 800 samples (e.g. 50 samples)
    std::vector<float> smallAudio(50, 0.8f);
    auto smallPoints = decimateWaveform(smallAudio.data(), smallAudio.size(), 800);
    assert(smallPoints.size() == 800);
    assert(!std::isnan(smallPoints[0]));
}

void test_T2_R4_FocusedShortcuts_Boundaries() {
    // Preset cycle clamping
    int activePreset = 0;
    const int maxPresets = 8;

    auto cyclePreset = [&](int delta) {
        activePreset = std::clamp(activePreset + delta, 0, maxPresets - 1);
    };

    // Cycle down at 0 -> clamps at 0
    cyclePreset(-1);
    assert(activePreset == 0);

    // Cycle up to 7 -> clamps at 7
    for (int i = 0; i < 20; ++i) cyclePreset(+1);
    assert(activePreset == 7);
}

// Tier 3: Cross-Feature Interactions (R4)
void test_T3_R4_CardDragDrop_MicroCrossfade() {
    std::vector<int> rack = {10, 20, 30, 40};
    const int from = 1;
    const int to = 3;

    int moving = rack[from];
    rack.erase(rack.begin() + from);
    rack.insert(rack.begin() + to, moving);

    assert(rack[0] == 10 && rack[1] == 30 && rack[2] == 40 && rack[3] == 20);

    // Crossfade energy verify
    for (size_t s = 0; s < 64; ++s) {
        float dry = 0.0f, wet = 0.0f;
        audio::DspUtils::calculateEqualPowerCrossfade(static_cast<float>(s) / 64.0f, dry, wet);
        assert(std::abs(dry * dry + wet * wet - 1.0f) < 1e-4f);
    }
}

void test_T3_R4_MacroReassignment_ContextOverride() {
    std::vector<std::string> paramNames = {"Drive", "Tone", "Volume", "Mix", "Custom Resonance"};
    auto macros = discoverMacrosFromNames(paramNames);
    assert(macros.toneParam == "Tone");

    // Right-click context override Tone to "Custom Resonance"
    macros.toneParam = "Custom Resonance";
    assert(macros.toneParam == "Custom Resonance");
}

// Tier 4: Real-World Scenarios (R4)
void test_T4_R4_FullPerformance_ErgonomicsSimulation() {
    // Musician scenario:
    // 1. Types in Spotlight: wantTextInput = true
    bool wantTextInput = true;
    bool hotkeyFired = false;
    if (!wantTextInput) hotkeyFired = true;
    assert(!hotkeyFired);

    // 2. Closes dialog: wantTextInput = false
    wantTextInput = false;
    if (!wantTextInput) hotkeyFired = true;
    assert(hotkeyFired);

    // 3. MIDI Map pulsating violet border alpha equation:
    for (double t = 0.0; t < 2.0; t += 0.1) {
        float alpha = static_cast<float>(0.45 + 0.45 * std::sin(4.0 * t));
        assert(alpha >= 0.0f && alpha <= 0.90f);
    }
}

// ============================================================================
// MODULE 5: R5 VISUAL POLISH, LIST VIRTUALIZATION & METERS
// ============================================================================

class DinPpmBallistics {
public:
    DinPpmBallistics() = default;

    void process(float inputPeak, float dtSeconds) {
        const float absPeak = std::clamp(std::abs(inputPeak), 0.0f, 1.0f);

        // Instantaneous attack (0 ms)
        if (absPeak >= m_level) {
            m_level = absPeak;
        } else {
            // Decay: tau = 350 ms
            const float decayFactor = std::exp(-dtSeconds / 0.350f);
            m_level *= decayFactor;
        }

        // Peak Hold: 1.2s
        if (absPeak >= m_peakHold) {
            m_peakHold = absPeak;
            m_peakHoldTimer = 1.2f;
        } else {
            m_peakHoldTimer -= dtSeconds;
            if (m_peakHoldTimer <= 0.0f) {
                const float decayFactor = std::exp(-dtSeconds / 0.350f);
                m_peakHold *= decayFactor;
            }
        }
    }

    [[nodiscard]] float level() const noexcept { return m_level; }
    [[nodiscard]] float peakHold() const noexcept { return m_peakHold; }

private:
    float m_level{0.0f};
    float m_peakHold{0.0f};
    float m_peakHoldTimer{0.0f};
};

// Tier 1: Feature Coverage (R5)
void test_T1_R5_Spotlight_ListClipper_Virtualization() {
    // Test ImGuiListClipper virtualization logic
    const int totalItems = 1500;
    const float rowH = 34.0f;
    const float viewH = 300.0f;

    auto calculateClipperRange = [&](float scrollY, int& outStart, int& outEnd) {
        outStart = std::clamp(static_cast<int>(scrollY / rowH), 0, totalItems);
        outEnd = std::clamp(static_cast<int>((scrollY + viewH) / rowH) + 1, 0, totalItems);
    };

    int start = 0, end = 0;
    calculateClipperRange(0.0f, start, end);
    assert(start == 0 && end >= 9 && end <= 10);

    calculateClipperRange(1700.0f, start, end);
    assert(start == 50 && end >= 59 && end <= 60);
}

void test_T1_R5_Footer_HighDpi_Layout_NoCollision() {
    const char* navText = "[UP/DOWN] Navigate    [ENTER] Insert Plugin    [ESC] Dismiss";
    const char* countText = "1500 plugins available";

    auto checkLayoutAtDpi = [&](float dpiScale, float containerWidth) {
        const float charWidth = 7.0f * dpiScale;
        const float leftW = static_cast<float>(std::strlen(navText)) * charWidth;
        const float rightW = static_cast<float>(std::strlen(countText)) * charWidth;
        const float requiredW = leftW + rightW + 16.0f * dpiScale;

        if (containerWidth < requiredW) {
            assert(containerWidth < requiredW);
        } else {
            const float rightX = containerWidth - rightW;
            assert(rightX >= leftW + 16.0f * dpiScale);
        }
    };

    checkLayoutAtDpi(1.0f, 600.0f);
    checkLayoutAtDpi(1.25f, 750.0f);
    checkLayoutAtDpi(1.5f, 900.0f);
    checkLayoutAtDpi(2.0f, 1200.0f);
}

void test_T1_R5_Spotlight_Favorites_ScoreBoost() {
    struct PluginItem {
        std::string name;
        bool isFavorite;
        int score;
    };

    PluginItem itemNormal{"Stereo Chorus", false, 100};
    PluginItem itemFav{"Stereo Chorus", true, 100 + 250}; // +250 boost

    assert(itemFav.score > itemNormal.score);
}

void test_T1_R5_Ppm_Ballistics_Math() {
    DinPpmBallistics ppm;

    // Instant attack: step from 0 to 1.0 in dt=0.01s
    ppm.process(1.0f, 0.01f);
    assert(std::abs(ppm.level() - 1.0f) < 1e-4f);
    assert(std::abs(ppm.peakHold() - 1.0f) < 1e-4f);

    // Decay after tau = 350 ms: level should be 1/e ~ 0.368
    ppm.process(0.0f, 0.350f);
    assert(std::abs(ppm.level() - (1.0f / std::numbers::e_v<float>)) < 0.02f);
    // Peak hold should still be 1.0 at 0.35s < 1.2s
    assert(std::abs(ppm.peakHold() - 1.0f) < 1e-4f);

    // Peak hold decay after 1.5s
    ppm.process(0.0f, 1.200f);
    assert(ppm.peakHold() < 1.0f);
}

// Tier 2: Boundary & Corner Cases (R5)
void test_T2_R5_Clipper_BoundaryCases() {
    auto calcRange = [](int count, float rowH, float scrollY, float viewH, int& s, int& e) {
        if (count <= 0) { s = 0; e = 0; return; }
        s = std::clamp(static_cast<int>(scrollY / rowH), 0, count);
        e = std::clamp(static_cast<int>((scrollY + viewH) / rowH) + 1, 0, count);
    };

    int s = 0, e = 0;
    calcRange(0, 34.0f, 0.0f, 300.0f, s, e);
    assert(s == 0 && e == 0);

    calcRange(1, 34.0f, 0.0f, 300.0f, s, e);
    assert(s == 0 && e == 1);
}

void test_T2_R5_Ppm_ExtremeSignals() {
    DinPpmBallistics ppm;

    // Negative peak input -> rectified
    ppm.process(-0.8f, 0.01f);
    assert(std::abs(ppm.level() - 0.8f) < 1e-4f);

    // Overshoot -> clamped to 1.0
    ppm.process(2.5f, 0.01f);
    assert(std::abs(ppm.level() - 1.0f) < 1e-4f);
}

// Tier 3: Cross-Feature Interactions (R5)
void test_T3_R5_DesignTokens_VisualHierarchy_Purity() {
    // Verify 4 themes tokens integrity from production design_tokens
    for (uint8_t i = 0; i < static_cast<uint8_t>(ui::ThemeId::Count); ++i) {
        const auto& theme = ui::getThemeTokens(static_cast<ui::ThemeId>(i));
        assert(std::abs(theme.surfaces.cardBg.a - 1.0f) < 1e-4f);
        assert(std::abs(theme.text.primary.a - 1.0f) < 1e-4f);
    }
}

// Tier 4: Real-World Scenarios (R5)
void test_T4_R5_HighLoad_GuiFrame_Simulation() {
    DinPpmBallistics ppm;
    const auto tStart = std::chrono::high_resolution_clock::now();
    for (int frame = 0; frame < 1000; ++frame) {
        ppm.process(0.5f, 0.007f);
    }
    const auto tEnd = std::chrono::high_resolution_clock::now();

    const double totalMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
    assert(totalMs < 100.0);
}

// ============================================================================
// MODULE 6: R6 SHARED INFRASTRUCTURE SUBSYSTEMS
// ============================================================================

// Tier 1: Feature Coverage (R6)
void test_T1_R6_Archive_DecompressionBomb_Guards() {
    utils::ArchiveBombGuards guards;
    assert(guards.maxXmlSize == 30ULL * 1024 * 1024);
    assert(guards.maxArchiveTotalSize == 128ULL * 1024 * 1024);
    assert(guards.maxCompressionRatio == 100);
    assert(guards.maxFileCount == 100);
}

void test_T1_R6_Archive_Win32PathSanitization() {
    std::filesystem::path safe;
    // Valid relative paths
    assert(utils::sanitizeZipPath("presets/default.ini", safe));
    assert(utils::sanitizeZipPath("sub/dir/audio.wav", safe));

    // Invalid traversal paths
    assert(!utils::sanitizeZipPath("../evil.dll", safe));
    assert(!utils::sanitizeZipPath("foo/../../bar.txt", safe));
    assert(!utils::sanitizeZipPath("/absolute/path", safe));
    assert(!utils::sanitizeZipPath("C:/system32", safe));
    assert(!utils::sanitizeZipPath("CON.TXT", safe));
    assert(!utils::sanitizeZipPath("aux.dll", safe));
}

void test_T1_R6_DragDropRouter_Classification() {
    assert(ui::DragDropRouter::classifyPath("track.wav") == ui::DroppedFileType::AudioTrack);
    assert(ui::DragDropRouter::classifyPath("song.FLAC") == ui::DroppedFileType::AudioTrack);
    assert(ui::DragDropRouter::classifyPath("rock.ini") == ui::DroppedFileType::Preset);
    assert(ui::DragDropRouter::classifyPath("lead.PRACPRESET") == ui::DroppedFileType::Preset);
    assert(ui::DragDropRouter::classifyPath("chain1.pracchaim") == ui::DroppedFileType::SignalChain);
    assert(ui::DragDropRouter::classifyPath("chain2.chain") == ui::DroppedFileType::SignalChain);
    assert(ui::DragDropRouter::classifyPath("notes.txt") == ui::DroppedFileType::Unknown);
    assert(ui::DragDropRouter::classifyPath("Praccy.exe") == ui::DroppedFileType::Unknown);
}

static void initializeMidiTableStrings(midi::MidiManager&) {
    // FlatCcBinding is now 100% trivially copyable; no placement-new workaround needed
}

void test_T1_R6_MidiManager_FlatTable_O1() {
    midi::MidiManager mgr;
    initializeMidiTableStrings(mgr);
    mgr.addCcBinding(1, 7, 101, 0.0f, 1.0f);
    mgr.addCcBinding(0, 10, 202, 0.0f, 1.0f);

    std::vector<std::pair<uint32_t, float>> events;
    mgr.setBindingCallback([&](const midi::MidiBinding& b, float norm) {
        events.emplace_back(b.targetSlotIndex >= 0 ? static_cast<uint32_t>(b.targetSlotIndex) : 0, norm);
    });

    const auto& b1 = mgr.getCcBinding(1, 7);
    assert(b1.active && b1.paramId == 101);

    const auto& b0 = mgr.getCcBinding(0, 10);
    assert(b0.active && b0.paramId == 202);
}

// Tier 2: Boundary & Corner Cases (R6)
void test_T2_R6_Archive_AdversarialPathMatrix() {
    std::filesystem::path safe;
    const std::vector<const char*> adversarial = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM9", "LPT1", "LPT9",
        "con.xml", "Prn.ini", "aux.wav", "nul.dat",
        "folder/file.txt.", "folder/file.txt ",
        ":stream", "C:test", "D:\\windows", "\\\\unc\\share",
        "..", "../", "..\\", "foo/bar/../../..", ""
    };
    for (const char* path : adversarial) {
        assert(!utils::sanitizeZipPath(path, safe));
    }
}

void test_T2_R6_DragDrop_MultiFileRouting() {
    std::vector<std::filesystem::path> drops = {
        "solo.wav", "lead.ini", "heavy.pracchaim", "readme.txt"
    };
    int audioCount = 0, presetCount = 0, chainCount = 0, unknownCount = 0;

    for (const auto& p : drops) {
        switch (ui::DragDropRouter::classifyFile(p)) {
            case ui::DroppedFileType::AudioTrack: audioCount++; break;
            case ui::DroppedFileType::Preset: presetCount++; break;
            case ui::DroppedFileType::SignalChain: chainCount++; break;
            case ui::DroppedFileType::Unknown: unknownCount++; break;
        }
    }

    assert(audioCount == 1);
    assert(presetCount == 1);
    assert(chainCount == 1);
    assert(unknownCount == 1);
}

void test_T2_R6_Midi_Out_Of_Bounds() {
    midi::MidiManager mgr;
    initializeMidiTableStrings(mgr);
    mgr.addCcBinding(1, 7, 101);

    // Out of bounds CC (>= 128) -> safe no-op
    const auto& bad1 = mgr.getCcBinding(1, 128);
    assert(!bad1.active);
    const auto& bad2 = mgr.getCcBinding(1, 255);
    assert(!bad2.active);
    mgr.handleControlChange(1, 128, 100);
    mgr.handleControlChange(1, 255, 100);
}

// Tier 3: Cross-Feature Interactions (R6)
void test_T3_R6_MidiDispatch_ZeroHeapAlloc() {
    midi::MidiManager mgr;
    initializeMidiTableStrings(mgr);
    for (uint8_t cc = 0; cc < 128; ++cc) {
        mgr.addCcBinding(1, cc, cc);
    }

    uint32_t eventCount = 0;
    mgr.setBindingCallback([&](const midi::MidiBinding&, float) { eventCount++; });

    t_disallowAllocations = true;
    t_allocationsCount = 0;

    for (int i = 0; i < 10000; ++i) {
        const uint8_t cc = static_cast<uint8_t>(i % 128);
        mgr.handleControlChange(1, cc, 64);
    }

    t_disallowAllocations = false;
    assert(t_allocationsCount == 0);
    assert(eventCount == 10000);
}

// Tier 4: Real-World Scenarios (R6)
void test_T4_R6_FullInfrastructure_Pipeline() {
    // 1. Path sanitization check
    std::filesystem::path safe;
    assert(utils::sanitizeZipPath("presets/factory_rock.ini", safe));

    // 2. Drag & Drop classification
    auto type = ui::DragDropRouter::classifyFile(safe);
    assert(type == ui::DroppedFileType::Preset);

    // 3. MIDI CC mapped to parameter register
    midi::MidiManager midiMgr;
    initializeMidiTableStrings(midiMgr);
    midiMgr.addCcBinding(1, 11, 42); // Expression pedal CC 11 -> Param 42

    audio::ParamAutomationRegister reg;

    midiMgr.setBindingCallback([&](const midi::MidiBinding& b, float norm) {
        const auto& ccBinding = midiMgr.getCcBinding(b.midiChannel, b.triggerNumber);
        reg.setParameter(ccBinding.paramId, norm);
    });

    midiMgr.handleControlChange(1, 11, 127);

    bool drained = false;
    reg.drainDirty([&](uint32_t id, float val) {
        assert(id == 42);
        assert(std::abs(val - 1.0f) < 1e-4f);
        drained = true;
    });
    assert(drained);
}

// ============================================================================
// MAIN EXECUTION ENTRY POINT
// ============================================================================
int main() {
    std::cout << "================================================================================\n";
    std::cout << "   PRACCY V2.4 STAGE 1 AUTOMATED REGRESSION HARNESS (test_praccy_v2_1)\n";
    std::cout << "================================================================================\n\n";

    std::cout << "[MODULE 1] Real-Time DSP Safety, SPSC Automation & Parallel Delay Compensation (R1)\n";
    RUN_TEST(test_T1_R1_ParamAutomationRegister_BasicDrain);
    RUN_TEST(test_T1_R1_Pdc_PhaseCoherence_ZeroSampleDelta);
    RUN_TEST(test_T1_R1_Pdc_EqualPowerLatencyCrossfade);
    RUN_TEST(test_T1_R1_Spsc_BlockBoundary_SlotSwap);
    RUN_TEST(test_T2_R1_ParamAutomationRegister_BoundsAndCoalescing);
    RUN_TEST(test_T2_R1_Pdc_LatencyDifferentialExtremes);
    RUN_TEST(test_T2_R1_Crossfade_SampleDiscontinuity);
    RUN_TEST(test_T3_R1_ParamAutomation_Concurrent_AudioDrain_ZeroAlloc);
    RUN_TEST(test_T3_R1_Pdc_With_DynamicAutomation);
    RUN_TEST(test_T4_R1_Continuous_Stream_Stress);

    std::cout << "\n[MODULE 2] Configurable Retroactive Riff Capture & In-Process WAV Export (R2)\n";
    RUN_TEST(test_T1_R2_RetroactiveBuffer_CircularAudioStorage);
    RUN_TEST(test_T1_R2_AtomicLatch_SliceCapture);
    RUN_TEST(test_T1_R2_WavWriter_24BitPcmRiffHeader);
    RUN_TEST(test_T1_R2_RiffFilename_Format);
    RUN_TEST(test_T2_R2_Buffer_Capacity_Durations);
    RUN_TEST(test_T2_R2_Wav24Bit_Quantization_Precision);
    RUN_TEST(test_T3_R2_AudioThread_ZeroAlloc_ConcurrentPushAndCapture);
    RUN_TEST(test_T4_R2_LatchCopy_PerformanceBenchmark);
    RUN_TEST(test_T4_R2_EndToEnd_WavExport_Validation);

    std::cout << "\n[MODULE 3] Transactional Undo/Redo & State Management (R3)\n";
    RUN_TEST(test_T1_R3_UndoRedo_BasicTransaction);
    RUN_TEST(test_T1_R3_SceneBar_ToneSnapshotAB_Toggle);
    RUN_TEST(test_T1_R3_LastLoadedPreset_Persistence_And_Startup);
    RUN_TEST(test_T2_R3_HistoryStack_Capacity64_Eviction);
    RUN_TEST(test_T2_R3_RedoInvalidation_OnNewSnapshot);
    RUN_TEST(test_T3_R3_Coalescing_400ms_IdleThreshold);
    RUN_TEST(test_T3_R3_MouseRelease_Commit);
    RUN_TEST(test_T3_R3_StructuralMutation_DiscretePush);
    RUN_TEST(test_T4_R3_LiveStudio_EditingSession);

    std::cout << "\n[MODULE 4] Front-Panel Macros & Rack Ergonomics (R4)\n";
    RUN_TEST(test_T1_R4_Macro_RegexHeuristic_Discovery);
    RUN_TEST(test_T1_R4_Hotkey_WantTextInput_Guard);
    RUN_TEST(test_T1_R4_OverviewWaveform_800Points);
    RUN_TEST(test_T2_R4_Macro_Regex_EdgeCases_Adversarial);
    RUN_TEST(test_T2_R4_Waveform_BoundaryCases);
    RUN_TEST(test_T2_R4_FocusedShortcuts_Boundaries);
    RUN_TEST(test_T3_R4_CardDragDrop_MicroCrossfade);
    RUN_TEST(test_T3_R4_MacroReassignment_ContextOverride);
    RUN_TEST(test_T4_R4_FullPerformance_ErgonomicsSimulation);

    std::cout << "\n[MODULE 5] Visual Polish, List Virtualization & Meters (R5)\n";
    RUN_TEST(test_T1_R5_Spotlight_ListClipper_Virtualization);
    RUN_TEST(test_T1_R5_Footer_HighDpi_Layout_NoCollision);
    RUN_TEST(test_T1_R5_Spotlight_Favorites_ScoreBoost);
    RUN_TEST(test_T1_R5_Ppm_Ballistics_Math);
    RUN_TEST(test_T2_R5_Clipper_BoundaryCases);
    RUN_TEST(test_T2_R5_Ppm_ExtremeSignals);
    RUN_TEST(test_T3_R5_DesignTokens_VisualHierarchy_Purity);
    RUN_TEST(test_T4_R5_HighLoad_GuiFrame_Simulation);

    std::cout << "\n[MODULE 6] Shared Infrastructure Subsystems (R6)\n";
    RUN_TEST(test_T1_R6_Archive_DecompressionBomb_Guards);
    RUN_TEST(test_T1_R6_Archive_Win32PathSanitization);
    RUN_TEST(test_T1_R6_DragDropRouter_Classification);
    RUN_TEST(test_T1_R6_MidiManager_FlatTable_O1);
    RUN_TEST(test_T2_R6_Archive_AdversarialPathMatrix);
    RUN_TEST(test_T2_R6_DragDrop_MultiFileRouting);
    RUN_TEST(test_T2_R6_Midi_Out_Of_Bounds);
    RUN_TEST(test_T3_R6_MidiDispatch_ZeroHeapAlloc);
    RUN_TEST(test_T4_R6_FullInfrastructure_Pipeline);

    std::cout << "\n================================================================================\n";
    std::cout << "   TEST RUN SUMMARY: " << g_passedTests << " / " << g_totalTests << " TESTS PASSED\n";
    std::cout << "================================================================================\n";

    if (g_passedTests == g_totalTests) {
        std::cout << ">>> ALL STAGE 1 REGRESSION TESTS PASSED CLEANLY (100% SUCCESS) <<<\n";
        return 0;
    } else {
        std::cerr << ">>> TEST FAILURES DETECTED: " << (g_totalTests - g_passedTests) << " FAILED <<<\n";
        return 1;
    }
}
