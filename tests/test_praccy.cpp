#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include <numbers>

#include "audio/audio_buffer.h"
#include "audio/dsp_utils.h"
#include "audio/graph_engine.h"
#include "plugins/builtin_dsp.h"
#include "tools/tuner.h"
#include "tools/metronome.h"
#include "state/scene_manager.h"

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
    assert(driveSlot->dryWet() == 1.0f);
    assert(driveSlot->isBypassed() == false);

    // Recall Scene 1
    scenes.applyScene(1, engine);
    assert(driveSlot->dryWet() == 0.25f);
    assert(driveSlot->isBypassed() == true);

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

    std::cout << "===========================================\n";
    std::cout << "   ALL TESTS PASSED SUCCESSFULLY! (8/8)    \n";
    std::cout << "===========================================\n";
    return 0;
}
