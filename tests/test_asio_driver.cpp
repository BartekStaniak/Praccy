#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <cmath>
#include <numbers>
#include <random>
#include <cstring>
#include <cassert>
#include <cstdlib>

#include "audio/asio_defs.h"
#include "audio/audio_buffer.h"
#include "audio/dsp_utils.h"
#include "audio/graph_engine.h"
#include "plugins/builtin_dsp.h"
#include "tools/tuner.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

// ============================================================================
// REAL-TIME AUDIO THREAD HEAP ALLOCATION MONITORING HOOKS
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

void operator delete(void* p, std::size_t size) noexcept {
    (void)size;
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

void operator delete[](void* p, std::size_t size) noexcept {
    (void)size;
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    if (t_isAudioThread) {
        g_audioThreadAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size ? size : 1);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    if (t_isAudioThread) {
        g_audioThreadAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    return std::malloc(size ? size : 1);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    if (t_isAudioThread) {
        g_audioThreadDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace praccy;
using namespace praccy::audio;
using namespace praccy::plugins;
using namespace praccy::tools;

// ============================================================================
// HEADLESS MOCK ASIO DRIVER IMPLEMENTATION (IASIO COM INTERFACE)
// ============================================================================

class MockAsioDriver : public IASIO {
public:
    MockAsioDriver() = default;
    virtual ~MockAsioDriver() {
        disposeBuffers();
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
        (void)riid;
        if (!ppvObject) return E_POINTER;
        *ppvObject = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++m_refCount;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        return --m_refCount;
    }

    ASIOBool init(void* sysHandle) override {
        (void)sysHandle;
        m_initialized = true;
        return 1;
    }

    void getDriverName(char* name) override {
        if (name) {
            std::strncpy(name, "Praccy Headless Mock ASIO 2.0", 32);
            name[31] = '\0';
        }
    }

    int32_t getDriverVersion() override {
        return 2;
    }

    void getErrorMessage(char* string) override {
        if (string) string[0] = '\0';
    }

    ASIOError start() override {
        m_running = true;
        return ASE_OK;
    }

    ASIOError stop() override {
        m_running = false;
        return ASE_OK;
    }

    ASIOError getChannels(int32_t* numInputChannels, int32_t* numOutputChannels) override {
        if (numInputChannels) *numInputChannels = 2;
        if (numOutputChannels) *numOutputChannels = 2;
        return ASE_OK;
    }

    ASIOError getLatencies(int32_t* inputLatency, int32_t* outputLatency) override {
        if (inputLatency) *inputLatency = static_cast<int32_t>(m_bufferSize);
        if (outputLatency) *outputLatency = static_cast<int32_t>(m_bufferSize);
        return ASE_OK;
    }

    ASIOError getBufferSize(int32_t* minSize, int32_t* maxSize, int32_t* preferredSize, int32_t* granularity) override {
        if (minSize) *minSize = 64;
        if (maxSize) *maxSize = 2048;
        if (preferredSize) *preferredSize = 512;
        if (granularity) *granularity = 0;
        return ASE_OK;
    }

    ASIOError canSampleRate(double sampleRate) override {
        if (sampleRate == 44100.0 || sampleRate == 48000.0 || sampleRate == 88200.0 || sampleRate == 96000.0 || sampleRate == 192000.0) {
            return ASE_OK;
        }
        return ASE_NoClock;
    }

    ASIOError getSampleRate(double* sampleRate) override {
        if (sampleRate) *sampleRate = m_sampleRate;
        return ASE_OK;
    }

    ASIOError setSampleRate(double sampleRate) override {
        m_sampleRate = sampleRate;
        return ASE_OK;
    }

    ASIOError getClockSources(void* clocks, int32_t* numSources) override {
        (void)clocks;
        if (numSources) *numSources = 1;
        return ASE_OK;
    }

    ASIOError setClockSource(int32_t reference) override {
        (void)reference;
        return ASE_OK;
    }

    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override {
        if (sPos) {
            sPos->lo = static_cast<uint32_t>(m_samplesProcessed & 0xFFFFFFFF);
            sPos->hi = static_cast<uint32_t>((m_samplesProcessed >> 32) & 0xFFFFFFFF);
        }
        if (tStamp) {
            tStamp->lo = 0;
            tStamp->hi = 0;
        }
        return ASE_OK;
    }

    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if (!info) return ASE_InvalidParameter;
        info->type = ASIOSTFloat32LSB;
        info->isActive = 1;
        info->channelGroup = 0;
        std::snprintf(info->name, sizeof(info->name), "%s %d", info->isInput ? "In" : "Out", info->channel);
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, int32_t numChannels, int32_t bufferSize, ASIOCallbacks* callbacks) override {
        m_numChannels = static_cast<uint32_t>(numChannels);
        m_bufferSize = static_cast<uint32_t>(bufferSize);
        m_callbacks = callbacks;

        m_rawDoubleBuffers.assign(static_cast<size_t>(numChannels) * 2 * bufferSize, 0.0f);

        for (int32_t i = 0; i < numChannels; ++i) {
            bufferInfos[i].buffers[0] = &m_rawDoubleBuffers[(i * 2 + 0) * bufferSize];
            bufferInfos[i].buffers[1] = &m_rawDoubleBuffers[(i * 2 + 1) * bufferSize];
        }
        return ASE_OK;
    }

    ASIOError disposeBuffers() override {
        m_rawDoubleBuffers.clear();
        m_callbacks = nullptr;
        return ASE_OK;
    }

    ASIOError controlPanel() override {
        return ASE_OK;
    }

    ASIOError future(int32_t selector, void* opt) override {
        (void)selector;
        (void)opt;
        return ASE_SUCCESS;
    }

    ASIOError outputReady() override {
        return ASE_OK;
    }

    [[nodiscard]] bool isRunning() const noexcept { return m_running; }
    [[nodiscard]] ASIOCallbacks* callbacks() const noexcept { return m_callbacks; }
    [[nodiscard]] uint32_t bufferSize() const noexcept { return m_bufferSize; }
    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    void addProcessedSamples(uint64_t n) noexcept { m_samplesProcessed += n; }

private:
    std::atomic<ULONG> m_refCount{1};
    bool m_initialized{false};
    bool m_running{false};
    double m_sampleRate{48000.0};
    uint32_t m_bufferSize{512};
    uint32_t m_numChannels{4};
    uint64_t m_samplesProcessed{0};
    ASIOCallbacks* m_callbacks{nullptr};
    std::vector<float> m_rawDoubleBuffers;
};

// ============================================================================
// HEADLESS MOCK ASIO ENGINE
// ============================================================================

class MockAsioAudioEngine {
public:
    MockAsioAudioEngine(double sampleRate = 48000.0, uint32_t bufferSize = 512)
        : m_sampleRate(sampleRate), m_bufferSize(bufferSize) {}

    bool initialize() {
        if (m_driver.init(nullptr) != 1) return false;
        m_driver.setSampleRate(m_sampleRate);
        return true;
    }

    bool setupBuffers(ASIOCallbacks* callbacks) {
        m_bufferInfos.resize(4);
        m_bufferInfos[0].isInput = 1; m_bufferInfos[0].channelNum = 0;
        m_bufferInfos[1].isInput = 1; m_bufferInfos[1].channelNum = 1;
        m_bufferInfos[2].isInput = 0; m_bufferInfos[2].channelNum = 0;
        m_bufferInfos[3].isInput = 0; m_bufferInfos[3].channelNum = 1;

        return (m_driver.createBuffers(m_bufferInfos.data(), 4, static_cast<int32_t>(m_bufferSize), callbacks) == ASE_OK);
    }

    void start() {
        m_driver.start();
    }

    void stop() {
        m_driver.stop();
        m_driver.disposeBuffers();
    }

    [[nodiscard]] ASIOBufferInfo* bufferInfos() { return m_bufferInfos.data(); }
    [[nodiscard]] MockAsioDriver& driver() { return m_driver; }
    [[nodiscard]] uint32_t bufferSize() const { return m_bufferSize; }
    [[nodiscard]] double sampleRate() const { return m_sampleRate; }

private:
    double m_sampleRate{48000.0};
    uint32_t m_bufferSize{512};
    MockAsioDriver m_driver;
    std::vector<ASIOBufferInfo> m_bufferInfos;
};

// ============================================================================
// DSP CHAIN FACTORY (SERIAL OVERDRIVE, DELAY, AMP + PARALLEL BRANCHES)
// ============================================================================

std::vector<std::unique_ptr<AudioNode>> createDSPChain(double sampleRate, uint32_t blockSize) {
    std::vector<std::unique_ptr<AudioNode>> chain;

    // 1. Serial Overdrive Pedal
    auto drive = std::make_unique<OverdriveEffect>();
    drive->setParameterValue(0, 5.0f); // Drive
    drive->setParameterValue(1, 0.6f); // Tone
    drive->setParameterValue(2, 1.0f); // Level
    auto driveSlot = std::make_unique<PluginSlot>(std::move(drive));
    driveSlot->prepare(sampleRate, blockSize);
    chain.push_back(std::move(driveSlot));

    // 2. Parallel Split/Merge block with dual branches and plugin slots
    auto split = std::make_unique<ParallelSplitMergeBlock>("Parallel FX Bus");
    split->prepare(sampleRate, blockSize);

    auto* b1 = split->addBranch("Amp+Delay Branch");
    b1->prepare(sampleRate, blockSize);
    b1->setPan(-0.6f);
    b1->setGainDb(0.0f);
    auto b1Amp = std::make_unique<PluginSlot>(std::make_unique<TubeAmpEffect>());
    b1Amp->prepare(sampleRate, blockSize);
    b1->addSlot(std::move(b1Amp));
    auto b1Delay = std::make_unique<PluginSlot>(std::make_unique<StereoDelayEffect>());
    b1Delay->prepare(sampleRate, blockSize);
    b1->addSlot(std::move(b1Delay));

    auto* b2 = split->addBranch("Drive+Amp Branch");
    b2->prepare(sampleRate, blockSize);
    b2->setPan(+0.6f);
    b2->setGainDb(0.0f);
    auto b2Drive = std::make_unique<PluginSlot>(std::make_unique<OverdriveEffect>());
    b2Drive->prepare(sampleRate, blockSize);
    b2->addSlot(std::move(b2Drive));
    auto b2Amp = std::make_unique<PluginSlot>(std::make_unique<TubeAmpEffect>());
    b2Amp->prepare(sampleRate, blockSize);
    b2->addSlot(std::move(b2Amp));

    chain.push_back(std::move(split));

    // 3. Serial Ping-Pong Stereo Delay
    auto delay = std::make_unique<StereoDelayEffect>();
    delay->setParameterValue(0, 300.0f); // Time ms
    delay->setParameterValue(1, 0.40f);  // Feedback
    delay->setParameterValue(2, 0.30f);  // Mix
    auto delaySlot = std::make_unique<PluginSlot>(std::move(delay));
    delaySlot->prepare(sampleRate, blockSize);
    chain.push_back(std::move(delaySlot));

    // 4. Serial Tube Amp Sim & Cab Emulation
    auto amp = std::make_unique<TubeAmpEffect>();
    amp->setParameterValue(0, 4.5f); // Gain
    amp->setParameterValue(1, 0.6f); // Bass
    amp->setParameterValue(2, 0.5f); // Treble
    amp->setParameterValue(3, 0.8f); // Master
    auto ampSlot = std::make_unique<PluginSlot>(std::move(amp));
    ampSlot->prepare(sampleRate, blockSize);
    chain.push_back(std::move(ampSlot));

    return chain;
}

// ============================================================================
// STRESS TEST CONTEXT & CALLBACK DEFINITIONS
// ============================================================================

struct HarnessContext {
    GraphEngine* engine{nullptr};
    InstrumentTuner* tuner{nullptr};
    LevelMeter* meter{nullptr};
    MockAsioAudioEngine* mockEngine{nullptr};

    std::atomic<uint64_t> nanCount{0};
    std::atomic<uint64_t> infCount{0};
    std::atomic<uint64_t> clipCount{0};
    std::atomic<uint64_t> blocksProcessed{0};
};

static HarnessContext* g_ctx = nullptr;

static void mockBufferSwitch(int32_t doubleBufferIndex, ASIOBool directProcess) {
    (void)directProcess;
    if (!g_ctx) return;

    const uint32_t blockSize = g_ctx->mockEngine->bufferSize();
    ASIOBufferInfo* bufInfos = g_ctx->mockEngine->bufferInfos();

    float* inL = static_cast<float*>(bufInfos[0].buffers[doubleBufferIndex]);
    float* inR = static_cast<float*>(bufInfos[1].buffers[doubleBufferIndex]);
    float* outL = static_cast<float*>(bufInfos[2].buffers[doubleBufferIndex]);
    float* outR = static_cast<float*>(bufInfos[3].buffers[doubleBufferIndex]);

    float* inChs[2] = { inL, inR };
    float* outChs[2] = { outL, outR };
    AudioBufferView hardwareIn(inChs, 2, blockSize);
    AudioBufferView hardwareOut(outChs, 2, blockSize);

    // 1. Process audio through the complete GraphEngine
    g_ctx->engine->process(hardwareIn, hardwareOut);

    // 2. Feed output samples to decoupled InstrumentTuner
    g_ctx->tuner->pushSamples(outL, outR, blockSize);

    // 3. Track output level meter
    g_ctx->meter->process(outL, outR, blockSize);

    // 4. Real-time safety validation (Zero NaNs, Zero Infs, Zero sample clipping > 10.0f)
    for (uint32_t s = 0; s < blockSize; ++s) {
        const float valL = outL[s];
        const float valR = outR[s];

        if (std::isnan(valL) || std::isnan(valR)) {
            g_ctx->nanCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (std::isinf(valL) || std::isinf(valR)) {
            g_ctx->infCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (std::abs(valL) > 10.0f || std::abs(valR) > 10.0f) {
            g_ctx->clipCount.fetch_add(1, std::memory_order_relaxed);
        }
    }

    g_ctx->blocksProcessed.fetch_add(1, std::memory_order_relaxed);
    g_ctx->mockEngine->driver().addProcessedSamples(blockSize);
}

static void mockSampleRateDidChange(double sRate) {
    (void)sRate;
}

static int32_t mockAsioMessage(int32_t selector, int32_t value, void* message, double* opt) {
    (void)selector;
    (void)value;
    (void)message;
    (void)opt;
    return 0;
}

static ASIOTime* mockBufferSwitchTimeInfo(ASIOTime* params, int32_t doubleBufferIndex, ASIOBool directProcess) {
    mockBufferSwitch(doubleBufferIndex, directProcess);
    return params;
}

// ============================================================================
// MAIN REGRESSION HARNESS ENTRY POINT
// ============================================================================

int main() {
    std::cout << "=======================================================================\n";
    std::cout << " PRACCY V2.0 HEADLESS MOCK ASIO REGRESSION HARNESS (20,000 BLOCKS)\n";
    std::cout << "=======================================================================\n";

    constexpr uint32_t TOTAL_BLOCKS = 20000;
    constexpr uint32_t BLOCK_SIZE = 512;
    constexpr double SAMPLE_RATE = 48000.0;
    const double simulatedAudioSeconds = (static_cast<double>(TOTAL_BLOCKS) * BLOCK_SIZE) / SAMPLE_RATE;

    std::cout << "Initializing in-process Headless Mock ASIO Driver...\n";
    MockAsioAudioEngine mockEngine(SAMPLE_RATE, BLOCK_SIZE);
    if (!mockEngine.initialize()) {
        std::cerr << "ERROR: Failed to initialize MockAsioAudioEngine!\n";
        return 1;
    }

    ASIOCallbacks callbacks{};
    callbacks.bufferSwitch = &mockBufferSwitch;
    callbacks.sampleRateDidChange = &mockSampleRateDidChange;
    callbacks.asioMessage = &mockAsioMessage;
    callbacks.bufferSwitchTimeInfo = &mockBufferSwitchTimeInfo;

    if (!mockEngine.setupBuffers(&callbacks)) {
        std::cerr << "ERROR: Failed to allocate ASIO double buffers!\n";
        return 1;
    }

    std::cout << "Configuring DSP GraphEngine pipeline and decoupled InstrumentTuner...\n";
    GraphEngine engine;
    engine.prepare(SAMPLE_RATE, BLOCK_SIZE);
    engine.setInputGainDb(0.0f);
    engine.setMasterVolumeDb(0.0f);

    auto initialNodes = createDSPChain(SAMPLE_RATE, BLOCK_SIZE);
    for (auto& node : initialNodes) {
        engine.addSerialNode(std::move(node));
    }

    InstrumentTuner tuner(2048);
    tuner.prepare(SAMPLE_RATE, BLOCK_SIZE);
    tuner.start(); // Start 60Hz decoupled background worker thread

    LevelMeter harnessMeter;

    HarnessContext ctx{};
    ctx.engine = &engine;
    ctx.tuner = &tuner;
    ctx.meter = &harnessMeter;
    ctx.mockEngine = &mockEngine;
    g_ctx = &ctx;

    mockEngine.start();

    // Concurrency synchronization primitives
    std::atomic<bool> audioThreadFinished{false};
    std::atomic<uint64_t> totalBypassToggles{0};
    std::atomic<uint64_t> totalGainAdjustments{0};
    std::atomic<uint64_t> totalCrossfades{0};
    std::atomic<uint64_t> totalReclamations{0};
    std::atomic<uint64_t> totalTunerQueries{0};
    std::atomic<uint64_t> confidentTunerDetections{0};

    std::cout << "Starting multi-threaded stress test: 20,000 blocks with concurrent UI control mutations...\n";
    const auto startTime = std::chrono::high_resolution_clock::now();

    // ------------------------------------------------------------------------
    // Thread 1: Simulated Audio Callback Thread (Hardware Driver Callback)
    // ------------------------------------------------------------------------
    std::thread audioThread([&] {
        // Fast PRNG for noise bursts (LCG, strictly zero heap allocation)
        uint32_t lcg = 0x5EED1234;
        auto fastNoise = [&]() noexcept -> float {
            lcg = lcg * 1664525u + 1013904223u;
            return (static_cast<float>(lcg & 0x7FFFFFFF) / 1073741824.0f) - 1.0f; // [-1.0, 1.0]
        };

        double phase = 0.0;
        const double phaseIncrement = 2.0 * std::numbers::pi * 440.0 / SAMPLE_RATE; // 440.0 Hz A4
        const uint32_t bSize = mockEngine.bufferSize();
        ASIOBufferInfo* bufInfos = mockEngine.bufferInfos();

        // ACTIVATE REAL-TIME MEMORY MONITORING ON AUDIO THREAD
        t_isAudioThread = true;

        for (uint32_t block = 0; block < TOTAL_BLOCKS; ++block) {
            const int32_t bufIdx = static_cast<int32_t>(block & 1);
            float* inL = static_cast<float*>(bufInfos[0].buffers[bufIdx]);
            float* inR = static_cast<float*>(bufInfos[1].buffers[bufIdx]);

            // Synthesize test signal: clean 440 Hz sine wave + periodic noise bursts
            const bool isNoiseBurst = ((block % 250) < 6);
            for (uint32_t s = 0; s < bSize; ++s) {
                float sample = static_cast<float>(std::sin(phase) * 0.4);
                phase += phaseIncrement;
                if (phase >= 2.0 * std::numbers::pi) {
                    phase -= 2.0 * std::numbers::pi;
                }

                if (isNoiseBurst) {
                    sample += 0.25f * fastNoise();
                }

                inL[s] = sample;
                inR[s] = sample;
            }

            // Dispatch hardware driver callback
            mockEngine.driver().callbacks()->bufferSwitch(bufIdx, 1);
        }

        // DEACTIVATE REAL-TIME MEMORY MONITORING
        t_isAudioThread = false;

        audioThreadFinished.store(true, std::memory_order_release);
    });

    // ------------------------------------------------------------------------
    // Thread 2: Simulated UI / Control Thread (High-Frequency Concurrent Stress)
    // ------------------------------------------------------------------------
    std::thread controlThread([&] {
        std::mt19937 rng(42);
        std::uniform_int_distribution<int> actionDist(0, 4);
        std::uniform_real_distribution<float> gainDist(-12.0f, 12.0f);
        std::uniform_real_distribution<float> mixDist(0.0f, 1.0f);
        std::uniform_real_distribution<float> panDist(-1.0f, 1.0f);

        uint32_t iter = 0;
        while (!audioThreadFinished.load(std::memory_order_relaxed)) {
            iter++;

            // 1. Random bypass toggles and gain adjustments across serial and parallel blocks
            const size_t numNodes = engine.numNodes();
            if (numNodes > 0) {
                const size_t nodeIdx = rng() % numNodes;
                AudioNode* node = engine.getNode(nodeIdx);
                if (node) {
                    if (auto* slot = dynamic_cast<PluginSlot*>(node)) {
                        const int act = actionDist(rng);
                        if (act == 0 || act == 1) {
                            slot->setBypassed(!slot->isBypassed());
                            totalBypassToggles.fetch_add(1, std::memory_order_relaxed);
                        } else if (act == 2) {
                            slot->setInputGainDb(gainDist(rng));
                            totalGainAdjustments.fetch_add(1, std::memory_order_relaxed);
                        } else if (act == 3) {
                            slot->setOutputGainDb(gainDist(rng));
                            totalGainAdjustments.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            slot->setDryWet(mixDist(rng));
                            totalGainAdjustments.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (auto* splitBlock = dynamic_cast<ParallelSplitMergeBlock*>(node)) {
                        if (splitBlock->numBranches() > 0) {
                            const size_t bIdx = rng() % splitBlock->numBranches();
                            auto* branch = splitBlock->getBranch(bIdx);
                            if (branch) {
                                if (branch->numSlots() > 0) {
                                    const size_t sIdx = rng() % branch->numSlots();
                                    auto* bSlot = branch->getSlot(sIdx);
                                    if (bSlot) {
                                        bSlot->setBypassed(!bSlot->isBypassed());
                                        totalBypassToggles.fetch_add(1, std::memory_order_relaxed);
                                    }
                                }
                                branch->setGainDb(gainDist(rng));
                                branch->setPan(panDist(rng));
                                totalGainAdjustments.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }
                }
            }

            // 2. High-frequency click-free preset crossfading (crossfadeToNodes)
            if ((iter % 60 == 0) && !engine.isSceneCrossfading()) {
                engine.crossfadeToNodes(createDSPChain(SAMPLE_RATE, BLOCK_SIZE));
                totalCrossfades.fetch_add(1, std::memory_order_relaxed);
            }

            // 3. Periodic reclamation draining (collectReclaimedSlots & processReclamation)
            if (iter % 15 == 0) {
                engine.processReclamation();
                totalReclamations.fetch_add(1, std::memory_order_relaxed);
            }

            // 4. Queries decoupled tuner pitch result (currentResult)
            TunerResult res = tuner.currentResult();
            if (res.confidence) {
                confidentTunerDetections.fetch_add(1, std::memory_order_relaxed);
            }
            totalTunerQueries.fetch_add(1, std::memory_order_relaxed);

            // Yield timeslice to allow audio callback thread maximum throughput
            std::this_thread::yield();
        }
    });

    // Wait for stress testing threads to finish
    audioThread.join();
    controlThread.join();

    const auto endTime = std::chrono::high_resolution_clock::now();
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count();
    const double elapsedSec = static_cast<double>(elapsedUs) / 1000000.0;
    const double speedup = simulatedAudioSeconds / (elapsedSec > 0.0 ? elapsedSec : 0.001);

    mockEngine.stop();
    tuner.stop();

    // Query telemetry readouts
    const uint64_t blocks = ctx.blocksProcessed.load();
    const uint64_t nans = ctx.nanCount.load();
    const uint64_t infs = ctx.infCount.load();
    const uint64_t clips = ctx.clipCount.load();
    const uint64_t audioAllocs = g_audioThreadAllocations.load();
    const uint64_t audioDeallocs = g_audioThreadDeallocations.load();
    const float finalPeakL = harnessMeter.peakLeft();
    const float finalPeakR = harnessMeter.peakRight();

    // ------------------------------------------------------------------------
    // OUTPUT DETAILED EXECUTION TELEMETRY
    // ------------------------------------------------------------------------
    std::cout << "\n=======================================================================\n";
    std::cout << " REGRESSION EXECUTION TELEMETRY REPORT\n";
    std::cout << "=======================================================================\n";
    std::cout << "  Simulated ASIO Driver:          " << "Praccy Headless Mock ASIO 2.0 (In-Process)\n";
    std::cout << "  Audio blocks processed:         " << blocks << " / " << TOTAL_BLOCKS << "\n";
    std::cout << "  Block size:                     " << BLOCK_SIZE << " frames (Stereo Float32)\n";
    std::cout << "  Sample rate:                    " << static_cast<uint32_t>(SAMPLE_RATE) << " Hz\n";
    std::cout << "  Total simulated audio duration: " << std::fixed << std::setprecision(2) << simulatedAudioSeconds << " seconds (~213s of 48kHz audio)\n";
    std::cout << "  Wall-clock elapsed time:        " << std::setprecision(3) << elapsedSec << " seconds (" << elapsedUs / 1000 << " ms)\n";
    std::cout << "  Processing speedup:             " << std::setprecision(1) << speedup << "x realtime\n";
    std::cout << "  Bypass toggles performed:       " << totalBypassToggles.load() << "\n";
    std::cout << "  Parameter/gain adjustments:     " << totalGainAdjustments.load() << "\n";
    std::cout << "  Preset crossfades executed:     " << totalCrossfades.load() << "\n";
    std::cout << "  Reclamation cycles run:         " << totalReclamations.load() << "\n";
    std::cout << "  Tuner queries completed:        " << totalTunerQueries.load() << " (Confident detections: " << confidentTunerDetections.load() << ")\n";
    std::cout << "  Final output peak levels:       Left=" << finalPeakL << ", Right=" << finalPeakR << "\n";
    std::cout << "  Signal integrity checks:        Zero NaNs=" << (nans == 0 ? "YES" : "NO")
              << ", Zero Infs=" << (infs == 0 ? "YES" : "NO")
              << ", Zero Clipping (>10.0f)=" << (clips == 0 ? "YES" : "NO") << "\n";
    std::cout << "  Audio thread heap allocations:  " << audioAllocs << " (REQUIRED: EXACTLY 0)\n";
    std::cout << "  Audio thread heap deallocations:" << audioDeallocs << " (REQUIRED: EXACTLY 0)\n";
    std::cout << "=======================================================================\n";

    bool pass = true;
    if (blocks != TOTAL_BLOCKS) {
        std::cerr << "FAIL: Incorrect block count! Expected " << TOTAL_BLOCKS << ", got " << blocks << "\n";
        pass = false;
    }
    if (nans > 0) {
        std::cerr << "FAIL: NaN samples detected on audio output! Count: " << nans << "\n";
        pass = false;
    }
    if (infs > 0) {
        std::cerr << "FAIL: Inf samples detected on audio output! Count: " << infs << "\n";
        pass = false;
    }
    if (clips > 0) {
        std::cerr << "FAIL: Clipped samples (>10.0f) detected on audio output! Count: " << clips << "\n";
        pass = false;
    }
    if (audioAllocs != 0) {
        std::cerr << "FAIL: Real-time audio thread performed dynamic heap allocations! Count: " << audioAllocs << "\n";
        pass = false;
    }
    if (audioDeallocs != 0) {
        std::cerr << "FAIL: Real-time audio thread performed dynamic heap deallocations! Count: " << audioDeallocs << "\n";
        pass = false;
    }
    if (totalBypassToggles.load() == 0) {
        std::cerr << "FAIL: Zero bypass toggles were performed during stress test!\n";
        pass = false;
    }
    if (speedup < 10.0) {
        std::cerr << "FAIL: Headless processing speedup was under 10x realtime!\n";
        pass = false;
    }

    if (pass) {
        std::cout << ">>> VERDICT: ALL ACCEPTANCE CRITERIA SATISFIED! (EXIT CODE 0) <<<\n\n";
        return 0;
    } else {
        std::cerr << ">>> VERDICT: REGRESSION HARNESS FAILED! (NON-ZERO EXIT CODE) <<<\n\n";
        return 1;
    }
}
