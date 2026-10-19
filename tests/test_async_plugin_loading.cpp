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
#include <fstream>

#include "audio/audio_node.h"
#include "audio/graph_engine.h"
#include "plugins/async_plugin_loader.h"
#include "plugins/plugin_window.h"
#include "plugins/vst3_host.h"
#include <pluginterfaces/vst/ivstmessage.h>
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

static bool test_real_vst3_async_load_and_swap() {
    std::cout << "[Test 4] Real VST3 Async Load & Swap...\n";
    std::string vst3Path = "C:/Program Files/Common Files/VST3/Archetype Gojira X.vst3";
    if (!std::filesystem::exists(vst3Path)) {
        std::cout << "  SKIPPED: Archetype Gojira X.vst3 not found.\n";
        return true;
    }

    auto placeholder = std::make_unique<praccy::audio::LoadingPluginPlaceholder>(
        "Archetype Gojira X", "Neural DSP", "VST3", vst3Path
    );
    auto slot = std::make_unique<praccy::audio::PluginSlot>(std::move(placeholder));
    slot->prepare(48000.0, 256);

    std::atomic<bool> audioRunning{true};
    std::thread audioThread([&]() {
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
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    auto& loader = praccy::plugins::AsyncPluginLoader::instance();
    loader.init(2);

    std::atomic<bool> loaded{false};
    loader.enqueueLoad(
        slot.get(),
        vst3Path,
        praccy::plugins::PluginType::VST3,
        "Archetype Gojira X",
        "Neural DSP",
        {},
        48000.0,
        256,
        false, // openGuiWhenDone = false
        [&](bool success) {
            std::cout << "  enqueueLoad callback finished with success=" << success << "\n";
            loaded.store(success);
        }
    );

    std::unique_ptr<praccy::plugins::IPluginInstance> readyInst;
    for (int i = 0; i < 200 && !readyInst; ++i) {
        loader.update([&](praccy::audio::PluginSlot* s, std::unique_ptr<praccy::plugins::IPluginInstance> inst, bool openGui) {
            std::cout << "  loader.update dispatched inst: " << (inst ? inst->name() : "null") << " at iteration " << i << "\n";
            readyInst = std::move(inst);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!readyInst) {
        std::cerr << "  FAIL: AsyncPluginLoader did not provide readyInst\n";
        audioRunning.store(false);
        audioThread.join();
        return false;
    }

    std::cout << "  Swapping slot inner node...\n";
    std::unique_ptr<praccy::audio::AudioNode> oldNode;
    slot->replaceInnerNode(std::move(readyInst), oldNode);

    std::cout << "  Opening plugin window on main thread...\n";
    auto* instPtr = dynamic_cast<praccy::plugins::IPluginInstance*>(slot->innerNode());
    bool winOk = praccy::plugins::PluginWindowManager::instance().openPluginWindow(instPtr);
    std::cout << "  openPluginWindow result: " << winOk << "\n";
    if (winOk) {
        for (int m = 0; m < 10; ++m) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        praccy::plugins::PluginWindowManager::instance().closePluginWindow(instPtr);
        std::cout << "  closePluginWindow succeeded\n";
    }

    std::cout << "  Letting audio run for 500ms...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    audioRunning.store(false);
    audioThread.join();

    if (oldNode) {
        oldNode.reset();
    }

    std::cout << "  PASS: Real VST3 loaded and processed without crash.\n";
    return true;
}

bool test_real_vst3_async_gui_opening() {
    std::cout << "\n--- Test 5: Real VST3 Non-Blocking Asynchronous GUI Opening ---\n";

    const std::string vPath = "C:\\Program Files\\Common Files\\VST3\\Archetype Gojira X.vst3";
    if (!std::filesystem::exists(vPath)) {
        std::cout << "  SKIPPED: Archetype Gojira X not installed on this machine.\n";
        return true;
    }

    auto placeholder = std::make_unique<praccy::audio::LoadingPluginPlaceholder>(
        "Archetype Gojira X", "Neural DSP", "VST3", vPath
    );
    auto slot = std::make_unique<praccy::audio::PluginSlot>(std::move(placeholder));
    slot->prepare(48000.0, 256);

    auto& loader = praccy::plugins::AsyncPluginLoader::instance();
    loader.init(2);

    std::atomic<bool> loaded{false};
    loader.enqueueLoad(
        slot.get(),
        vPath,
        praccy::plugins::PluginType::VST3,
        "Archetype Gojira X",
        "Neural DSP",
        {},
        48000.0,
        256,
        false, // openGuiWhenDone = false
        [&](bool success) {
            loaded.store(success);
        }
    );

    std::unique_ptr<praccy::plugins::IPluginInstance> readyInst;
    for (int i = 0; i < 200 && !readyInst; ++i) {
        loader.update([&](praccy::audio::PluginSlot* s, std::unique_ptr<praccy::plugins::IPluginInstance> inst, bool openGui) {
            readyInst = std::move(inst);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!readyInst) {
        std::cerr << "  FAIL: AsyncPluginLoader did not provide readyInst\n";
        return false;
    }

    std::unique_ptr<praccy::audio::AudioNode> oldNode;
    slot->replaceInnerNode(std::move(readyInst), oldNode);
    auto* instPtr = dynamic_cast<praccy::plugins::IPluginInstance*>(slot->innerNode());

    // Start background audio thread simulating real-time audio playback
    std::atomic<bool> audioRunning{true};
    std::atomic<uint64_t> audioBlocks{0};
    std::thread audioThread([&]() {
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
            audioBlocks.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::cout << "  Calling openPluginWindowAsync...\n";
    std::atomic<bool> guiFinished{false};
    std::atomic<bool> guiSuccess{false};

    auto t0 = std::chrono::steady_clock::now();
    bool accepted = praccy::plugins::PluginWindowManager::instance().openPluginWindowAsync(
        instPtr,
        [&](bool success) {
            std::cout << "  openPluginWindowAsync completion callback fired: success=" << success << "\n";
            guiSuccess.store(success);
            guiFinished.store(true);
        }
    );
    auto t1 = std::chrono::steady_clock::now();
    auto dispatchMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "  openPluginWindowAsync returned in " << dispatchMs << " ms (accepted=" << accepted << ")\n";

    if (!accepted) {
        std::cerr << "  FAIL: openPluginWindowAsync returned false\n";
        audioRunning.store(false);
        audioThread.join();
        return false;
    }

    if (dispatchMs > 100) {
        std::cerr << "  FAIL: openPluginWindowAsync blocked main thread for " << dispatchMs << " ms!\n";
        audioRunning.store(false);
        audioThread.join();
        return false;
    }

    bool isOpening = praccy::plugins::PluginWindowManager::instance().isWindowOpening(instPtr);
    std::cout << "  isWindowOpening: " << (isOpening ? "true" : "false") << "\n";
    if (!isOpening && !guiFinished.load()) {
        std::cerr << "  FAIL: isWindowOpening should be true while opening in background\n";
        audioRunning.store(false);
        audioThread.join();
        return false;
    }

    // Simulate main UI 60 FPS loop with message pumping and manager update
    std::cout << "  Simulating 60 FPS UI frame loop while GUI attaches in background...\n";
    int uiFrames = 0;
    auto loopStart = std::chrono::steady_clock::now();
    while (!guiFinished.load()) {
        // Win32 message pump
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Main thread UI update
        praccy::plugins::PluginWindowManager::instance().update();
        uiFrames++;

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - loopStart).count();
        if (elapsed > 15) {
            std::cerr << "  FAIL: Timeout waiting for async GUI to open\n";
            audioRunning.store(false);
            audioThread.join();
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
    }

    std::cout << "  GUI opened successfully after " << uiFrames << " UI frames!\n";
    bool isOpen = praccy::plugins::PluginWindowManager::instance().isWindowOpen(instPtr);
    std::cout << "  isWindowOpen: " << (isOpen ? "true" : "false") << "\n";
    if (!isOpen || !guiSuccess.load()) {
        std::cerr << "  FAIL: Window is not open or guiSuccess is false\n";
        audioRunning.store(false);
        audioThread.join();
        return false;
    }

    // Verify audio continued to process without dropping
    uint64_t blocksProcessed = audioBlocks.load();
    std::cout << "  Audio blocks processed during test: " << blocksProcessed << "\n";
    if (blocksProcessed < 20) {
        std::cerr << "  FAIL: Audio thread stalled during GUI opening!\n";
        audioRunning.store(false);
        audioThread.join();
        return false;
    }

    // Clean up
    praccy::plugins::PluginWindowManager::instance().closePluginWindow(instPtr);
    std::cout << "  closePluginWindow succeeded\n";

    audioRunning.store(false);
    audioThread.join();

    if (oldNode) {
        oldNode.reset();
    }

    std::cout << "  PASS: Asynchronous GUI opening verified non-blocking and thread-safe.\n";
    return true;
}

static bool test_cancel_all_watermark_preserves_new_tasks() {
    std::cout << "[Test 6] AsyncPluginLoader cancelAll Watermark Semantics...\n";

    auto& loader = praccy::plugins::AsyncPluginLoader::instance();

    // 1. Enqueue task to cancel
    auto oldPlaceholder = std::make_unique<praccy::audio::LoadingPluginPlaceholder>(
        "Old Plugin", "Vendor", "Builtin", "builtin://drive"
    );
    auto oldSlot = std::make_unique<praccy::audio::PluginSlot>(std::move(oldPlaceholder));

    loader.enqueueLoad(
        oldSlot.get(),
        "builtin://drive",
        praccy::plugins::PluginType::BuiltIn,
        "Overdrive",
        "",
        {},
        48000.0,
        256,
        false
    );

    // 2. Call cancelAll() simulating preset change
    loader.cancelAll();

    // 3. Immediately enqueue a new task (like createSlotFromPreset does)
    auto newPlaceholder = std::make_unique<praccy::audio::LoadingPluginPlaceholder>(
        "New Plugin", "Vendor", "Builtin", "builtin://drive"
    );
    auto newSlot = std::make_unique<praccy::audio::PluginSlot>(std::move(newPlaceholder));

    std::atomic<bool> newSlotReady{false};
    loader.enqueueLoad(
        newSlot.get(),
        "builtin://drive",
        praccy::plugins::PluginType::BuiltIn,
        "Overdrive",
        "",
        {},
        48000.0,
        256,
        false,
        [&](bool success) {
            newSlotReady.store(success);
        }
    );

    // Wait up to 500ms for loader to process
    for (int i = 0; i < 50; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        loader.update([&](praccy::audio::PluginSlot* s, std::unique_ptr<praccy::plugins::IPluginInstance> inst, bool) {
            std::unique_ptr<praccy::audio::AudioNode> oldNode;
            s->replaceInnerNode(std::move(inst), oldNode);
        });
        if (newSlotReady.load() && !newSlot->isLoading()) {
            break;
        }
    }

    if (!newSlotReady.load() || newSlot->isLoading()) {
        std::cerr << "  FAIL: Task enqueued immediately after cancelAll was erroneously cancelled!\n";
        return false;
    }

    std::cout << "  PASS: Tasks enqueued after cancelAll complete and swap correctly.\n";
    return true;
}

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::cout << "================================================================================\n"
              << "PRACCY ASYNC PLUGIN LOADING TEST SUITE\n"
              << "================================================================================\n";

    std::cout << "Running Test 1...\n";
    bool ok1 = test_loading_placeholder_dry_passthrough();
    std::cout << "Running Test 2...\n";
    bool ok2 = test_atomic_slot_swap_under_load();
    std::cout << "Running Test 3...\n";
    bool ok3 = test_async_plugin_loader_cancellation();
    std::cout << "Running Test 4...\n";
    bool ok4 = test_real_vst3_async_load_and_swap();
    std::cout << "Running Test 5...\n";
    bool ok5 = test_real_vst3_async_gui_opening();
    std::cout << "Running Test 6...\n";
    bool ok6 = test_cancel_all_watermark_preserves_new_tasks();

    std::cout << "\n================================================================================\n"
              << "SUMMARY RESULTS:\n"
              << "  [1] Loading Placeholder Dry Pass-Through: " << (ok1 ? "PASSED" : "FAILED") << "\n"
              << "  [2] Atomic Slot Swap Under RT Load:       " << (ok2 ? "PASSED" : "FAILED") << "\n"
              << "  [3] Async Loader Cancellation:            " << (ok3 ? "PASSED" : "FAILED") << "\n"
              << "  [4] Real VST3 Async Load & Swap:          " << (ok4 ? "PASSED" : "FAILED") << "\n"
              << "  [5] Real VST3 Async GUI Opening:          " << (ok5 ? "PASSED" : "FAILED") << "\n"
              << "  [6] cancelAll Watermark Preservation:     " << (ok6 ? "PASSED" : "FAILED") << "\n"
              << "================================================================================\n";

    if (ok1 && ok2 && ok3 && ok4 && ok5 && ok6) {
        std::cout << ">> ALL ASYNC PLUGIN LOADING TESTS PASSED CLEANLY.\n";
        return 0;
    } else {
        std::cerr << ">> SOME TESTS FAILED.\n";
        return 1;
    }
}
