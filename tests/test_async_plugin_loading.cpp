#define NOMINMAX
#include <windows.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <cassert>
#include <memory>

#include "audio/audio_node.h"
#include "audio/graph_engine.h"
#include "plugins/async_plugin_loader.h"
#include "plugins/builtin_dsp.h"

// Real-time heap tracking
static thread_local bool t_isRealTimeThread = false;
static std::atomic<uint64_t> g_realTimeAllocations{0};
static std::atomic<uint64_t> g_realTimeDeallocations{0};

#if defined(__GNUC__) || defined(__clang__)
#define PRACCY_NOINLINE __attribute__((noinline))
#else
#define PRACCY_NOINLINE
#endif

PRACCY_NOINLINE void* operator new(std::size_t size) {
    if (t_isRealTimeThread) {
        g_realTimeAllocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* p = std::malloc(size ? size : 1);
    if (!p) throw std::bad_alloc();
    return p;
}

PRACCY_NOINLINE void operator delete(void* p) noexcept {
    if (t_isRealTimeThread) {
        g_realTimeDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    if (t_isRealTimeThread) {
        g_realTimeDeallocations.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(p);
}

static bool test_loading_placeholder_dry_passthrough() {
    std::cout << "[Test 1] LoadingPluginPlaceholder Dry Audio Pass-Through & RT Safety...\n";

    auto placeholder = std::make_unique<praccy::audio::LoadingPluginPlaceholder>(
        "Test Comp", "FabFilter", "VST3", "C:/Plugins/TestComp.vst3"
    );

    if (!placeholder->isLoading()) {
        std::cerr << "  FAIL: placeholder->isLoading() is false!\n";
        return false;
    }
    if (placeholder->name() != "Test Comp" || placeholder->format() != "VST3") {
        std::cerr << "  FAIL: Metadata mismatch on placeholder!\n";
        return false;
    }

    placeholder->prepare(48000.0, 256);

    std::vector<float> inL(256), inR(256), outL(256, 0.0f), outR(256, 0.0f);
    for (size_t i = 0; i < 256; ++i) {
        inL[i] = std::sin(static_cast<float>(i) * 0.05f);
        inR[i] = std::cos(static_cast<float>(i) * 0.05f);
    }

    float* inChannels[2] = { inL.data(), inR.data() };
    float* outChannels[2] = { outL.data(), outR.data() };

    praccy::audio::AudioBufferView inBuf(inChannels, 2, 256);
    praccy::audio::AudioBufferView outBuf(outChannels, 2, 256);
    praccy::audio::AudioProcessContext ctx{
        .input = inBuf,
        .output = outBuf,
        .sampleRate = 48000.0,
        .numSamples = 256
    };

    // Verify 0 real-time heap allocations
    g_realTimeAllocations.store(0);
    g_realTimeDeallocations.store(0);
    t_isRealTimeThread = true;

    placeholder->process(ctx);

    t_isRealTimeThread = false;

    if (g_realTimeAllocations.load() != 0 || g_realTimeDeallocations.load() != 0) {
        std::cerr << "  FAIL: Real-time heap allocations detected in placeholder process!\n";
        return false;
    }

    // Verify bit-exact pass-through
    for (size_t i = 0; i < 256; ++i) {
        if (outL[i] != inL[i] || outR[i] != inR[i]) {
            std::cerr << "  FAIL: Pass-through output mismatch at index " << i << "!\n";
            return false;
        }
    }

    std::cout << "  PASS: Bit-exact pass-through verified with zero RT allocations.\n";
    return true;
}

static bool test_atomic_slot_swap_under_load() {
    std::cout << "[Test 2] Atomic PluginSlot Swap Under Concurrent Audio Thread Load...\n";

    auto placeholder = std::make_unique<praccy::audio::LoadingPluginPlaceholder>(
        "Heavy Synth", "Serum", "VST3", "C:/Plugins/HeavySynth.vst3"
    );

    auto slot = std::make_unique<praccy::audio::PluginSlot>(std::move(placeholder));
    slot->prepare(48000.0, 256);

    if (!slot->isLoading()) {
        std::cerr << "  FAIL: slot->isLoading() should be true initially!\n";
        return false;
    }

    std::atomic<bool> audioRunning{true};
    std::atomic<uint64_t> framesProcessed{0};

    // Spin concurrent real-time audio thread
    std::thread audioThread([&]() {
        t_isRealTimeThread = true;

        std::vector<float> lIn(256, 0.5f), rIn(256, 0.5f);
        std::vector<float> lOut(256, 0.0f), rOut(256, 0.0f);
        float* inCh[2] = { lIn.data(), rIn.data() };
        float* outCh[2] = { lOut.data(), rOut.data() };

        praccy::audio::AudioBufferView inB(inCh, 2, 256);
        praccy::audio::AudioBufferView outB(outCh, 2, 256);
        praccy::audio::AudioProcessContext ctx{
            .input = inB,
            .output = outB,
            .sampleRate = 48000.0,
            .numSamples = 256
        };

        while (audioRunning.load(std::memory_order_relaxed)) {
            slot->process(ctx);
            framesProcessed.fetch_add(256, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }

        t_isRealTimeThread = false;
    });

    // Let audio run on placeholder for 20ms
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Reset allocation counters to verify swap itself causes 0 allocations on RT thread
    g_realTimeAllocations.store(0);

    // Prepare new loaded node on main thread
    auto loadedNode = std::make_unique<praccy::plugins::OverdriveEffect>();
    loadedNode->prepare(48000.0, 256);

    // Execute atomic swap
    std::unique_ptr<praccy::audio::AudioNode> oldNode;
    slot->replaceInnerNode(std::move(loadedNode), oldNode);

    // Verify state transition immediately on main thread
    if (slot->isLoading()) {
        std::cerr << "  FAIL: slot->isLoading() should be false after atomic swap!\n";
        audioRunning.store(false);
        audioThread.join();
        return false;
    }

    if (!oldNode) {
        std::cerr << "  FAIL: replaceInnerNode did not transfer old node out for deferred deletion!\n";
        audioRunning.store(false);
        audioThread.join();
        return false;
    }

    // Let audio run on new node for another 20ms
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    audioRunning.store(false);
    audioThread.join();

    if (g_realTimeAllocations.load() != 0) {
        std::cerr << "  FAIL: Audio thread allocated " << g_realTimeAllocations.load() << " times during/after swap!\n";
        return false;
    }

    // Reclaim oldNode safely off-audio-thread
    oldNode.reset();

    std::cout << "  PASS: Atomic swap completed smoothly (" << framesProcessed.load()
              << " frames processed, 0 RT allocations).\n";
    return true;
}

static bool test_async_plugin_loader_cancellation() {
    std::cout << "[Test 3] AsyncPluginLoader Queue, Dispatch & Slot Cancellation...\n";

    auto& loader = praccy::plugins::AsyncPluginLoader::instance();

    // Create dummy slot
    auto placeholder = std::make_unique<praccy::audio::LoadingPluginPlaceholder>(
        "Cancel Plugin", "Vendor", "VST3", "C:/Plugins/CancelPlugin.vst3"
    );
    auto slot = std::make_unique<praccy::audio::PluginSlot>(std::move(placeholder));

    std::atomic<bool> callbackFired{false};

    loader.enqueueLoad(
        slot.get(),
        "invalid://nonexistent_path_test.vst3",
        praccy::plugins::PluginType::VST3,
        "Cancel Plugin",
        "",
        {},
        48000.0,
        256,
        false
    );

    // Cancel slot immediately
    loader.cancelSlot(slot.get());

    // Wait a brief moment for worker to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Update dispatch queue
    loader.update([&](praccy::audio::PluginSlot*, std::unique_ptr<praccy::plugins::IPluginInstance>, bool) {
        callbackFired.store(true);
    });

    if (callbackFired.load()) {
        std::cerr << "  FAIL: Cancelled slot callback should not have fired!\n";
        return false;
    }

    std::cout << "  PASS: Cancellation verified; callback cleanly suppressed.\n";
    return true;
}

int main() {
    std::cout << "================================================================================\n"
              << "PRACCY ASYNC PLUGIN LOADING TEST SUITE\n"
              << "================================================================================\n";

    bool ok1 = test_loading_placeholder_dry_passthrough();
    bool ok2 = test_atomic_slot_swap_under_load();
    bool ok3 = test_async_plugin_loader_cancellation();

    std::cout << "\n================================================================================\n"
              << "SUMMARY RESULTS:\n"
              << "  [1] Loading Placeholder Dry Pass-Through: " << (ok1 ? "PASSED" : "FAILED") << "\n"
              << "  [2] Atomic Slot Swap Under RT Load:       " << (ok2 ? "PASSED" : "FAILED") << "\n"
              << "  [3] Async Loader Cancellation:            " << (ok3 ? "PASSED" : "FAILED") << "\n"
              << "================================================================================\n";

    if (ok1 && ok2 && ok3) {
        std::cout << ">> ALL ASYNC PLUGIN LOADING TESTS PASSED CLEANLY.\n";
        return 0;
    } else {
        std::cerr << ">> SOME TESTS FAILED.\n";
        return 1;
    }
}
