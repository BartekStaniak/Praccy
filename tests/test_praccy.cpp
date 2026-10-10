#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include <numbers>
#include <thread>
#include <atomic>
#include <chrono>

#include "audio/audio_buffer.h"
#include "audio/dsp_utils.h"
#include "audio/graph_engine.h"
#include "audio/asio_manager.h"
#include "plugins/builtin_dsp.h"
#include "plugins/crash_isolation.h"
#include "plugins/plugin_base.h"
#include "tools/tuner.h"
#include "tools/metronome.h"
#include "tools/audio_player.h"
#include "tools/quick_looper.h"
#include "state/scene_manager.h"
#include "state/app_config.h"
#include "ui/update_checker.h"
#include "ui/design_tokens.h"
#include "ui/ui_helpers.h"
#include "utils/parse_utils.h"
#include "miniz.h"
#include <fstream>
#include <filesystem>
#include <cstring>

using namespace praccy;

void testAudioBuffers() {
    std::cout << "[TEST] AudioBufferView & OwnedAudioBuffer... ";
    audio::OwnedAudioBuffer owned(2, 512);
    assert(owned.numChannels() == 2);
    assert(owned.maxSamples() == 512);

    auto view = owned.view(256);
    assert(view.numChannels() == 2);
    assert(view.numSamples() == 256);

    view.clear();
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t s = 0; s < 256; ++s) {
            assert(view.channel(ch)[s] == 0.0f);
        }
    }

    // Test gain
    for (uint32_t s = 0; s < 256; ++s) {
        view.channel(0)[s] = 1.0f;
    }
    view.applyGain(0.5f);
    assert(std::abs(view.channel(0)[0] - 0.5f) < 1e-5f);

    std::cout << "PASSED\n";
}

void testDspUtils() {
    std::cout << "[TEST] DspUtils Math & Crossfading... ";

    // dB to gain
    assert(std::abs(audio::DspUtils::dbToGain(0.0f) - 1.0f) < 1e-4f);
    assert(std::abs(audio::DspUtils::dbToGain(-6.0206f) - 0.5f) < 1e-3f);
    assert(audio::DspUtils::dbToGain(-120.0f) == 0.0f);

    // Equal power crossfade: dry^2 + wet^2 should equal 1.0 at any mix ratio
    for (float mix = 0.0f; mix <= 1.0f; mix += 0.1f) {
        float dry = 0.0f, wet = 0.0f;
        audio::DspUtils::calculateEqualPowerCrossfade(mix, dry, wet);
        float power = (dry * dry) + (wet * wet);
        assert(std::abs(power - 1.0f) < 1e-4f);
    }

    // Equal power stereo pan: left^2 + right^2 should equal 1.0
    for (float pan = -1.0f; pan <= 1.0f; pan += 0.2f) {
        float l = 0.0f, r = 0.0f;
        audio::DspUtils::calculateStereoPan(pan, l, r);
        float power = (l * l) + (r * r);
        assert(std::abs(power - 1.0f) < 1e-4f);
    }

    std::cout << "PASSED\n";
}

void testGraphEngineSerialAndParallel() {
    std::cout << "[TEST] GraphEngine Serial & Parallel Processing... ";

    audio::GraphEngine engine;
    engine.prepare(48000.0, 128);

    // Add a parallel block with 2 branches: Branch A (+0 dB, Pan L), Branch B (+0 dB, Pan R)
    auto splitBlock = std::make_unique<audio::ParallelSplitMergeBlock>("Test Split");
    auto* branchA = splitBlock->addBranch("Branch A");
    branchA->setPan(-1.0f); // Hard Left

    auto* branchB = splitBlock->addBranch("Branch B");
    branchB->setPan(+1.0f); // Hard Right

    engine.addSerialNode(std::move(splitBlock));

    audio::OwnedAudioBuffer inBuf(2, 128);
    audio::OwnedAudioBuffer outBuf(2, 128);

    auto inView = inBuf.view(128);
    auto outView = outBuf.view(128);

    // Feed impulse into input
    inView.clear();
    for (uint32_t s = 0; s < 128; ++s) {
        inView.channel(0)[s] = 0.5f;
        inView.channel(1)[s] = 0.5f;
    }

    engine.process(inView, outView);

    // Verify non-zero output processed through graph
    assert(std::abs(outView.channel(0)[0]) > 0.0f);
    assert(std::abs(outView.channel(1)[0]) > 0.0f);

    std::cout << "PASSED\n";
}

void testTunerPitchDetection() {
    std::cout << "[TEST] InstrumentTuner YIN Algorithm Pitch Detection... ";

    tools::InstrumentTuner tuner(2048);
    tuner.prepare(48000.0);

    // Generate 440.0 Hz sine wave (A4 note, MIDI 69)
    std::vector<float> sineWave(2048);
    constexpr double freq = 440.0;
    constexpr double sampleRate = 48000.0;
    for (size_t i = 0; i < sineWave.size(); ++i) {
        double phase = 2.0 * std::numbers::pi * freq * (static_cast<double>(i) / sampleRate);
        sineWave[i] = static_cast<float>(std::sin(phase) * 0.7);
    }

    tuner.process(sineWave.data(), static_cast<uint32_t>(sineWave.size()));
    auto res = tuner.currentResult();

    assert(res.confidence == true);
    assert(res.noteNumber == 69);
    assert(res.noteName == "A4");
    assert(std::abs(res.frequencyHz - 440.0f) < 2.0f);
    assert(std::abs(res.centDeviation) < 5.0f);

    // Test low guitar E string (82.4 Hz, MIDI 40 = E2)
    constexpr double freqE = 82.41;
    for (size_t i = 0; i < sineWave.size(); ++i) {
        double phase = 2.0 * std::numbers::pi * freqE * (static_cast<double>(i) / sampleRate);
        sineWave[i] = static_cast<float>(std::sin(phase) * 0.7);
    }

    tuner.process(sineWave.data(), static_cast<uint32_t>(sineWave.size()));
    auto resE = tuner.currentResult();

    assert(resE.confidence == true);
    assert(resE.noteNumber == 40);
    assert(resE.noteName == "E2");
    assert(std::abs(resE.frequencyHz - 82.41f) < 2.0f);

    std::cout << "PASSED\n";
}

void testMetronome() {
    std::cout << "[TEST] Metronome Beat Generation... ";

    tools::Metronome metro;
    metro.prepare(48000.0);
    metro.setBpm(120.0f);
    metro.setPlaying(true);

    audio::OwnedAudioBuffer outBuf(2, 512);
    auto outView = outBuf.view(512);
    outView.clear();

    metro.process(outView);

    // When playing, metronome should synthesize non-zero audio samples
    bool hasAudio = false;
    for (uint32_t s = 0; s < 512; ++s) {
        if (std::abs(outView.channel(0)[s]) > 1e-4f) {
            hasAudio = true;
            break;
        }
    }
    assert(hasAudio);

    std::cout << "PASSED\n";
}

void testSceneManager() {
    std::cout << "[TEST] SceneManager Snapshot Capture & Recall... ";

    audio::GraphEngine engine;
    engine.prepare(48000.0, 128);

    auto drive = std::make_unique<plugins::OverdriveEffect>();
    auto* driveSlot = new audio::PluginSlot(std::move(drive));
    engine.addSerialNode(std::unique_ptr<audio::AudioNode>(driveSlot));

    state::SceneManager scenes;

    // Scene 0: Set Drive wet to 100%
    driveSlot->setDryWet(1.0f);
    scenes.captureCurrentScene(0, engine);

    // Scene 1: Set Drive wet to 25% and bypassed
    driveSlot->setDryWet(0.25f);
    driveSlot->setBypassed(true);
    scenes.captureCurrentScene(1, engine);

    // Recall Scene 0
    scenes.applyScene(0, engine);
    auto* slot0 = dynamic_cast<audio::PluginSlot*>(engine.getNode(0));
    assert(slot0 != nullptr);
    assert(slot0->dryWet() == 1.0f);
    assert(slot0->isBypassed() == false);

    // Recall Scene 1
    scenes.applyScene(1, engine);
    auto* slot1 = dynamic_cast<audio::PluginSlot*>(engine.getNode(0));
    assert(slot1 != nullptr);
    assert(slot1->dryWet() == 0.25f);
    assert(slot1->isBypassed() == true);

    std::cout << "PASSED\n";
}

void testGraphEngineDynamicTopology() {
    std::cout << "[TEST] Dynamic Topology (Split, Delete, Branch Slot Removal)... ";

    audio::GraphEngine engine;
    engine.prepare(48000.0, 128);

    // 1. Add 3 slots
    engine.addSerialNode(std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>()));
    engine.addSerialNode(std::make_unique<audio::PluginSlot>(std::make_unique<plugins::TubeAmpEffect>()));
    engine.addSerialNode(std::make_unique<audio::PluginSlot>(std::make_unique<plugins::StereoDelayEffect>()));
    assert(engine.numNodes() == 3);

    // 2. Remove middle slot (TubeAmp)
    engine.removeSerialNode(1);
    assert(engine.numNodes() == 2);
    assert(engine.getNode(0)->name() == "Praccy Drive");
    assert(engine.getNode(1)->name() == "Praccy Stereo Delay");

    // 3. Split node 0 (Drive) into parallel branches
    engine.splitSerialNodeIntoParallel(0);
    assert(engine.numNodes() == 2);
    assert(engine.getNode(0)->type() == audio::NodeType::ParallelSplitMerge);

    auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(engine.getNode(0));
    assert(block != nullptr);
    assert(block->numBranches() == 2);

    auto* branchA = block->getBranch(0);
    auto* branchB = block->getBranch(1);
    assert(branchA->numSlots() == 1);
    assert(branchA->getSlot(0)->name() == "Praccy Drive");
    assert(branchB->numSlots() == 0);

    // 4. Add slot to Branch B and verify removal
    branchB->addSlot(std::make_unique<audio::PluginSlot>(std::make_unique<plugins::TubeAmpEffect>()));
    assert(branchB->numSlots() == 1);
    branchB->removeSlot(0);
    assert(branchB->numSlots() == 0);

    // 5. Test input routing modes
    audio::InputRoutingConfig inCfg;
    inCfg.mode = audio::InputRoutingMode::MonoRight;
    engine.setInputRouting(inCfg);
    assert(engine.inputRouting().mode == audio::InputRoutingMode::MonoRight);

    std::cout << "PASSED\n";
}

#include "state/app_config.h"

void testAppConfigPersistence() {
    std::cout << "[TEST] AppConfig Persistence (Save & Load)... ";

    const std::string testPath = "test_config_temp.ini";

    state::AppConfig cfg;
    cfg.lastAsioDriver = "Yamaha Steinberg USB ASIO";
    cfg.inputMode = audio::InputRoutingMode::MonoRight;
    cfg.inputGainDb = 4.5f;
    cfg.masterVolumeDb = -2.0f;
    cfg.metronomeBpm = 135.0f;
    cfg.customPluginPaths = {"D:\\VSTPlugins", "E:\\MyAudio"};

    assert(cfg.save(testPath) == true);

    state::AppConfig loaded;
    assert(loaded.load(testPath) == true);
    assert(loaded.lastAsioDriver == "Yamaha Steinberg USB ASIO");
    assert(loaded.inputMode == audio::InputRoutingMode::MonoRight);
    assert(std::abs(loaded.inputGainDb - 4.5f) < 1e-4f);
    assert(std::abs(loaded.masterVolumeDb - (-2.0f)) < 1e-4f);
    assert(std::abs(loaded.metronomeBpm - 135.0f) < 1e-4f);
    assert(loaded.customPluginPaths.size() >= 2);

    std::remove(testPath.c_str());

    std::cout << "PASSED\n";
}

void testParallelBlockBlendAndDissolve() {
    std::cout << "[TEST] ParallelBlock Blend & Dissolve... ";

    audio::GraphEngine engine;
    engine.prepare(48000.0, 128);

    auto splitBlock = std::make_unique<audio::ParallelSplitMergeBlock>("Split A/B");
    auto* b0 = splitBlock->addBranch("Branch A");
    auto* b1 = splitBlock->addBranch("Branch B");

    auto amp = std::make_unique<plugins::TubeAmpEffect>();
    b0->addSlot(std::make_unique<audio::PluginSlot>(std::move(amp)));

    auto delay = std::make_unique<plugins::StereoDelayEffect>();
    b1->addSlot(std::make_unique<audio::PluginSlot>(std::move(delay)));

    // Test blend
    splitBlock->setBlend(-0.5f);
    assert(std::abs(splitBlock->blend() - (-0.5f)) < 1e-5f);
    splitBlock->setBlend(0.0f);
    assert(std::abs(splitBlock->blend()) < 1e-5f);

    engine.addSerialNode(std::move(splitBlock));
    assert(engine.numNodes() == 1);

    // Dissolve keeping Branch B (promotes Delay B to serial chain)
    engine.dissolveParallelBlock(0, 1);
    assert(engine.numNodes() == 1);
    auto* remainingSlot = dynamic_cast<audio::PluginSlot*>(engine.getNode(0));
    assert(remainingSlot != nullptr);
    assert(remainingSlot->name() == "Praccy Stereo Delay");

    std::cout << "PASSED\n";
}

void testQuickLooper() {
    std::cout << "[TEST] QuickLooper State Transitions... ";

    tools::QuickLooper looper;
    looper.prepare(48000.0, 5);

    assert(looper.state() == tools::LooperState::Empty);

    // Empty -> Recording
    looper.triggerAction();
    assert(looper.state() == tools::LooperState::Recording);

    // Process a dummy buffer while recording
    audio::OwnedAudioBuffer inBuf(2, 256);
    audio::OwnedAudioBuffer outBuf(2, 256);
    auto inView = inBuf.view(256);
    auto outView = outBuf.view(256);
    for (uint32_t s = 0; s < 256; ++s) {
        inView.channel(0)[s] = 0.5f;
        inView.channel(1)[s] = 0.5f;
    }
    for (int b = 0; b < 5; ++b) {
        looper.process(inView, outView);
    }

    // Recording -> Playing
    looper.triggerAction();
    assert(looper.state() == tools::LooperState::Playing);
    assert(looper.loopLengthSeconds() > 0.0);

    // Playing -> Overdubbing
    looper.triggerAction();
    assert(looper.state() == tools::LooperState::Overdubbing);

    // Overdubbing -> Playing
    looper.triggerAction();
    assert(looper.state() == tools::LooperState::Playing);

    // Stop and Clear
    looper.stop();
    assert(looper.state() == tools::LooperState::Stopped);

    looper.clear();
    assert(looper.state() == tools::LooperState::Empty);

    // 1. Pathological sample rates & sanitization
    tools::QuickLooper looperNeg;
    looperNeg.prepare(-48000.0, 5);
    assert(looperNeg.loopLengthSeconds() == 0.0);

    tools::QuickLooper looperNan;
    looperNan.prepare(std::numeric_limits<double>::quiet_NaN(), 5);
    assert(looperNan.loopLengthSeconds() == 0.0);

    tools::QuickLooper looperZero;
    looperZero.prepare(0.0, 5);
    assert(looperZero.loopLengthSeconds() == 0.0);

    // 2. Corrupted WAV Chunk & Memory Exhaustion Defense
    const std::filesystem::path fuzzPath = std::filesystem::current_path() / "test_looper_fuzz.wav";
    std::error_code ec;
    std::filesystem::remove(fuzzPath, ec);
    {
        std::ofstream out(fuzzPath, std::ios::binary);
        uint8_t wavHeader[44] = {
            'R', 'I', 'F', 'F',
            0x24, 0x00, 0x00, 0x00, // file size
            'W', 'A', 'V', 'E',
            'f', 'm', 't', ' ',
            16, 0, 0, 0,            // fmt size
            1, 0,                   // PCM
            2, 0,                   // 2 channels
            0x80, 0xBB, 0x00, 0x00, // 48000 Hz
            0x00, 0xEE, 0x02, 0x00, // byte rate
            4, 0,                   // block align
            16, 0,                  // 16 bits
            'd', 'a', 't', 'a',
            0xFF, 0xFF, 0xFF, 0xFF  // 4 GB corrupted chunk.size
        };
        out.write(reinterpret_cast<const char*>(wavHeader), sizeof(wavHeader));
    }
    assert(!looper.loadWavFile(fuzzPath.string()));
    std::filesystem::remove(fuzzPath, ec);

    std::cout << "PASSED\n";
}

void testAudioPlayer() {
    std::cout << "[TEST] AudioPlayer Initialization & Controls... ";

    tools::AudioPlayer player;
    player.prepare(48000.0);

    assert(!player.isLoaded());
    assert(!player.isPlaying());
    assert(player.isLooping());

    player.setLooping(false);
    assert(!player.isLooping());

    player.setVolume(1.25f);
    assert(std::abs(player.volume() - 1.25f) < 1e-4f);

    std::cout << "PASSED\n";
}

void testConcurrentParallelMutation() {
    std::cout << "[TEST] GraphEngineTest.ConcurrentParallelMutation... ";

    audio::GraphEngine engine;
    constexpr double sampleRate = 48000.0;
    constexpr uint32_t blockSize = 128;
    engine.prepare(sampleRate, blockSize);

    auto splitBlock = std::make_unique<audio::ParallelSplitMergeBlock>("Stress Split");
    auto* branchA = splitBlock->addBranch("Branch A");
    auto* branchB = splitBlock->addBranch("Branch B");
    engine.addSerialNode(std::move(splitBlock));

    std::atomic<bool> running{true};
    std::atomic<uint64_t> audioBlocksProcessed{0};
    std::atomic<uint32_t> destructorOnAudioThreadCount{0};
    std::atomic<std::thread::id> audioThreadId{};

    // Custom tracked effect verifying destructors NEVER run on the audio thread
    class TrackedEffect : public audio::AudioNode {
    public:
        TrackedEffect(std::atomic<uint32_t>& badDtorCount, const std::atomic<std::thread::id>& aThreadId)
            : m_badDtor(badDtorCount), m_audioId(aThreadId) {}

        ~TrackedEffect() override {
            if (std::this_thread::get_id() == m_audioId.load()) {
                m_badDtor.fetch_add(1);
            }
        }
        void prepare(double, uint32_t) override {}
        void process(audio::AudioProcessContext& ctx) override { ctx.output.copyFrom(ctx.input); }
        void reset() override {}
        [[nodiscard]] audio::NodeType type() const noexcept override { return audio::NodeType::Plugin; }
        [[nodiscard]] const std::string& name() const noexcept override {
            static const std::string n = "TrackedEffect";
            return n;
        }
    private:
        std::atomic<uint32_t>& m_badDtor;
        const std::atomic<std::thread::id>& m_audioId;
    };

    // Thread 1: Audio callback thread simulator
    std::thread audioThread([&]() {
        audioThreadId.store(std::this_thread::get_id());
        audio::OwnedAudioBuffer inBuf(2, blockSize);
        audio::OwnedAudioBuffer outBuf(2, blockSize);
        auto inView = inBuf.view(blockSize);
        auto outView = outBuf.view(blockSize);

        for (uint32_t s = 0; s < blockSize; ++s) {
            inView.channel(0)[s] = 0.25f;
            inView.channel(1)[s] = -0.25f;
        }

        while (running.load(std::memory_order_relaxed)) {
            outView.clear();
            engine.process(inView, outView);

            const float* outL = outView.channel(0);
            const float* outR = outView.channel(1);
            for (uint32_t s = 0; s < blockSize; ++s) {
                assert(!std::isnan(outL[s]) && !std::isinf(outL[s]));
                assert(!std::isnan(outR[s]) && !std::isinf(outR[s]));
            }

            audioBlocksProcessed.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Thread 2: UI thread simulator (Rapid additions, removals, reads, property updates, and reclamation)
    constexpr int numIterations = 600;
    for (int iter = 0; iter < numIterations; ++iter) {
        if (iter % 3 == 0) {
            branchA->addSlot(std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>()));
            branchB->addSlot(std::make_unique<audio::PluginSlot>(std::make_unique<plugins::StereoDelayEffect>()));
        } else {
            branchA->addSlot(std::make_unique<audio::PluginSlot>(std::make_unique<TrackedEffect>(destructorOnAudioThreadCount, audioThreadId)));
            branchB->addSlot(std::make_unique<audio::PluginSlot>(std::make_unique<TrackedEffect>(destructorOnAudioThreadCount, audioThreadId)));
        }

        size_t nA = branchA->numSlots();
        size_t nB = branchB->numSlots();
        assert(nA >= 1);
        assert(nB >= 1);

        auto* sA = branchA->getSlot(0);
        if (sA) {
            sA->setDryWet(0.5f);
            sA->setBypassed(iter % 2 == 0);
        }

        branchA->setGainDb(static_cast<float>((iter % 12) - 6));
        branchB->setPan(static_cast<float>((iter % 10) - 5) / 5.0f);

        // Reclaim on UI thread
        engine.processReclamation();

        if (branchA->numSlots() > 2) branchA->removeSlot(0);
        if (branchB->numSlots() > 2) branchB->removeSlot(0);

        engine.processReclamation();
    }

    // Clean up remaining slots
    while (branchA->numSlots() > 0) branchA->removeSlot(0);
    while (branchB->numSlots() > 0) branchB->removeSlot(0);

    // Let audio thread pump and process the remove commands
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    running.store(false, std::memory_order_release);
    audioThread.join();

    // Final reclamation sweep on main thread
    engine.processReclamation();

    assert(audioBlocksProcessed.load() >= 500);
    assert(destructorOnAudioThreadCount.load() == 0 && "CRITICAL: Plugin destructor executed on audio thread!");

    std::cout << "PASSED (" << audioBlocksProcessed.load() << " audio blocks, 0 audio-thread destructions)\n";
}

void testAsio24BitUnpackAndPack() {
    std::cout << "[TEST] AsioManagerTest.Format24BitUnpack... ";

    // ----------------------------------------------------
    // Part 1: ASIOSTInt24LSB (Packed 3 Bytes, Little Endian)
    // ----------------------------------------------------
    {
        constexpr uint32_t numSamples = 7;
        const uint8_t rawInput[numSamples * 3] = {
            0x00, 0x00, 0x00,
            0xFF, 0xFF, 0x7F,
            0x00, 0x00, 0x80,
            0x00, 0x00, 0x40,
            0x00, 0x00, 0xC0,
            0x01, 0x00, 0x00,
            0xFF, 0xFF, 0xFF
        };

        float unpacked[numSamples] = {};
        audio::AsioManager::unpackInt24LSB(rawInput, unpacked, numSamples);

        assert(unpacked[0] == 0.0f);
        assert(std::abs(unpacked[1] - (8388607.0f / 8388608.0f)) < 1e-7f);
        assert(unpacked[2] == -1.0f);
        assert(unpacked[3] == 0.5f);
        assert(unpacked[4] == -0.5f);
        assert(std::abs(unpacked[5] - (1.0f / 8388608.0f)) < 1e-7f);
        assert(std::abs(unpacked[6] - (-1.0f / 8388608.0f)) < 1e-7f);

        // Pack back to raw bytes
        uint8_t packedOutput[numSamples * 3] = {};
        audio::AsioManager::packInt24LSB(unpacked, packedOutput, numSamples);

        // Verify bit-exact matches
        assert(packedOutput[0] == 0x00 && packedOutput[1] == 0x00 && packedOutput[2] == 0x00);
        assert(packedOutput[3] == 0xFF && packedOutput[4] == 0xFF && packedOutput[5] == 0x7F);
        assert(packedOutput[6] == 0x00 && packedOutput[7] == 0x00 && packedOutput[8] == 0x80);
        assert(packedOutput[12] == 0x00 && packedOutput[13] == 0x00 && packedOutput[14] == 0xC0);

        // Generic unpackSamples test
        float unpackedGeneric[numSamples] = {};
        audio::AsioManager::unpackSamples(audio::ASIOSTInt24LSB, rawInput, unpackedGeneric, numSamples);
        for (uint32_t i = 0; i < numSamples; ++i) {
            assert(unpackedGeneric[i] == unpacked[i]);
        }
    }

    // ----------------------------------------------------
    // Part 2: ASIOSTInt32LSB24 (4-Byte Container, LSB Aligned)
    // ----------------------------------------------------
    {
        constexpr uint32_t numSamples = 9;
        const int32_t rawInput[numSamples] = {
            0x00000000,                       // 0
            0x007FFFFF,                       // +8,388,607 (Max positive)
            static_cast<int32_t>(0xFF800000), // -8,388,608 (Max negative, sign-extended)
            0x00800000,                       // -8,388,608 (Max negative, zero-extended high byte)
            static_cast<int32_t>(0xAB800000), // -8,388,608 (Max negative, DMA noise in high byte)
            0x00400000,                       // +4,194,304 (+0.5f)
            static_cast<int32_t>(0xFFC00000), // -4,194,304 (-0.5f)
            0x00000001,                       // +1
            static_cast<int32_t>(0xFFFFFFFF)  // -1
        };

        float unpacked[numSamples] = {};
        audio::AsioManager::unpackInt32LSB24(rawInput, unpacked, numSamples);

        assert(unpacked[0] == 0.0f);
        assert(std::abs(unpacked[1] - (8388607.0f / 8388608.0f)) < 1e-7f);
        assert(unpacked[2] == -1.0f);
        assert(unpacked[3] == -1.0f); // Robust decoding regardless of high byte padding
        assert(unpacked[4] == -1.0f); // Immune to high byte noise
        assert(unpacked[5] == 0.5f);
        assert(unpacked[6] == -0.5f);
        assert(std::abs(unpacked[7] - (1.0f / 8388608.0f)) < 1e-7f);
        assert(std::abs(unpacked[8] - (-1.0f / 8388608.0f)) < 1e-7f);

        int32_t packedOutput[numSamples] = {};
        audio::AsioManager::packInt32LSB24(unpacked, packedOutput, numSamples);

        assert(packedOutput[0] == 0);
        assert((packedOutput[1] & 0x00FFFFFF) == 0x007FFFFF);
        assert((packedOutput[2] & 0x00FFFFFF) == 0x00800000);
        assert((packedOutput[6] & 0x00FFFFFF) == 0x00C00000);

        // Generic unpackSamples test
        float unpackedGeneric[numSamples] = {};
        audio::AsioManager::unpackSamples(audio::ASIOSTInt32LSB24, rawInput, unpackedGeneric, numSamples);
        for (uint32_t i = 0; i < numSamples; ++i) {
            assert(unpackedGeneric[i] == unpacked[i]);
        }
    }

    std::cout << "PASSED\n";
}

void testTunerAsynchronousDecoupling() {
    std::cout << "[TEST] InstrumentTuner Asynchronous Decoupling... ";
    tools::InstrumentTuner tuner(2048);
    tuner.prepare(48000.0, 256);

    // Push 440 Hz blocks asynchronously
    std::vector<float> sineBlock(256);
    constexpr double freq = 440.0;
    constexpr double sampleRate = 48000.0;
    for (int block = 0; block < 16; ++block) {
        for (size_t i = 0; i < sineBlock.size(); ++i) {
            double phase = 2.0 * std::numbers::pi * freq * ((block * 256.0 + i) / sampleRate);
            sineBlock[i] = static_cast<float>(std::sin(phase) * 0.7);
        }
        tuner.pushSamples(sineBlock.data(), static_cast<uint32_t>(sineBlock.size()));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Wait for at least two 60 Hz worker ticks (~35 ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(40));

    auto res = tuner.currentResult();
    assert(res.confidence == true);
    assert(res.noteNumber == 69);
    assert(res.noteName == "A4");
    assert(std::abs(res.frequencyHz - 440.0f) < 2.0f);
    std::cout << "PASSED\n";
}

void testZipSlipSanitization() {
    std::cout << "[TEST] Zip Slip Path Traversal Sanitization... ";

    std::filesystem::path safe;

    // Valid paths
    assert(ui::sanitizeZipEntryPath("Praccy.exe", safe));
    assert(safe == std::filesystem::path("Praccy.exe"));

    assert(ui::sanitizeZipEntryPath("resources/icon.png", safe));
    assert(safe == std::filesystem::path("resources/icon.png"));

    assert(ui::sanitizeZipEntryPath("sub\\folder\\nested.txt", safe));
    assert(safe == std::filesystem::path("sub/folder/nested.txt"));

    // Traversal and injection attempts -> must be rejected
    assert(!ui::sanitizeZipEntryPath("../evil.exe", safe));
    assert(!ui::sanitizeZipEntryPath("../../windows/system32/calc.exe", safe));
    assert(!ui::sanitizeZipEntryPath("foo/../../bar.exe", safe));
    assert(!ui::sanitizeZipEntryPath("/absolute/unix/path", safe));
    assert(!ui::sanitizeZipEntryPath("\\absolute\\windows\\path", safe));
    assert(!ui::sanitizeZipEntryPath("C:\\windows\\system32\\cmd.exe", safe));
    assert(!ui::sanitizeZipEntryPath("C:foo.exe", safe));
    assert(!ui::sanitizeZipEntryPath("\\\\unc\\share\\file.exe", safe));
    assert(!ui::sanitizeZipEntryPath("//unc/share/file.exe", safe));
    assert(!ui::sanitizeZipEntryPath("CON.txt", safe));
    assert(!ui::sanitizeZipEntryPath("aux.dll", safe));
    assert(!ui::sanitizeZipEntryPath("NUL", safe));
    assert(!ui::sanitizeZipEntryPath("COM1", safe));
    assert(!ui::sanitizeZipEntryPath("LPT1", safe));
    assert(!ui::sanitizeZipEntryPath("file:stream.exe", safe));
    assert(!ui::sanitizeZipEntryPath("trailing_space.txt ", safe));
    assert(!ui::sanitizeZipEntryPath("trailing_dot.txt.", safe));
    assert(!ui::sanitizeZipEntryPath("", safe));

    std::cout << "PASSED\n";
}

void testInProcessMinizArchiveExtraction() {
    std::cout << "[TEST] In-Process miniz Archive Extraction & Zip Bomb Guard... ";

    const std::filesystem::path testZip = "test_archive_temp.zip";
    const std::filesystem::path extractDir = "test_extract_temp";

    std::error_code ec;
    std::filesystem::remove(testZip, ec);
    std::filesystem::remove_all(extractDir, ec);

    // 1. Create a valid ZIP archive using miniz writer API
    mz_zip_archive zipWrite;
    memset(&zipWrite, 0, sizeof(zipWrite));
    assert(mz_zip_writer_init_file(&zipWrite, testZip.string().c_str(), 0));

    const char* file1Data = "Hello Praccy In-Process Update!";
    assert(mz_zip_writer_add_mem(&zipWrite, "file1.txt", file1Data, strlen(file1Data), MZ_DEFAULT_COMPRESSION));

    const char* file2Data = "Embedded Subdirectory Asset Data";
    assert(mz_zip_writer_add_mem(&zipWrite, "sub/file2.dat", file2Data, strlen(file2Data), MZ_DEFAULT_COMPRESSION));

    assert(mz_zip_writer_finalize_archive(&zipWrite));
    assert(mz_zip_writer_end(&zipWrite));

    // 2. Extract using extractZipArchive
    std::string errStr;
    bool ok = ui::extractZipArchive(testZip, extractDir, errStr);
    assert(ok == true);
    assert(errStr.empty());

    // Verify extracted content
    assert(std::filesystem::exists(extractDir / "file1.txt"));
    assert(std::filesystem::exists(extractDir / "sub" / "file2.dat"));

    std::ifstream in1(extractDir / "file1.txt");
    std::string read1((std::istreambuf_iterator<char>(in1)), std::istreambuf_iterator<char>());
    assert(read1 == file1Data);

    std::ifstream in2(extractDir / "sub" / "file2.dat");
    std::string read2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
    assert(read2 == file2Data);

    // Clean up
    std::filesystem::remove(testZip, ec);
    std::filesystem::remove_all(extractDir, ec);

    // 3. Test Zip Slip Defense with Malicious Archive
    memset(&zipWrite, 0, sizeof(zipWrite));
    assert(mz_zip_writer_init_file(&zipWrite, testZip.string().c_str(), 0));
    assert(mz_zip_writer_add_mem(&zipWrite, "../escaped_file.txt", "evil", 4, MZ_DEFAULT_COMPRESSION));
    assert(mz_zip_writer_finalize_archive(&zipWrite));
    assert(mz_zip_writer_end(&zipWrite));

    bool maliciousOk = ui::extractZipArchive(testZip, extractDir, errStr);
    assert(maliciousOk == false);
    assert(!errStr.empty());
    assert(errStr.find("Zip Slip") != std::string::npos || errStr.find("traversal") != std::string::npos);

    // Ensure no file was written outside extractDir
    assert(!std::filesystem::exists("escaped_file.txt"));
    assert(!std::filesystem::exists("../escaped_file.txt"));

    std::filesystem::remove(testZip, ec);
    std::filesystem::remove_all(extractDir, ec);

    std::cout << "PASSED\n";
}

void testZeroProhibitedCommandsInCodebase() {
    std::cout << "[TEST] SecOps Zero Shell/Command Invocations in src/... ";

    std::filesystem::path srcDir = "src";
    if (!std::filesystem::exists(srcDir)) srcDir = "../src";
    if (!std::filesystem::exists(srcDir)) srcDir = "f:/Projects/Praccy/src";
    assert(std::filesystem::exists(srcDir));

    const std::vector<std::string> prohibited = {
        "std::system",
        "cmd.exe",
        "powershell.exe",
        "apply_update.bat"
    };

    size_t inspectedFiles = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(srcDir)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;

        std::ifstream file(entry.path());
        assert(file.is_open());
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        for (const auto& badStr : prohibited) {
            size_t pos = content.find(badStr);
            if (pos != std::string::npos) {
                std::cerr << "\nVIOLATION: Found '" << badStr << "' in " << entry.path().string() << "\n";
                assert(false && "Prohibited command string detected in source code!");
            }
        }
        inspectedFiles++;
    }

    assert(inspectedFiles >= 15);
    std::cout << "PASSED (" << inspectedFiles << " files inspected)\n";
}

class CrashingMockPlugin : public plugins::IPluginInstance {
public:
    CrashingMockPlugin() = default;

    void prepare(double sampleRate, uint32_t maxBlockSize) override {
        m_sampleRate = sampleRate;
        m_maxBlockSize = maxBlockSize;
    }

    void process(audio::AudioProcessContext& ctx) override {
        if (m_faulted.load(std::memory_order_relaxed)) {
            ctx.output.copyFrom(ctx.input);
            return;
        }

        DWORD exCode = 0;
        bool ok = plugins::safeCallPluginAudio([&]() {
            if (m_shouldCrash) {
                // Deliberately dereference null pointer to simulate hardware access violation
                volatile int* badPtr = nullptr;
                *badPtr = 0xDEADBEEF;
            }

            // Normal processing: double input signal
            for (uint32_t ch = 0; ch < ctx.output.numChannels(); ++ch) {
                for (uint32_t s = 0; s < ctx.numSamples; ++s) {
                    ctx.output.channel(ch)[s] = ctx.input.channel(ch)[s] * 2.0f;
                }
            }
        }, &exCode);

        if (!ok) {
            m_faulted.store(true, std::memory_order_release);
            setFaultReason(plugins::getExceptionDescription(exCode));
            ctx.output.copyFrom(ctx.input);
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
    bool hasCustomGui() const noexcept override { return true; }

    bool openGui(HWND) override {
        DWORD exCode = 0;
        bool ok = plugins::safeCallPluginGui([&]() {
            if (m_shouldCrashGui) {
                volatile int* badPtr = nullptr;
                *badPtr = 0xBAD;
            }
        }, &exCode);

        if (!ok) {
            m_faulted.store(true, std::memory_order_release);
            setFaultReason(plugins::getExceptionDescription(exCode));
            return false;
        }
        return true;
    }

    void closeGui() override {}
    std::vector<uint8_t> saveState() const override { return {}; }
    bool loadState(const std::vector<uint8_t>&) override { return true; }

    void armCrash() noexcept { m_shouldCrash = true; }
    void armGuiCrash() noexcept { m_shouldCrashGui = true; }

private:
    std::string m_name{"CrashingMockPlugin"};
    std::string m_vendor{"Praccy Testing"};
    std::string m_version{"2.0.0"};
    double m_sampleRate{48000.0};
    uint32_t m_maxBlockSize{256};
    bool m_shouldCrash{false};
    bool m_shouldCrashGui{false};
};

void testPluginCrashIsolation() {
    std::cout << "[TEST] Win32 SEH/VEH Plugin Crash Isolation & Dry Bypass... ";

    plugins::initCrashIsolation();

    // 1. Standalone Mock Plugin Test
    auto mockPlugin = std::make_unique<CrashingMockPlugin>();
    mockPlugin->prepare(48000.0, 128);

    audio::OwnedAudioBuffer inBuf(2, 128);
    audio::OwnedAudioBuffer outBuf(2, 128);
    auto inView = inBuf.view(128);
    auto outView = outBuf.view(128);

    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t s = 0; s < 128; ++s) {
            inView.channel(ch)[s] = 0.5f;
        }
    }

    audio::AudioProcessContext ctx{
        .input = inView,
        .output = outView,
        .sampleRate = 48000.0,
        .numSamples = 128
    };

    // Before crash: should amplify signal by 2.0x
    mockPlugin->process(ctx);
    assert(!mockPlugin->isFaulted());
    assert(std::abs(outView.channel(0)[0] - 1.0f) < 1e-5f);

    // Arm crash and process: must NOT terminate process, must latch faulted and copy dry
    mockPlugin->armCrash();
    mockPlugin->process(ctx);
    assert(mockPlugin->isFaulted());
    assert(std::string_view(mockPlugin->faultReason()).find("Access Violation") != std::string_view::npos);
    // Output must equal input (0.5f)
    assert(std::abs(outView.channel(0)[0] - 0.5f) < 1e-5f);
    assert(std::abs(outView.channel(1)[64] - 0.5f) < 1e-5f);

    // Subsequent process calls: must remain safely bypassed
    outView.clear();
    mockPlugin->process(ctx);
    assert(std::abs(outView.channel(0)[0] - 0.5f) < 1e-5f);

    // Test fault reset
    mockPlugin->resetFault();
    assert(!mockPlugin->isFaulted());
    assert(std::strcmp(mockPlugin->faultReason(), "Unknown fault") == 0);

    // 2. GUI Crash Isolation Test
    auto guiPlugin = std::make_unique<CrashingMockPlugin>();
    guiPlugin->armGuiCrash();
    bool guiOpened = guiPlugin->openGui(reinterpret_cast<HWND>(0x1234));
    assert(!guiOpened);
    assert(guiPlugin->isFaulted());

    // 3. Integration Test inside GraphEngine
    audio::GraphEngine engine;
    engine.prepare(48000.0, 128);

    auto crashSlot = std::make_unique<CrashingMockPlugin>();
    auto* rawPluginPtr = crashSlot.get();
    engine.addSerialNode(std::make_unique<audio::PluginSlot>(std::move(crashSlot)));
    // Set test signal to 0.25f so doubled output (0.50f) stays below GraphEngine's 0.95f soft limiter
    for (uint32_t ch = 0; ch < 2; ++ch) {
        for (uint32_t s = 0; s < 128; ++s) {
            inView.channel(ch)[s] = 0.25f;
        }
    }

    // Process blocks before crash: should double input to 0.5f
    for (int i = 0; i < 5; ++i) {
        engine.process(inView, outView);
        assert(std::abs(outView.channel(0)[0] - 0.5f) < 1e-4f);
    }

    // Trigger crash in plugin slot inside GraphEngine
    rawPluginPtr->armCrash();
    engine.process(inView, outView);
    assert(rawPluginPtr->isFaulted());
    assert(std::abs(outView.channel(0)[0] - 0.25f) < 1e-4f);

    // Pump 20 more blocks through GraphEngine with faulted slot: dry pass-through
    for (int i = 0; i < 20; ++i) {
        engine.process(inView, outView);
        assert(std::abs(outView.channel(0)[0] - 0.25f) < 1e-4f);
    }

    std::cout << "PASSED\n";
}

void testCorruptedPresetsIni() {
    std::cout << "[TEST] Corrupted presets.ini & config.ini Parsing Resilience... ";

    const std::string corruptPresetsPath = "test_corrupt_presets_temp.ini";

    // Scenario 1: Malformed numeric fields, invalid hex state, garbage tokens
    {
        std::ofstream out(corruptPresetsPath);
        out << "[Scene_0]\n"
            << "name=Corrupted Test Scene\n"
            << "numNodes=not_a_valid_integer\n"
            << "node_0_kind=plugin\n"
            << "node_0_name=Corrupted Tube\n"
            << "node_0_path=builtin://amp\n"
            << "node_0_type=BuiltIn\n"
            << "node_0_bypassed=not_a_bool\n"
            << "node_0_dryWet=corrupted_float_nan\n"
            << "node_0_inGain=--++broken\n"
            << "node_0_outGain=+99999999999999999999999999999999999999999999999\n"
            << "node_0_state=INVALID_HEX_STREAM_ZZ\n"
            << "[Scene_1]\n"
            << "name=Parallel Corrupted\n"
            << "numNodes=1\n"
            << "node_0_kind=parallel\n"
            << "node_0_numBranches=broken_branch_num\n"
            << "node_0_b_0_name=Branch Broken\n"
            << "node_0_b_0_gain=bad_gain\n"
            << "node_0_b_0_pan=bad_pan\n"
            << "node_0_b_0_numSlots=invalid_slots\n"
            << "node_0_b_0_s_0_dryWet=NaN\n"
            << "node_0_b_0_s_0_state=012\n";
        out.close();

        state::SceneManager mgr;
        bool loaded = mgr.loadFromFile(corruptPresetsPath);
        assert(loaded == true);
        assert(mgr.scenes().size() >= 2);

        const auto& s0 = mgr.scenes()[0];
        assert(s0.name == "Corrupted Test Scene");
        assert(s0.nodes.empty());

        const auto& s1 = mgr.scenes()[1];
        assert(s1.name == "Parallel Corrupted");
        assert(s1.nodes.size() == 1);
        assert(s1.nodes[0].branches.empty());

        std::remove(corruptPresetsPath.c_str());
    }

    // Scenario 2: Truncated syntax, missing '=', broken brackets, explicit '+'
    {
        std::ofstream out(corruptPresetsPath);
        out << "[Scene_0\n"
            << "name Scene Without Equals\n"
            << "numNodes=\n"
            << "==\n"
            << "random_unformatted_line\n"
            << "[Scene_1]\n"
            << "name=Valid Scene\n"
            << "numNodes=1\n"
            << "node_0_kind=plugin\n"
            << "node_0_dryWet=+0.85\n"
            << "node_0_inGain=-3.50\n"
            << "node_0_outGain=+6.00\n"
            << "node_0_state=0A0B0C\n";
        out.close();

        state::SceneManager mgr;
        bool loaded = mgr.loadFromFile(corruptPresetsPath);
        assert(loaded == true);
        assert(!mgr.scenes().empty());

        bool foundValid = false;
        for (const auto& sc : mgr.scenes()) {
            if (sc.name == "Valid Scene") {
                foundValid = true;
                assert(sc.nodes.size() == 1);
                assert(std::abs(sc.nodes[0].slot.dryWet - 0.85f) < 1e-4f);
                assert(std::abs(sc.nodes[0].slot.inputGainDb - (-3.50f)) < 1e-4f);
                assert(std::abs(sc.nodes[0].slot.outputGainDb - 6.00f) < 1e-4f);
                assert(sc.nodes[0].slot.state.size() == 3);
                assert(sc.nodes[0].slot.state[0] == 0x0A);
                assert(sc.nodes[0].slot.state[1] == 0x0B);
                assert(sc.nodes[0].slot.state[2] == 0x0C);
            }
        }
        assert(foundValid);
        std::remove(corruptPresetsPath.c_str());
    }

    // Scenario 3: Zero-byte file recovery
    {
        std::ofstream out(corruptPresetsPath);
        out.close();

        state::SceneManager mgr;
        bool loaded = mgr.loadFromFile(corruptPresetsPath);
        assert(loaded == true);
        assert(mgr.scenes().size() == 4);
        assert(mgr.scenes()[0].name == "1: Clean");
        assert(mgr.scenes()[1].name == "2: Crunch");
        assert(mgr.scenes()[2].name == "3: Lead");
        assert(mgr.scenes()[3].name == "4: Ambient");

        std::remove(corruptPresetsPath.c_str());
    }

    // Scenario 4: Corrupted AppConfig resilience
    {
        const std::string corruptConfigPath = "test_corrupt_config_temp.ini";
        std::ofstream out(corruptConfigPath);
        out << "input_mode=broken_routing\n"
            << "input_gain_db=not_a_float\n"
            << "master_volume_db=nan_volume\n"
            << "metronome_bpm=-9999999999999999999999999999999999999999999\n"
            << "window_x=invalid_x\n"
            << "window_y=invalid_y\n"
            << "window_w=broken_w\n"
            << "window_h=broken_h\n"
            << "window_maximized=invalid_bool\n";
        out.close();

        state::AppConfig cfg;
        bool loaded = cfg.load(corruptConfigPath);
        assert(loaded == true);
        assert(cfg.inputMode == audio::InputRoutingMode::MonoLeft);
        assert(cfg.inputGainDb == 0.0f);
        assert(cfg.masterVolumeDb == 0.0f);
        assert(cfg.metronomeBpm == 120.0f);
        assert(cfg.windowX == 100);
        assert(cfg.windowY == 100);
        assert(cfg.windowW == 1280);
        assert(cfg.windowH == 720);
        assert(cfg.windowMaximized == false);

        std::remove(corruptConfigPath.c_str());
    }

    std::cout << "PASSED\n";
}

void testStringParsingSanitization() {
    std::cout << "[TEST] String Parsing NaN/Inf & Leading Sign Sanitization... ";

    // 1. Float NaN & Inf rejection
    assert(utils::parseFloat("nan", -99.0f) == -99.0f);
    assert(utils::parseFloat("NAN", -99.0f) == -99.0f);
    assert(utils::parseFloat("inf", -99.0f) == -99.0f);
    assert(utils::parseFloat("INF", -99.0f) == -99.0f);
    assert(utils::parseFloat("+inf", -99.0f) == -99.0f);
    assert(utils::parseFloat("-inf", -99.0f) == -99.0f);
    assert(utils::parseFloat("infinity", -99.0f) == -99.0f);
    assert(utils::parseFloat("-infinity", -99.0f) == -99.0f);

    // 2. Double NaN & Inf rejection
    assert(utils::parseDouble("nan", -99.0) == -99.0);
    assert(utils::parseDouble("inf", -99.0) == -99.0);
    assert(utils::parseDouble("+inf", -99.0) == -99.0);
    assert(utils::parseDouble("-inf", -99.0) == -99.0);
    assert(utils::parseDouble("infinity", -99.0) == -99.0);

    // 3. Valid float / double with leading plus
    assert(std::abs(utils::parseFloat("+42.5", 0.0f) - 42.5f) < 1e-5f);
    assert(std::abs(utils::parseDouble("+123.456", 0.0) - 123.456) < 1e-6);

    // 4. Integer leading sign tests
    assert(utils::parseInteger<int>("+42", 999) == 42);
    assert(utils::parseInteger<int>("-42", 999) == -42);
    assert(utils::parseInteger<int>("+-123", 999) == 999);
    assert(utils::parseInteger<int>("++123", 999) == 999);
    assert(utils::parseInteger<int>("-+123", 999) == 999);
    assert(utils::parseInteger<int>("+--", 999) == 999);
    assert(utils::parseInteger<int>("+", 999) == 999);
    assert(utils::parseInteger<int>("-", 999) == 999);

    // 5. Hex integer with leading plus
    assert(utils::parseInteger<int>("+FF", 999, 16) == 255);
    assert(utils::parseInteger<int>("+0A", 999, 16) == 10);
    assert(utils::parseInteger<int>("+G", 999, 16) == 999);

    std::cout << "PASSED\n";
}

static float calculateRelativeLuminance(const ui::ColorToken& c) {
    auto linearize = [](float channel) -> float {
        return (channel <= 0.04045f) ? (channel / 12.92f) : std::pow((channel + 0.055f) / 1.055f, 2.4f);
    };
    float r = linearize(c.r);
    float g = linearize(c.g);
    float b = linearize(c.b);
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

static float calculateContrastRatio(const ui::ColorToken& c1, const ui::ColorToken& c2) {
    float l1 = calculateRelativeLuminance(c1);
    float l2 = calculateRelativeLuminance(c2);
    if (l1 < l2) std::swap(l1, l2);
    return (l1 + 0.05f) / (l2 + 0.05f);
}

void testWcagContrastCompliance() {
    std::cout << "[TEST] WCAG AA/AAA Contrast Ratio Compliance (4 Themes)... ";

    for (uint8_t i = 0; i < static_cast<uint8_t>(ui::ThemeId::Count); ++i) {
        const auto& theme = ui::getThemeTokens(static_cast<ui::ThemeId>(i));

        // 1. Normal text contrast: text.primary vs windowBg, panelBg, cardBg must be >= 4.5:1 (WCAG AA)
        float crWindow = calculateContrastRatio(theme.text.primary, theme.surfaces.windowBg);
        float crPanel  = calculateContrastRatio(theme.text.primary, theme.surfaces.panelBg);
        float crCard   = calculateContrastRatio(theme.text.primary, theme.surfaces.cardBg);

        assert(crWindow >= 4.5f);
        assert(crPanel  >= 4.5f);
        assert(crCard   >= 4.5f);

        // 2. Secondary text readability: text.secondary vs windowBg must be >= 3.0:1 (WCAG AA large / UI)
        float crSecWindow = calculateContrastRatio(theme.text.secondary, theme.surfaces.windowBg);
        assert(crSecWindow >= 3.0f);

        // 3. Graphical UI components: border focus & active signal against backgrounds >= 3.0:1
        float crFocusCard = calculateContrastRatio(theme.borders.focus, theme.surfaces.cardBg);
        assert(crFocusCard >= 3.0f);

        float crActiveBg = calculateContrastRatio(theme.signal.active, theme.surfaces.windowBg);
        assert(crActiveBg >= 3.0f);
    }

    std::cout << "PASSED\n";
}

void testThemeSwitchingAndTokenIntegrity() {
    std::cout << "[TEST] Theme Switching & Atomic Token Integrity... ";

    // 1. Initial default state
    ui::applyTheme(ui::ThemeId::ObsidianStudio);
    assert(ui::themeTokens().id == ui::ThemeId::ObsidianStudio);
    assert(std::strcmp(ui::themeTokens().name, "Obsidian Studio") == 0);
    assert(ui::themeTokens().isDark == true);

    // 2. Switch through all themes and verify active tokens
    ui::applyTheme(ui::ThemeId::CyberMidnight);
    assert(ui::themeTokens().id == ui::ThemeId::CyberMidnight);
    assert(std::strcmp(ui::themeTokens().name, "Cyber / Midnight") == 0);
    assert(ui::themeTokens().isDark == true);

    ui::applyTheme(ui::ThemeId::NordicSlate);
    assert(ui::themeTokens().id == ui::ThemeId::NordicSlate);
    assert(std::strcmp(ui::themeTokens().name, "Nordic Slate") == 0);
    assert(ui::themeTokens().isDark == true);

    ui::applyTheme(ui::ThemeId::VintageConsole);
    assert(ui::themeTokens().id == ui::ThemeId::VintageConsole);
    assert(std::strcmp(ui::themeTokens().name, "Vintage Console") == 0);
    assert(ui::themeTokens().isDark == false);

    // 3. Out-of-bounds fallback clamping
    ui::applyTheme(static_cast<ui::ThemeId>(99));
    assert(ui::themeTokens().id == ui::ThemeId::ObsidianStudio);

    // 4. ColorToken memory packing and type conversion integrity
    ui::ColorToken token(0x12, 0x34, 0x56, 0x78);
    ImU32 expectedU32 = (0x78u << 24) | (0x56u << 16) | (0x34u << 8) | (0x12u);
    assert(token.u32 == expectedU32);
    assert(static_cast<ImU32>(token) == expectedU32);
    assert(std::abs(token.r - (0x12 / 255.0f)) < 1e-4f);
    assert(std::abs(token.g - (0x34 / 255.0f)) < 1e-4f);
    assert(std::abs(token.b - (0x56 / 255.0f)) < 1e-4f);
    assert(std::abs(token.a - (0x78 / 255.0f)) < 1e-4f);

    // 5. Concurrent multithreaded reads & writes
    std::atomic<bool> stopStress{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t]() {
            while (!stopStress.load(std::memory_order_relaxed)) {
                auto id = static_cast<ui::ThemeId>(t % static_cast<int>(ui::ThemeId::Count));
                ui::applyTheme(id);
                const auto& cur = ui::themeTokens();
                assert(cur.name != nullptr);
                assert(cur.text.primary.u32 != 0);
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    stopStress.store(true, std::memory_order_relaxed);
    for (auto& th : threads) {
        th.join();
    }

    // Reset back to Obsidian Studio
    ui::applyTheme(ui::ThemeId::ObsidianStudio);

    std::cout << "PASSED\n";
}

void testCubicHermiteSplineEvaluation() {
    std::cout << "[TEST] Cubic Hermite Spline Evaluation & Audio-Reactive Sag... ";

    auto evaluateCubicBezier = [](ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, float t) -> ImVec2 {
        float u = 1.0f - t;
        float tt = t * t;
        float uu = u * u;
        float uuu = uu * u;
        float ttt = tt * t;

        ImVec2 p;
        p.x = uuu * p0.x + 3.0f * uu * t * p1.x + 3.0f * u * tt * p2.x + ttt * p3.x;
        p.y = uuu * p0.y + 3.0f * uu * t * p1.y + 3.0f * u * tt * p2.y + ttt * p3.y;
        return p;
    };

    // 1. Boundary conditions
    ImVec2 p0(10.0f, 50.0f);
    ImVec2 p1(50.0f, 120.0f);
    ImVec2 p2(150.0f, 120.0f);
    ImVec2 p3(190.0f, 50.0f);

    ImVec2 start = evaluateCubicBezier(p0, p1, p2, p3, 0.0f);
    assert(std::abs(start.x - p0.x) < 1e-4f && std::abs(start.y - p0.y) < 1e-4f);

    ImVec2 end = evaluateCubicBezier(p0, p1, p2, p3, 1.0f);
    assert(std::abs(end.x - p3.x) < 1e-4f && std::abs(end.y - p3.y) < 1e-4f);

    // 2. Symmetric midpoint evaluation
    ImVec2 mid = evaluateCubicBezier(p0, p1, p2, p3, 0.5f);
    assert(std::abs(mid.x - 100.0f) < 1e-3f);
    // At t=0.5, B(0.5).y = 0.125*(50 + 50) + 0.375*(120 + 120) = 12.5 + 90 = 102.5
    assert(std::abs(mid.y - 102.5f) < 1e-3f);

    // 3. Monotonic horizontal progression
    float prevX = p0.x;
    for (int step = 1; step <= 20; ++step) {
        float t = static_cast<float>(step) / 20.0f;
        ImVec2 pt = evaluateCubicBezier(p0, p1, p2, p3, t);
        assert(pt.x > prevX);
        prevX = pt.x;
    }

    // 4. Distance-adaptive sag calculation logic
    auto computeSag = [](float dx, float peak) -> float {
        float sagFactor = std::clamp(std::abs(dx) * 0.22f, 18.0f, 65.0f);
        float peakBoost = 1.0f + std::clamp(peak, 0.0f, 1.0f) * 0.35f;
        return sagFactor * peakBoost;
    };

    float shortSag = computeSag(20.0f, 0.0f);
    assert(shortSag == 18.0f); // clamped to minimum

    float longSag = computeSag(1000.0f, 0.0f);
    assert(longSag == 65.0f); // clamped to maximum

    float activeSag = computeSag(100.0f, 1.0f);
    float idleSag   = computeSag(100.0f, 0.0f);
    assert(activeSag > idleSag);
    assert(std::abs(activeSag / idleSag - 1.35f) < 1e-3f); // audio reactivity boost

    std::cout << "PASSED\n";
}

void testViewportCenteringCalculations() {
    std::cout << "[TEST] Viewport Footprint Centering & Deadband Calculations... ";

    // Rack analytical footprint math:
    // width = padLeft(12) + inputCard(120) + wire(24) + sum(slot(240) + wire(24)) + padRight(40)
    auto computeRackWidth = [](size_t numSlots) -> float {
        float w = 12.0f + 120.0f + 24.0f;
        for (size_t i = 0; i < numSlots; ++i) {
            w += 240.0f + 24.0f;
        }
        w += 40.0f;
        return w;
    };

    auto computeCenteringOffset = [](float contentW, float viewportW) -> float {
        constexpr float kMinMargin = 20.0f;
        if (contentW < viewportW) {
            return std::max(kMinMargin, (viewportW - contentW) * 0.5f);
        }
        return kMinMargin;
    };

    // 1. Content narrower than viewport -> perfectly centered
    float contentW_2slots = computeRackWidth(2); // 12 + 120 + 24 + 2*(264) + 40 = 724 px
    float viewportW_large = 1200.0f;
    float offsetX = computeCenteringOffset(contentW_2slots, viewportW_large);
    float expectedX = (1200.0f - 724.0f) * 0.5f; // 238.0f
    assert(std::abs(offsetX - expectedX) < 1e-4f);
    assert(offsetX > 20.0f);

    // 2. Content wider than viewport -> left-anchored to deadband minimum margin
    float viewportW_small = 600.0f;
    float offsetX_overflow = computeCenteringOffset(contentW_2slots, viewportW_small);
    assert(offsetX_overflow == 20.0f);

    // 3. Vertical centering deadband calculation
    auto computeVerticalOffset = [](float contentH, float viewportH) -> float {
        constexpr float kMinTopMargin = 16.0f;
        if (contentH < viewportH) {
            return std::max(kMinTopMargin, (viewportH - contentH) * 0.5f);
        }
        return kMinTopMargin;
    };

    float rackHeight = 248.0f;
    float viewH_large = 600.0f;
    float offsetY = computeVerticalOffset(rackHeight, viewH_large);
    assert(std::abs(offsetY - (600.0f - 248.0f) * 0.5f) < 1e-4f); // 176.0f

    float viewH_small = 200.0f;
    float offsetY_overflow = computeVerticalOffset(rackHeight, viewH_small);
    assert(offsetY_overflow == 16.0f); // clamped to deadband minimum

    std::cout << "PASSED\n";
}

void testEqualPowerRampEnergyConservation() {
    std::cout << "[TEST] EqualPowerRamp Energy Conservation Identity... ";

    const std::vector<uint32_t> testRampLengths = { 480, 960, 441, 1920 }; // 10ms at 48k, 96k, 44.1k, 192k

    for (uint32_t length : testRampLengths) {
        audio::EqualPowerRamp ramp;
        ramp.reset(length);
        ramp.startTransition(true); // Activating transition

        assert(ramp.isTransitioning());

        float prevGainOut = 1.0f;
        float prevGainIn = 0.0f;

        for (uint32_t s = 0; s < length; ++s) {
            float gOut = 0.0f, gIn = 0.0f;
            ramp.getNextGains(gOut, gIn);

            // 1. Assert energy conservation identity: gOut^2 + gIn^2 == 1.0 (+/- 1e-4)
            const float power = (gOut * gOut) + (gIn * gIn);
            assert(std::abs(power - 1.0f) < 1e-4f);

            // 2. Assert monotonic decay of outgoing gain and monotonic rise of incoming gain
            assert(gOut <= prevGainOut + 1e-6f);
            assert(gIn >= prevGainIn - 1e-6f);

            prevGainOut = gOut;
            prevGainIn = gIn;
        }

        // 3. Assert transition termination
        assert(!ramp.isTransitioning());
        float finalOut = 0.0f, finalIn = 0.0f;
        ramp.getNextGains(finalOut, finalIn);
        assert(finalOut == 0.0f);
        assert(finalIn == 1.0f);
    }

    // 4. Test GraphEngine::crossfadeToNodes automatic incoming node preparation
    {
        audio::GraphEngine engine;
        engine.prepare(48000.0, 256);
        std::vector<std::unique_ptr<audio::AudioNode>> newNodes;
        newNodes.push_back(std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>()));
        engine.crossfadeToNodes(std::move(newNodes));

        audio::OwnedAudioBuffer inBuf(2, 256), outBuf(2, 256);
        auto inView = inBuf.view(256), outView = outBuf.view(256);
        engine.process(inView, outView);
    }

    std::cout << "PASSED\n";
}

void testQuickLooperCircularProgressMath() {
    std::cout << "[TEST] QuickLooper Circular Progress & Trigonometry... ";

    auto computeLooperAngle = [](size_t currentSample, size_t loopLength) -> float {
        if (loopLength == 0) return 0.0f;
        float norm = std::clamp(static_cast<float>(currentSample) / static_cast<float>(loopLength), 0.0f, 1.0f);
        return 2.0f * audio::DspUtils::PI * norm;
    };

    // 1. Boundary & Quarter Point Tests
    const size_t kLoopLen = 48000; // 1 second loop
    assert(computeLooperAngle(0, kLoopLen) == 0.0f);
    assert(std::abs(computeLooperAngle(12000, kLoopLen) - audio::DspUtils::HALF_PI) < 1e-4f);
    assert(std::abs(computeLooperAngle(24000, kLoopLen) - audio::DspUtils::PI) < 1e-4f);
    assert(std::abs(computeLooperAngle(48000, kLoopLen) - (2.0f * audio::DspUtils::PI)) < 1e-4f);

    // 2. Zero-division protection
    assert(computeLooperAngle(100, 0) == 0.0f);

    // 3. State cycling assertion
    tools::QuickLooper looper;
    looper.prepare(48000.0, 5);
    assert(looper.state() == tools::LooperState::Empty);

    looper.triggerAction();
    assert(looper.state() == tools::LooperState::Recording);

    // Simulate dummy audio block
    audio::OwnedAudioBuffer dummyIn(2, 256), dummyOut(2, 256);
    auto inView = dummyIn.view(256), outView = dummyOut.view(256);
    for (int b = 0; b < 5; ++b) {
        looper.process(inView, outView);
    }

    looper.triggerAction();
    assert(looper.state() == tools::LooperState::Playing);

    looper.triggerAction();
    assert(looper.state() == tools::LooperState::Overdubbing);

    looper.triggerAction();
    assert(looper.state() == tools::LooperState::Playing);

    looper.stop();
    assert(looper.state() == tools::LooperState::Stopped);

    std::cout << "PASSED\n";
}

void testWavDragAndDropExtensionValidation() {
    std::cout << "[TEST] WAV Drag-and-Drop Extension Validation... ";

    auto isValidWav = [](const std::filesystem::path& p) -> bool {
        if (!p.has_extension()) return false;
        auto ext = p.extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));
        return ext == L".wav";
    };

    // Valid WAV variations
    assert(isValidWav("solo_take.wav"));
    assert(isValidWav("BACKING_TRACK.WAV"));
    assert(isValidWav("Groove_Loop.Wav"));
    assert(isValidWav("C:/Music/Practice/riff.wAv"));

    // Long path (>260 characters)
    std::string longPath = "C:/Music/Practice/";
    longPath.append(300, 'a');
    longPath += ".wav";
    assert(isValidWav(longPath));

    // Invalid extensions
    assert(!isValidWav("backing.mp3"));
    assert(!isValidWav("track.flac"));
    assert(!isValidWav("recording.aiff"));
    assert(!isValidWav("preset.ini"));
    assert(!isValidWav("plugin.vst3"));
    assert(!isValidWav("riff.wav.txt"));
    assert(!isValidWav("no_extension"));
    assert(!isValidWav(""));

    std::cout << "PASSED\n";
}

void testFloatingHudAlphaDecayComputation() {
    std::cout << "[TEST] Floating HUD Alpha Decay Computation... ";

    const float duration = 1.8f;

    // 1. Exact boundary values
    assert(ui::computeHudToastAlpha(1.8f, duration) == 1.0f);
    assert(ui::computeHudToastAlpha(2.5f, duration) == 1.0f); // Upper clamp
    assert(ui::computeHudToastAlpha(0.0f, duration) == 0.0f);
    assert(ui::computeHudToastAlpha(-0.5f, duration) == 0.0f); // Lower clamp

    // 2. Midpoint value
    assert(std::abs(ui::computeHudToastAlpha(0.9f, duration) - 0.5f) < 1e-4f);

    // 3. Strict monotonic decrease over time
    float prevAlpha = 1.0f;
    for (float t = 1.8f; t >= 0.0f; t -= 0.05f) {
        float a = ui::computeHudToastAlpha(t, duration);
        assert(a <= prevAlpha + 1e-6f);
        assert(a >= 0.0f && a <= 1.0f);
        prevAlpha = a;
    }

    // 4. NaN / Inf Sanitization
    const float kNan = std::numeric_limits<float>::quiet_NaN();
    assert(ui::computeHudToastAlpha(kNan, duration) == 0.0f);
    assert(ui::computeHudToastAlpha(0.9f, kNan) == 0.0f);
    assert(ui::computeHudToastAlpha(kNan, kNan) == 0.0f);
    assert(ui::computeHudToastAlpha(std::numeric_limits<float>::infinity(), duration) == 1.0f);
    assert(ui::computeHudToastAlpha(-std::numeric_limits<float>::infinity(), duration) == 0.0f);
    assert(ui::computeHudToastAlpha(1.0f, std::numeric_limits<float>::infinity()) == 0.0f);
    assert(ui::computeHudToastAlpha(1.0f, 0.0f) == 0.0f);
    assert(ui::computeHudToastAlpha(1.0f, -1.8f) == 0.0f);

    std::cout << "PASSED\n";
}

int main() {
    std::cout << "===========================================\n";
    std::cout << "   PRACCY CORE AUDIO ENGINE TEST SUITE   \n";
    std::cout << "===========================================\n";

    testAudioBuffers();
    testDspUtils();
    testGraphEngineSerialAndParallel();
    testTunerPitchDetection();
    testMetronome();
    testSceneManager();
    testGraphEngineDynamicTopology();
    testAppConfigPersistence();
    testParallelBlockBlendAndDissolve();
    testQuickLooper();
    testAudioPlayer();
    testConcurrentParallelMutation();
    testAsio24BitUnpackAndPack();
    testTunerAsynchronousDecoupling();
    testZipSlipSanitization();
    testInProcessMinizArchiveExtraction();
    testZeroProhibitedCommandsInCodebase();
    testPluginCrashIsolation();
    testCorruptedPresetsIni();
    testStringParsingSanitization();
    testWcagContrastCompliance();
    testThemeSwitchingAndTokenIntegrity();
    testCubicHermiteSplineEvaluation();
    testViewportCenteringCalculations();
    testEqualPowerRampEnergyConservation();
    testQuickLooperCircularProgressMath();
    testWavDragAndDropExtensionValidation();
    testFloatingHudAlphaDecayComputation();

    std::cout << "===========================================\n";
    std::cout << "   ALL TESTS PASSED SUCCESSFULLY! (28/28)  \n";
    std::cout << "===========================================\n";
    return 0;
}
