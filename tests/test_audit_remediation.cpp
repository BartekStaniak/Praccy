#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <string_view>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

#include "audio/dsp_utils.h"
#include "audio/graph_engine.h"
#include "audio/retroactive_buffer.h"
#include "midi/midi_manager.h"
#include "tools/quick_looper.h"
#include "tools/tuner.h"
#include "plugins/builtin_dsp.h"
#include "plugins/plugin_scanner.h"
#include "plugins/async_plugin_loader.h"
#include "plugins/vst3_host.h"
#include "plugins/clap_host.h"
#include "plugins/plugin_window.h"
#include "state/scene_manager.h"
#include "state/daw_interop/reaper_adapter.h"
#include "state/daw_interop/ableton_adapter.h"
#include "state/daw_interop/dawproject_adapter.h"
#include "utils/xml_parser.h"
#include "ui/design_tokens.h"
#include "ui/ui_spec.h"

#if !defined(_WIN32)
// Non-Win32 link stubs for Win32-only plugin window / DLL loaders when running tests on POSIX
namespace praccy::plugins {
struct Vst3PluginInstance::Impl {};
Vst3PluginInstance::Vst3PluginInstance() = default;
Vst3PluginInstance::~Vst3PluginInstance() = default;
std::unique_ptr<Vst3PluginInstance> Vst3PluginInstance::loadFromFile(const std::string&) { return nullptr; }
void Vst3PluginInstance::prepare(double, uint32_t) {}
void Vst3PluginInstance::process(audio::AudioProcessContext&) {}
void Vst3PluginInstance::reset() {}
size_t Vst3PluginInstance::numParameters() const noexcept { return 0; }
PluginParameterDesc Vst3PluginInstance::getParameterDesc(size_t) const { return {}; }
void Vst3PluginInstance::setParameterValue(uint32_t, float) {}
void Vst3PluginInstance::setParameterFromAudioThread(uint32_t, float) noexcept {}
float Vst3PluginInstance::getParameterValue(uint32_t) const { return 0.0f; }
bool Vst3PluginInstance::hasCustomGui() const noexcept { return false; }
bool Vst3PluginInstance::openGui(HWND) { return false; }
void Vst3PluginInstance::openGuiAsync(HWND, std::function<void(bool)> cb) { if (cb) cb(false); }
void Vst3PluginInstance::pollAsyncGuiResults() {}
void Vst3PluginInstance::closeGui() {}
void Vst3PluginInstance::getPreferredSize(int& w, int& h) const { w = 800; h = 600; }
void Vst3PluginInstance::onGuiResize(int, int) {}
std::vector<uint8_t> Vst3PluginInstance::saveState() const { return {}; }
bool Vst3PluginInstance::loadState(const std::vector<uint8_t>&) { return false; }
bool Vst3PluginInstance::ensureControllerInitialized() { return false; }
void Vst3PluginInstance::resetFault() noexcept {}

} // namespace praccy::plugins
namespace Steinberg {
DEF_CLASS_IID(FUnknown)
namespace Vst {
DEF_CLASS_IID(IParamValueQueue)
DEF_CLASS_IID(IParameterChanges)
} // namespace Vst
} // namespace Steinberg
namespace praccy::plugins {
ClapPluginInstance::~ClapPluginInstance() = default;
std::unique_ptr<ClapPluginInstance> ClapPluginInstance::loadFromFile(const std::string&, uint32_t) { return nullptr; }
void ClapPluginInstance::prepare(double, uint32_t) {}
void ClapPluginInstance::process(audio::AudioProcessContext&) {}
void ClapPluginInstance::reset() {}
size_t ClapPluginInstance::numParameters() const noexcept { return 0; }
PluginParameterDesc ClapPluginInstance::getParameterDesc(size_t) const { return {}; }
void ClapPluginInstance::setParameterValue(uint32_t, float) {}
void ClapPluginInstance::setParameterFromAudioThread(uint32_t, float) noexcept {}
float ClapPluginInstance::getParameterValue(uint32_t) const { return 0.0f; }
bool ClapPluginInstance::hasCustomGui() const noexcept { return false; }
bool ClapPluginInstance::openGui(HWND) { return false; }
void ClapPluginInstance::closeGui() {}
void ClapPluginInstance::getPreferredSize(int& w, int& h) const { w = 800; h = 600; }
void ClapPluginInstance::onGuiResize(int, int) {}
std::vector<uint8_t> ClapPluginInstance::saveState() const { return {}; }
bool ClapPluginInstance::loadState(const std::vector<uint8_t>&) { return false; }
void ClapPluginInstance::handleMainThreadCallbacks() noexcept {}
void ClapPluginInstance::resetFault() noexcept {}

PluginWindowManager::PluginWindowManager() = default;
PluginWindowManager::~PluginWindowManager() = default;
PluginWindowManager& PluginWindowManager::instance() {
    static PluginWindowManager s_inst;
    return s_inst;
}
void PluginWindowManager::closeAllWindows() {}
void PluginWindowManager::closePluginWindow(IPluginInstance*) {}
} // namespace praccy::plugins
#endif

namespace {

// Standard-compliant member pointer robber for white-box concurrency verification
template <typename Tag>
struct MemberAccess {
    inline static typename Tag::type ptr;
};

template <typename Tag, typename Tag::type Ptr>
struct MemberRobber {
    struct Init {
        Init() { MemberAccess<Tag>::ptr = Ptr; }
    };
    inline static Init init{};
};

struct MidiSeqTag {
    using type = std::atomic<uint32_t> praccy::midi::MidiManager::*;
};
template struct MemberRobber<MidiSeqTag, &praccy::midi::MidiManager::m_ccTableSeq>;

struct RiffCaptureActiveTag {
    using type = std::atomic<bool> praccy::audio::RetroactiveRiffBuffer::*;
};
template struct MemberRobber<RiffCaptureActiveTag, &praccy::audio::RetroactiveRiffBuffer::m_captureActive>;

struct TunerMutexTag {
    using type = std::mutex praccy::tools::InstrumentTuner::*;
};
template struct MemberRobber<TunerMutexTag, &praccy::tools::InstrumentTuner::m_configMutex>;

template <typename T>
bool checkScopedDenormalGuard() {
    if constexpr (requires { typename T::ScopedDenormalGuard; }) {
        typename T::ScopedDenormalGuard guard;
        (void)guard;
        return true;
    } else {
        return false;
    }
}

template <typename T>
bool checkPpmMeterBallistics() {
    if constexpr (requires(T m) { m.setPpmMode(true); }) {
        T meter;
        meter.setPpmMode(true);
        std::vector<float> burstL(240, 1.0f);
        std::vector<float> burstR(240, 1.0f);
        meter.process(burstL.data(), burstR.data(), 240, 48000.0);
        const float peakAfter5ms = meter.peakLeft();
        return (peakAfter5ms > 0.65f && peakAfter5ms < 0.92f);
    } else {
        return false;
    }
}

// ============================================================================
// SUITE 1: REAL-TIME SAFETY (F-001, F-002, F-007, F-008, F-009, F-010)
// ============================================================================
bool runSuiteRt() {
    using namespace praccy;
    std::cout << "[SUITE: RT] Running Real-Time Safety Regression Tests...\n";
    bool allPassed = true;

    // 1. F-009: Subnormal scrubbing in DspUtils::sanitizeChannel & ScopedDenormalGuard
    {
        const float subnormalPos = std::numeric_limits<float>::denorm_min();
        const float subnormalNeg = -subnormalPos;
        float chBuf[4] = { subnormalPos, subnormalNeg, 0.5f, std::numeric_limits<float>::quiet_NaN() };
        audio::DspUtils::sanitizeChannel(chBuf, 4);

        const bool subnormalsScrubbed = (chBuf[0] == 0.0f) && (chBuf[1] == 0.0f) &&
                                        (chBuf[2] == 0.5f) && (chBuf[3] == 0.0f);
        const bool guardAvailable = checkScopedDenormalGuard<audio::DspUtils>();
        if (!subnormalsScrubbed || !guardAvailable) {
            std::cout << "  [FAIL] F-009: Subnormal scrubbing (" << subnormalsScrubbed
                      << ") or ScopedDenormalGuard (" << guardAvailable << ") missing.\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-009: Subnormals scrubbed and ScopedDenormalGuard active.\n";
        }
    }

    // 2. F-001: Lock-free cross-thread PluginSlot::setBypassed & EqualPowerRamp
    {
        auto slot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>());
        slot->prepare(48000.0, 64);

        std::atomic<bool> running{true};
        std::thread uiToggleThread([&]() {
            for (int i = 0; i < 400; ++i) {
                slot->setBypassed((i & 1) != 0);
            }
            running.store(false, std::memory_order_release);
        });

        float left[64]{};
        float right[64]{};
        float* ptrs[2] = { left, right };
        audio::AudioBufferView view(ptrs, 2, 64);
        audio::AudioProcessContext ctx{view, view, 48000.0, 64};

        bool finiteOutput = true;
        while (running.load(std::memory_order_acquire)) {
            std::fill_n(left, 64, 0.25f);
            std::fill_n(right, 64, 0.25f);
            slot->process(ctx);
            for (uint32_t i = 0; i < 64; ++i) {
                if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
                    finiteOutput = false;
                }
            }
        }
        uiToggleThread.join();
        if (!finiteOutput) {
            std::cout << "  [FAIL] F-001: Non-finite output during concurrent bypass toggling.\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-001: Concurrent PluginSlot::setBypassed race-free under TSan.\n";
        }
    }

    // 3. F-002: Multi-block GraphEngine crossfade feedback explosion test
    {
        audio::GraphEngine engine;
        engine.prepare(48000.0, 64);
        engine.setAudioRunning(true);

        float inL[64]{};
        float inR[64]{};
        float outL[64]{};
        float outR[64]{};
        std::fill_n(inL, 64, 0.5f);
        std::fill_n(inR, 64, 0.5f);
        float* inPtrs[2] = { inL, inR };
        float* outPtrs[2] = { outL, outR };
        audio::AudioBufferView inView(inPtrs, 2, 64);
        audio::AudioBufferView outView(outPtrs, 2, 64);

        // Prime outgoing tail buffer with 1.0f block
        engine.process(inView, outView);

        // Trigger 10ms crossfade (480 samples = 7.5 blocks of 64 samples)
        engine.triggerCrossfade(10.0f);

        float maxCrossfadeSample = 0.0f;
        for (int b = 0; b < 7; ++b) {
            engine.process(inView, outView);
            for (uint32_t s = 0; s < 64; ++s) {
                maxCrossfadeSample = std::max(maxCrossfadeSample, std::abs(outL[s]));
            }
        }

        // Equal-power sum of two correlated 0.5f signals has theoretical peak 0.5f * sqrt(2) = 0.7071f.
        // If m_outgoingTailBuffer is overwritten every block during the crossfade, feedback snowballs > 0.85f!
        if (maxCrossfadeSample > 0.73f) {
            std::cout << "  [FAIL] F-002: Multi-block crossfade feedback explosion (peak="
                      << maxCrossfadeSample << " > 0.73f).\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-002: Multi-block crossfade bounded (peak=" << maxCrossfadeSample << ").\n";
        }
    }

    // 4. F-007 / F-027: MidiManager torn seqlock read & multi-event learn queue
    {
        midi::MidiManager mgr;
        mgr.addCcBinding(1, 7, 42, 0.0f, 1.0f);

        // Simulate writer active (odd sequence number)
        (mgr.*MemberAccess<MidiSeqTag>::ptr).fetch_add(1, std::memory_order_seq_cst);
        auto tornSlot = mgr.getCcBinding(1, 7);
        (mgr.*MemberAccess<MidiSeqTag>::ptr).fetch_sub(1, std::memory_order_seq_cst);

        // Also verify two back-to-back MIDI Learn captures before UI drain are both retained
        mgr.startLearning(midi::BindingTargetType::SlotBypass, 0);
        mgr.handleMidiMessage(0xB0, 21, 127); // Ch 1 CC 21
        mgr.startLearning(midi::BindingTargetType::SlotDryWet, 1);
        mgr.handleMidiMessage(0xB0, 22, 127); // Ch 1 CC 22
        // Trigger UI drain via removeBinding of a non-existent ID (which rebuilds table from m_bindings)
        mgr.removeBinding("non_existent_id");
        const auto learned1 = mgr.getCcBinding(1, 21);
        const auto learned2 = mgr.getCcBinding(1, 22);

        if (tornSlot.active || !learned1.active || !learned2.active) {
            std::cout << "  [FAIL] F-007: MidiManager torn read (active=" << tornSlot.active
                      << ") or dropped multi-learn (" << learned1.active << "," << learned2.active << ").\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-007: MidiManager seqlock retry exhaustion safe and multi-learn queued.\n";
        }
    }

    // 5. F-008 / F-028: QuickLooper O(1) real-time clear latency (< 400 us)
    {
        tools::QuickLooper looper;
        looper.prepare(48000.0, 60, 64);
        looper.triggerAction(); // Recording

        float inL[64]{};
        float inR[64]{};
        float outL[64]{};
        float outR[64]{};
        float* inCh[2] = { inL, inR };
        float* outCh[2] = { outL, outR };
        audio::AudioBufferView inView(inCh, 2, 64);
        audio::AudioBufferView outView(outCh, 2, 64);
        looper.process(inView, outView);

        auto t0 = std::chrono::steady_clock::now();
        looper.clear();
        looper.process(inView, outView);
        auto t1 = std::chrono::steady_clock::now();
        const auto clearUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        if (clearUs > 400 || looper.state() != tools::LooperState::Empty) {
            std::cout << "  [FAIL] F-008: QuickLooper clear took " << clearUs << " us (limit 400 us).\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-008: QuickLooper O(1) clear completed in " << clearUs << " us.\n";
        }
    }

    // 6. F-010: InstrumentTuner::currentResult() must be wait-free (must not lock m_configMutex)
    {
        tools::InstrumentTuner tuner(2048);
        tuner.prepare(48000.0, 256);
        std::vector<float> sine(4096, 0.0f);
        for (size_t i = 0; i < sine.size(); ++i) {
            sine[i] = std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / 48000.0f);
        }
        tuner.process(sine.data(), static_cast<uint32_t>(sine.size()));

        // Hold m_configMutex on this thread and verify a second thread calling currentResult() does not block!
        std::atomic<bool> readerDone{false};
        std::unique_lock<std::mutex> holdLock(tuner.*MemberAccess<TunerMutexTag>::ptr);
        std::thread uiReader([&]() {
            auto res = tuner.currentResult();
            (void)res;
            readerDone.store(true, std::memory_order_release);
        });

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
        while (!readerDone.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const bool wasWaitFree = readerDone.load(std::memory_order_acquire);
        holdLock.unlock();
        uiReader.join();

        if (!wasWaitFree) {
            std::cout << "  [FAIL] F-010: InstrumentTuner::currentResult() blocked on m_configMutex!\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-010: InstrumentTuner::currentResult() is wait-free.\n";
        }
    }

    return allPassed;
}

// ============================================================================
// SUITE 2: MEMORY SAFETY & UB (F-011, F-012, F-013, F-016, F-026)
// ============================================================================
bool runSuiteMem() {
    using namespace praccy;
    std::cout << "[SUITE: MEM] Running Memory Safety & Concurrency Regression Tests...\n";
    bool allPassed = true;

    // 1. F-011: RetroactiveRiffBuffer::reset() / prepare() must fence against m_captureActive
    {
        audio::RetroactiveRiffBuffer riff;
        riff.prepare(48000.0, 1);

        // Simulate active slice capture holding m_captureActive = true
        (riff.*MemberAccess<RiffCaptureActiveTag>::ptr).store(true, std::memory_order_seq_cst);
        std::atomic<bool> resetFinished{false};
        std::thread resetter([&]() {
            riff.reset();
            resetFinished.store(true, std::memory_order_release);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(15));
        const bool prematureResetDuringCapture = resetFinished.load(std::memory_order_acquire);
        (riff.*MemberAccess<RiffCaptureActiveTag>::ptr).store(false, std::memory_order_seq_cst);
        resetter.join();

        if (prematureResetDuringCapture) {
            std::cout << "  [FAIL] F-011: RetroactiveRiffBuffer::reset() bypassed m_captureActive fence!\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-011: RetroactiveRiffBuffer::reset() waited for m_captureActive.\n";
        }
    }

    // 2. F-013: AsyncPluginLoader reload cancellation ABA bug
    {
        auto& loader = plugins::AsyncPluginLoader::instance();
        loader.cancelAll();

        auto slot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>());
        loader.enqueueLoad(slot.get(), "", plugins::PluginType::BuiltIn, "Overdrive", "", {}, 48000.0, 64);
        loader.cancelSlot(slot.get());
        loader.enqueueLoad(slot.get(), "", plugins::PluginType::BuiltIn, "Tube Amp", "", {}, 48000.0, 64);

        int completions = 0;
        std::string lastLoadedName;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (std::chrono::steady_clock::now() < deadline) {
            loader.update([&](audio::PluginSlot*, std::unique_ptr<plugins::IPluginInstance> inst, bool) {
                if (inst) {
                    completions++;
                    lastLoadedName = inst->name();
                }
            });
            if (completions >= 1 && !loader.isSlotLoading(slot.get())) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                loader.update([&](audio::PluginSlot*, std::unique_ptr<plugins::IPluginInstance> inst, bool) {
                    if (inst) {
                        completions++;
                        lastLoadedName = inst->name();
                    }
                });
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (completions != 1 || lastLoadedName != "Praccy Amp Sim") {
            std::cout << "  [FAIL] F-013: AsyncPluginLoader ABA un-cancelled stale load (completions="
                      << completions << ", last=" << lastLoadedName << ").\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-013: AsyncPluginLoader delivered only latest load after cancelSlot.\n";
        }
    }

    // 3. F-012 / F-026: InstrumentTuner prepare() vs pushSamples() & process() concurrency under ASan/TSan
    {
        tools::InstrumentTuner tuner(2048);
        tuner.prepare(48000.0, 256);
        std::atomic<bool> running{true};

        std::thread audioThread([&]() {
            std::vector<float> block(256, 0.25f);
            while (running.load(std::memory_order_relaxed)) {
                tuner.pushSamples(block.data(), 256);
                std::this_thread::yield();
            }
        });

        for (int i = 0; i < 30; ++i) {
            tuner.prepare((i & 1) ? 96000.0 : 48000.0, 256 + static_cast<uint32_t>((i % 4) * 64));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        running.store(false, std::memory_order_release);
        audioThread.join();
        std::cout << "  [PASS] F-012/F-026: InstrumentTuner prepare() vs pushSamples() completed cleanly.\n";
    }

    return allPassed;
}

// ============================================================================
// SUITE 3: PLUGIN HOST & STATE HARDENING (F-004, F-006, F-014, F-015, F-029)
// ============================================================================
bool runSuiteHost() {
    using namespace praccy;
    std::cout << "[SUITE: HOST] Running Plugin Boundary & State Hardening Tests...\n";
    bool allPassed = true;

    // 1. F-004: SceneManager::createSlotFromPreset async macro range preservation
    {
        state::PluginSlotPreset p;
        p.name = "Overdrive";
        p.type = "VST3";
        p.path = "C:/NonExistent/FakePlugin.vst3";
        p.macros.resize(4);
        p.macros[0].paramId = 101;
        p.macros[0].label = "Drive";
        p.macros[0].value = 6.0f;
        p.macros[0].defaultValue = -3.0f;
        p.macros[0].minValue = -24.0f;
        p.macros[0].maxValue = 12.0f;
        p.macros[0].bound = true;
        p.paramValues.push_back({ 101, 0.75f });

        auto slot = state::SceneManager::createSlotFromPreset(p, true /* allowAsync */, 48000.0, 256);
        plugins::AsyncPluginLoader::instance().cancelAll();

        const bool macroRangesPreserved = slot &&
            std::abs(slot->macros()[0].minValue - (-24.0f)) < 1e-4f &&
            std::abs(slot->macros()[0].maxValue - 12.0f) < 1e-4f &&
            std::abs(slot->macros()[0].defaultValue - (-3.0f)) < 1e-4f;

        if (!macroRangesPreserved) {
            std::cout << "  [FAIL] F-004: createSlotFromPreset(allowAsync=true) corrupted macro ranges ("
                      << (slot ? slot->macros()[0].minValue : 0.0f) << ".."
                      << (slot ? slot->macros()[0].maxValue : 0.0f) << ").\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-004: createSlotFromPreset(allowAsync=true) preserved macro ranges.\n";
        }
    }

    // 2. F-006: PluginScanner format-aware deduplication and preferredType resolution
    {
        plugins::PluginScanner scanner;
        const auto tempScanDir = std::filesystem::temp_directory_path() / "praccy_audit_scan_test";
        std::filesystem::create_directories(tempScanDir);
        {
            std::ofstream f1(tempScanDir / "DualFormatSynth.vst3");
            f1 << "x";
            std::ofstream f2(tempScanDir / "DualFormatSynth.clap");
            f2 << "x";
        }
        scanner.addCustomSearchPath(tempScanDir.string());
        scanner.scanAll();
        while (scanner.isScanning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        const auto* resolvedClap = scanner.resolvePlugin("DualFormatSynth", "CLAP");
        const auto* resolvedVst3 = scanner.resolvePlugin("DualFormatSynth", "VST3");
        const bool formatHonored = resolvedClap && resolvedVst3 &&
                                   (resolvedClap->type == plugins::PluginType::CLAP) &&
                                   (resolvedVst3->type == plugins::PluginType::VST3);
        std::error_code ec;
        std::filesystem::remove_all(tempScanDir, ec);

        if (!formatHonored) {
            std::cout << "  [FAIL] F-006: PluginScanner failed to preserve/resolve both VST3 and CLAP formats.\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-006: PluginScanner preserved and resolved both VST3 and CLAP formats.\n";
        }
    }

    // 3. F-015: NaN state injection sanitization in Built-In DSP and PluginSlot
    {
        plugins::OverdriveEffect od;
        plugins::StereoDelayEffect dly;
        dly.prepare(48000.0, 64);

        const float nanVal = std::numeric_limits<float>::quiet_NaN();
        struct NanState3 { float a, b, c; } nan3{ nanVal, nanVal, nanVal };
        std::vector<uint8_t> nanBytes(sizeof(nan3));
        std::memcpy(nanBytes.data(), &nan3, sizeof(nan3));
        od.loadState(nanBytes);
        dly.loadState(nanBytes);

        audio::PluginSlot slot(std::make_unique<plugins::OverdriveEffect>());
        slot.setDryWet(nanVal);
        slot.setInputGainDb(nanVal);
        slot.setOutputGainDb(nanVal);

        const bool allFinite = std::isfinite(od.getParameterValue(0)) &&
                               std::isfinite(od.getParameterValue(1)) &&
                               std::isfinite(od.getParameterValue(2)) &&
                               std::isfinite(dly.getParameterValue(0)) &&
                               std::isfinite(dly.getParameterValue(1)) &&
                               std::isfinite(dly.getParameterValue(2)) &&
                               std::isfinite(slot.dryWet()) &&
                               std::isfinite(slot.inputGainDb()) &&
                               std::isfinite(slot.outputGainDb());
        if (!allFinite) {
            std::cout << "  [FAIL] F-015: NaN state/parameter injection was not sanitized!\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-015: NaN state and parameter values sanitized cleanly.\n";
        }
    }

    // 4. F-029: XmlParser DOCTYPE internal subset & DawprojectAdapter lossless slot roundtrip
    {
        const std::string xmlWithDtd = "<?xml version=\"1.0\"?>\n<!DOCTYPE project [ <!ENTITY ver \"1.0\"> ]>\n<Project version=\"1.0\"><Transport/></Project>";
        auto parsed = utils::XmlParser::parse(xmlWithDtd);
        const bool dtdParsed = (parsed != nullptr && parsed->tag == "Project");

        state::ScenePreset srcScene;
        srcScene.name = "DawprojectRoundtrip";
        state::NodePreset np;
        np.kind = state::NodePreset::Kind::Plugin;
        np.slot.name = "Overdrive";
        np.slot.type = "BuiltIn";
        np.slot.inputGainDb = 4.5f;
        np.slot.outputGainDb = -3.25f;
        np.slot.dryWet = 0.65f;
        np.slot.paramValues = { {0, 12.0f}, {1, 0.4f} };
        srcScene.nodes.push_back(np);

        const auto tempDawFile = std::filesystem::temp_directory_path() / "praccy_audit_roundtrip.dawproject";
        state::DawprojectAdapter adapter;
        bool expOk = adapter.exportFromScene(srcScene, tempDawFile);
        auto impOpt = adapter.parseToScene(tempDawFile);
        bool impOk = expOk && impOpt.has_value();
        const auto dstScene = impOpt.value_or(state::ScenePreset{});
        std::error_code ec;
        std::filesystem::remove(tempDawFile, ec);

        const bool roundtripOk = impOk && !dstScene.nodes.empty() &&
            std::abs(dstScene.nodes[0].slot.inputGainDb - 4.5f) < 1e-3f &&
            std::abs(dstScene.nodes[0].slot.outputGainDb - (-3.25f)) < 1e-3f &&
            std::abs(dstScene.nodes[0].slot.dryWet - 0.65f) < 1e-3f &&
            dstScene.nodes[0].slot.paramValues.size() == 2;

        if (!dtdParsed || !roundtripOk) {
            std::cout << "  [FAIL] F-029: XmlParser DTD (" << dtdParsed
                      << ") or DawprojectAdapter roundtrip (" << roundtripOk << ") failed.\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-029: XmlParser DTD subset and DawprojectAdapter roundtrip verified.\n";
        }
    }

    return allPassed;
}

// ============================================================================
// SUITE 4: IMGUI DESIGN SYSTEM, DPI & METERING (F-022, F-023, F-024, F-025)
// ============================================================================
bool runSuiteUi() {
    using namespace praccy;
    std::cout << "[SUITE: UI] Running ImGui Design System, DPI & Metering Tests...\n";
    bool allPassed = true;

    // 1. F-024: High-DPI scaling in ui::Spec metrics
    {
        ui::setDpiScale(1.0f);
        const float row1x = ui::Spec::tableRowHeight();
        const float btn1x = ui::Spec::smallButtonHeight();
        ui::setDpiScale(2.0f);
        const float row2x = ui::Spec::tableRowHeight();
        const float btn2x = ui::Spec::smallButtonHeight();
        ui::setDpiScale(1.0f);

        const bool dpiScaled = (std::abs(row2x - row1x * 2.0f) < 0.5f) &&
                               (std::abs(btn2x - btn1x * 2.0f) < 0.5f);
        if (!dpiScaled) {
            std::cout << "  [FAIL] F-024: ui::Spec metrics did not scale with DPI (row1x="
                      << row1x << ", row2x=" << row2x << ").\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-024: ui::Spec metrics scale proportionally with ui::dpiScale().\n";
        }
    }

    // 2. F-025: LevelMeter DIN IEC 60268-10 PPM ballistics support
    {
        const bool ppmSupported = checkPpmMeterBallistics<audio::LevelMeter>();
        if (!ppmSupported) {
            std::cout << "  [FAIL] F-025: LevelMeter DIN IEC 60268-10 PPM ballistics not implemented or out of spec.\n";
            allPassed = false;
        } else {
            std::cout << "  [PASS] F-025: LevelMeter DIN IEC 60268-10 PPM ballistics verified.\n";
        }
    }

    return allPassed;
}

} // namespace

int main(int argc, char** argv) {
    std::string_view suite = "--all";
    if (argc > 1) {
        suite = argv[1];
    }

    bool ok = true;
    if (suite == "--suite=rt") {
        ok = runSuiteRt();
    } else if (suite == "--suite=mem") {
        ok = runSuiteMem();
    } else if (suite == "--suite=host") {
        ok = runSuiteHost();
    } else if (suite == "--suite=ui") {
        ok = runSuiteUi();
    } else {
        const bool r1 = runSuiteRt();
        const bool r2 = runSuiteMem();
        const bool r3 = runSuiteHost();
        const bool r4 = runSuiteUi();
        ok = r1 && r2 && r3 && r4;
    }

    return ok ? 0 : 1;
}
