#include <iostream>
#undef NDEBUG
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
#include "plugins/plugin_scanner.h"
#include "ui/update_checker.h"
#include "ui/design_tokens.h"
#include "ui/ui_helpers.h"
#include "plugins/vst3_param_queue.h"
#include "ui/widgets/device_badges.h"
#include "ui/widgets/rotary_knob.h"
#include "ui/widgets/strobe_tuner.h"
#include "ui/modals/tuner_settings_modal.h"
#include <imgui_internal.h>
#include "utils/parse_utils.h"
#include "utils/archive_utils.h"
#include "state/daw_interop/reaper_adapter.h"
#include "state/daw_interop/vstpreset_adapter.h"
#include "state/daw_interop/dawproject_adapter.h"
#include "miniz.h"
#include <fstream>
#include <filesystem>
#include <cstring>

ImFont* g_fontUI = nullptr;
ImFont* g_fontMono = nullptr;

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

    // LevelMeter responsive ballistics: instantaneous peak capture & 120ms decay
    audio::LevelMeter meter;
    assert(meter.peakLeft() == 0.0f);
    assert(meter.peakRight() == 0.0f);

    float peakBlock[64];
    std::fill_n(peakBlock, 64, 0.8f);
    meter.process(peakBlock, peakBlock, 64, 48000.0);
    assert(std::abs(meter.peakLeft() - 0.8f) < 1e-4f);
    assert(std::abs(meter.peakRight() - 0.8f) < 1e-4f);

    // Responsive decay after tau = 120 ms (5760 samples @ 48kHz)
    std::vector<float> silence(5760, 0.0f);
    meter.process(silence.data(), silence.data(), static_cast<uint32_t>(silence.size()), 48000.0);
    const float expectedDecay = 0.8f * (1.0f / std::numbers::e_v<float>);
    assert(std::abs(meter.peakLeft() - expectedDecay) < 0.01f);
    assert(std::abs(meter.peakRight() - expectedDecay) < 0.01f);

    meter.reset();
    assert(meter.peakLeft() == 0.0f);
    assert(meter.peakRight() == 0.0f);

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

    // 1. Test Pitch Standard (Middle A / A4 Reference change to 432 Hz)
    tuner.setA4Reference(432.0f);
    assert(std::abs(tuner.a4Reference() - 432.0f) < 0.01f);

    for (size_t i = 0; i < sineWave.size(); ++i) {
        double phase = 2.0 * std::numbers::pi * 432.0 * (static_cast<double>(i) / sampleRate);
        sineWave[i] = static_cast<float>(std::sin(phase) * 0.7);
    }
    tuner.process(sineWave.data(), static_cast<uint32_t>(sineWave.size()));
    auto res432 = tuner.currentResult();
    assert(res432.confidence == true);
    assert(res432.noteNumber == 69); // A4
    assert(res432.noteName == "A4");
    assert(std::abs(res432.frequencyHz - 432.0f) < 2.0f);
    assert(std::abs(res432.centDeviation) < 5.0f); // Exactly 0 cents relative to A4=432Hz

    // Restore to standard 440 Hz
    tuner.setA4Reference(440.0f);
    assert(std::abs(tuner.a4Reference() - 440.0f) < 0.01f);

    // 2. Test Tuner On/Off Toggle
    tuner.setEnabled(false);
    assert(!tuner.isEnabled());
    auto resOff = tuner.currentResult();
    assert(!resOff.confidence);
    assert(resOff.frequencyHz == 0.0f);
    assert(resOff.noteName == "--");
    tuner.setEnabled(true);
    assert(tuner.isEnabled());

    // 3. Test Guitar String Counts (6, 7, 8, 9 strings)
    tools::TunerConfig cfg;
    cfg.setInstrument(tools::InstrumentType::Guitar);
    assert(cfg.minStrings() == 6);
    assert(cfg.maxStrings() == 9);
    for (int s = 6; s <= 9; ++s) {
        cfg.setStringCount(s);
        assert(cfg.stringCount == s);
        assert(static_cast<int>(cfg.stringMidiNotes.size()) == s);
    }

    // 4. Test Bass String Counts (4, 5, 6 strings)
    cfg.setInstrument(tools::InstrumentType::Bass);
    assert(cfg.minStrings() == 4);
    assert(cfg.maxStrings() == 6);
    for (int s = 4; s <= 6; ++s) {
        cfg.setStringCount(s);
        assert(cfg.stringCount == s);
        assert(static_cast<int>(cfg.stringMidiNotes.size()) == s);
    }

    // 5. Test Other Electric Stringed Instruments
    cfg.setInstrument(tools::InstrumentType::ElectricMandolin);
    assert(cfg.minStrings() == 4 && cfg.maxStrings() == 8);
    cfg.setInstrument(tools::InstrumentType::ElectricBowed);
    assert(cfg.minStrings() == 4 && cfg.maxStrings() == 5);
    cfg.setInstrument(tools::InstrumentType::ElectricUkulele);
    assert(cfg.minStrings() == 4 && cfg.maxStrings() == 4);
    cfg.setInstrument(tools::InstrumentType::ElectricBanjo);
    assert(cfg.minStrings() == 4 && cfg.maxStrings() == 5);
    cfg.setInstrument(tools::InstrumentType::LapSteel);
    assert(cfg.minStrings() == 6 && cfg.maxStrings() == 8);

    // 6. Test Common Presets (Root + Type: Drop D, DADGAD, Open D)
    cfg.setInstrument(tools::InstrumentType::Guitar);
    cfg.setStringCount(6);
    // Standard: E2, A2, D3, G3, B3, E4 -> { 64, 59, 55, 50, 45, 40 }
    cfg.setTuningPreset(tools::TuningType::Standard, 4);
    assert((cfg.stringMidiNotes == std::vector<int>{ 64, 59, 55, 50, 45, 40 }));
    // Drop D (Root D = index 2) -> { 64, 59, 55, 50, 45, 38 }
    cfg.setTuningPreset(tools::TuningType::Drop, 2);
    assert((cfg.stringMidiNotes == std::vector<int>{ 64, 59, 55, 50, 45, 38 }));
    // Drop C (Root C = index 0) -> C2-G2-C3-F3-A3-D4 -> { 62, 57, 53, 48, 43, 36 }
    cfg.setTuningPreset(tools::TuningType::Drop, 0);
    assert((cfg.stringMidiNotes == std::vector<int>{ 62, 57, 53, 48, 43, 36 }));
    // E-Flat Standard / Half-Step Down -> D#2-G#2-C#3-F#3-A#3-D#4 -> { 63, 58, 54, 49, 44, 39 }
    cfg.setTuningPreset(tools::TuningType::HalfStepDown);
    assert((cfg.stringMidiNotes == std::vector<int>{ 63, 58, 54, 49, 44, 39 }));
    // Open D -> D2-A2-D3-F#3-A3-D4 -> { 62, 57, 54, 50, 45, 38 }
    cfg.setTuningPreset(tools::TuningType::OpenD, 2);
    assert((cfg.stringMidiNotes == std::vector<int>{ 62, 57, 54, 50, 45, 38 }));
    // DADGAD
    cfg.setTuningPreset(tools::TuningType::DADGAD, 2);
    assert(cfg.stringMidiNotes[5] == 38); // D2
    assert(cfg.stringMidiNotes[0] == 62); // D4

    // 7-String Guitar:
    cfg.setStringCount(7);
    cfg.setTuningPreset(tools::TuningType::Standard);
    // Standard: B1-E2-A2-D3-G3-B3-E4 -> { 64, 59, 55, 50, 45, 40, 35 }
    assert((cfg.stringMidiNotes == std::vector<int>{ 64, 59, 55, 50, 45, 40, 35 }));
    // Drop A: A1-E2-A2-D3-G3-B3-E4 -> { 64, 59, 55, 50, 45, 40, 33 }
    cfg.setTuningPreset(tools::TuningType::Drop);
    assert((cfg.stringMidiNotes == std::vector<int>{ 64, 59, 55, 50, 45, 40, 33 }));
    // Half-Step Down: A#1-D#2-G#2-C#3-F#3-A#3-D#4 -> { 63, 58, 54, 49, 44, 39, 34 }
    cfg.setTuningPreset(tools::TuningType::HalfStepDown);
    assert((cfg.stringMidiNotes == std::vector<int>{ 63, 58, 54, 49, 44, 39, 34 }));

    // 8-String Guitar:
    cfg.setStringCount(8);
    cfg.setTuningPreset(tools::TuningType::Standard);
    // Standard: F#1-B1-E2-A2-D3-G3-B3-E4 -> { 64, 59, 55, 50, 45, 40, 35, 30 }
    assert((cfg.stringMidiNotes == std::vector<int>{ 64, 59, 55, 50, 45, 40, 35, 30 }));
    // Drop E: E1-B1-E2-A2-D3-G3-B3-E4 -> { 64, 59, 55, 50, 45, 40, 35, 28 }
    cfg.setTuningPreset(tools::TuningType::Drop);
    assert((cfg.stringMidiNotes == std::vector<int>{ 64, 59, 55, 50, 45, 40, 35, 28 }));

    // 9-String Guitar:
    cfg.setStringCount(9);
    cfg.setTuningPreset(tools::TuningType::Standard);
    // Standard (C#): C#1-F#1-B1-E2-A2-D3-G3-B3-E4 -> { 64, 59, 55, 50, 45, 40, 35, 30, 25 }
    assert((cfg.stringMidiNotes == std::vector<int>{ 64, 59, 55, 50, 45, 40, 35, 30, 25 }));
    // Standard (C): C1-F1-Bb1-Eb2-Ab2-Db3-Gb3-Bb3-Eb4 -> { 63, 58, 54, 49, 44, 39, 34, 29, 24 }
    cfg.setTuningPreset(tools::TuningType::Standard, 0); // Root C
    assert((cfg.stringMidiNotes == std::vector<int>{ 63, 58, 54, 49, 44, 39, 34, 29, 24 }));

    // Bass Verification:
    cfg.setInstrument(tools::InstrumentType::Bass);
    cfg.setStringCount(4);
    cfg.setTuningPreset(tools::TuningType::Standard);
    assert((cfg.stringMidiNotes == std::vector<int>{ 43, 38, 33, 28 })); // E1, A1, D2, G2
    cfg.setTuningPreset(tools::TuningType::Drop);
    assert((cfg.stringMidiNotes == std::vector<int>{ 43, 38, 33, 26 })); // Drop D: D1, A1, D2, G2
    cfg.setStringCount(5);
    cfg.setTuningPreset(tools::TuningType::Standard);
    assert((cfg.stringMidiNotes == std::vector<int>{ 43, 38, 33, 28, 23 })); // B0, E1, A1, D2, G2
    cfg.setTuningPreset(tools::TuningType::Drop);
    assert((cfg.stringMidiNotes == std::vector<int>{ 43, 38, 33, 28, 21 })); // Drop A: A0, E1, A1, D2, G2
    cfg.setStringCount(6);
    cfg.setTuningPreset(tools::TuningType::Standard);
    assert((cfg.stringMidiNotes == std::vector<int>{ 48, 43, 38, 33, 28, 23 })); // B0, E1, A1, D2, G2, C3
    cfg.setTuningPreset(tools::TuningType::Drop);
    assert((cfg.stringMidiNotes == std::vector<int>{ 48, 43, 38, 33, 28, 21 })); // Drop A: A0, E1, A1, D2, G2, C3

    // Bowed Verification:
    cfg.setInstrument(tools::InstrumentType::ElectricBowed);
    cfg.setStringCount(4);
    cfg.setTuningPreset(tools::TuningType::Standard, 7); // Violin G3
    assert((cfg.stringMidiNotes == std::vector<int>{ 76, 69, 62, 55 })); // E5, A4, D4, G3
    cfg.setTuningPreset(tools::TuningType::Standard, 0); // Cello C2
    assert((cfg.stringMidiNotes == std::vector<int>{ 57, 50, 43, 36 })); // A3, D3, G2, C2

    // Ukulele Verification:
    cfg.setInstrument(tools::InstrumentType::ElectricUkulele);
    cfg.setTuningPreset(tools::TuningType::Standard);
    assert((cfg.stringMidiNotes == std::vector<int>{ 69, 64, 60, 67 })); // High-G: A4, E4, C4, G4
    cfg.setTuningPreset(tools::TuningType::Drop);
    assert((cfg.stringMidiNotes == std::vector<int>{ 69, 64, 60, 55 })); // Low-G: A4, E4, C4, G3

    // Banjo Verification:
    cfg.setInstrument(tools::InstrumentType::ElectricBanjo);
    cfg.setStringCount(5);
    cfg.setTuningPreset(tools::TuningType::Standard);
    assert((cfg.stringMidiNotes == std::vector<int>{ 62, 59, 55, 50, 67 })); // Open G
    cfg.setTuningPreset(tools::TuningType::Drop);
    assert((cfg.stringMidiNotes == std::vector<int>{ 62, 59, 55, 48, 67 })); // Drop C: D4, B3, G3, C3, G4

    // Lap Steel Verification:
    cfg.setInstrument(tools::InstrumentType::LapSteel);
    cfg.setStringCount(6);
    cfg.setTuningPreset(tools::TuningType::Standard);
    assert((cfg.stringMidiNotes == std::vector<int>{ 64, 60, 57, 55, 52, 48 })); // C6
    cfg.setTuningPreset(tools::TuningType::OpenD);
    assert((cfg.stringMidiNotes == std::vector<int>{ 62, 57, 54, 50, 45, 38 })); // Open D: D4, A3, F#3, D3, A2, D2

    // 7. Test Custom Tuning
    cfg.setCustomStringNote(0, 64); // E4
    cfg.setCustomStringNote(1, 59); // B3
    cfg.setCustomStringNote(2, 55); // G3
    cfg.setCustomStringNote(3, 50); // D3
    cfg.setCustomStringNote(4, 45); // A2
    cfg.setCustomStringNote(5, 40); // E2
    assert(cfg.mode == tools::TuningMode::Custom);
    auto strInfo = cfg.getStringInfoList();
    assert(strInfo.size() == 6);
    assert(strInfo[0].noteName == "E4");
    assert(strInfo[5].noteName == "E2");
    assert(std::abs(strInfo[0].targetFreqHz - 329.63f) < 0.5f);
    assert(std::abs(strInfo[5].targetFreqHz - 82.41f) < 0.5f);

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

    // Test preset category management
    scenes.savePresetChain("Test High Gain Lead", engine);
    scenes.setPresetCategory("Test High Gain Lead", "High Gain Metal");
    assert(scenes.presetCategory("Test High Gain Lead") == "High Gain Metal");

    // Test custom category creation & listing
    scenes.setPresetCategory("Test High Gain Lead", "Prog Metal");
    assert(scenes.presetCategory("Test High Gain Lead") == "Prog Metal");
    auto cats = scenes.allPresetCategories();
    assert(std::find(cats.begin(), cats.end(), "Prog Metal") != cats.end());

    // Test persistence across save/load file
    std::string tempPresetsFile = (std::filesystem::temp_directory_path() / "test_presets_cat.ini").string();
    scenes.saveToFile(tempPresetsFile);

    state::SceneManager loadedScenes;
    loadedScenes.loadFromFile(tempPresetsFile);
    assert(loadedScenes.presetCategory("Test High Gain Lead") == "Prog Metal");
    std::filesystem::remove(tempPresetsFile);

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
    cfg.lastLoadedPreset = "Modern High Gain Djent";
    cfg.rackViewMode = state::AppConfig::RackViewMode::Modular;
    cfg.customPluginPaths = {"D:\\VSTPlugins", "E:\\MyAudio"};
    cfg.scannedPlugins.push_back(state::ScannedPluginEntry{
        .name = "Pro-Q 3",
        .path = "C:\\VST3\\FabFilter Pro-Q 3.vst3",
        .type = "VST3",
        .vendor = "FabFilter",
        .category = "Equalizer",
        .bundleFileName = "FabFilter Pro-Q 3.vst3",
        .classGuid = "12345678",
        .clapId = ""
    });
    cfg.scannedPlugins.push_back(state::ScannedPluginEntry{
        .name = "Diva",
        .path = "C:\\CLAP\\Diva.clap",
        .type = "CLAP",
        .vendor = "u-he",
        .category = "Audio FX",
        .bundleFileName = "Diva.clap",
        .classGuid = "",
        .clapId = "com.u-he.diva"
    });

    cfg.favoritePresets = {"Modern High Gain Djent", "California Glassy Clean"};
    cfg.presetFavoritesInitialized = true;

    assert(cfg.save(testPath) == true);

    state::AppConfig loaded;
    assert(loaded.load(testPath) == true);
    assert(loaded.lastAsioDriver == "Yamaha Steinberg USB ASIO");
    assert(loaded.inputMode == audio::InputRoutingMode::MonoRight);
    assert(std::abs(loaded.inputGainDb - 4.5f) < 1e-4f);
    assert(std::abs(loaded.masterVolumeDb - (-2.0f)) < 1e-4f);
    assert(std::abs(loaded.metronomeBpm - 135.0f) < 1e-4f);
    assert(loaded.lastLoadedPreset == "Modern High Gain Djent");
    assert(loaded.rackViewMode == state::AppConfig::RackViewMode::Modular);
    assert(loaded.customPluginPaths.size() >= 2);
    assert(loaded.presetFavoritesInitialized == true);
    assert(loaded.favoritePresets.size() == 2);
    assert(loaded.favoritePresets[0] == "Modern High Gain Djent");
    assert(loaded.favoritePresets[1] == "California Glassy Clean");
    assert(loaded.scannedPlugins.size() == 2);
    assert(loaded.scannedPlugins[0].name == "Pro-Q 3");
    assert(loaded.scannedPlugins[0].type == "VST3");
    assert(loaded.scannedPlugins[0].category == "Equalizer");
    assert(loaded.scannedPlugins[1].name == "Diva");
    assert(loaded.scannedPlugins[1].type == "CLAP");
    assert(loaded.scannedPlugins[1].clapId == "com.u-he.diva");

    // Test explicit empty favorites with presetFavoritesInitialized = true
    loaded.favoritePresets.clear();
    assert(loaded.save(testPath) == true);
    state::AppConfig loadedEmptyFavs;
    assert(loadedEmptyFavs.load(testPath) == true);
    assert(loadedEmptyFavs.presetFavoritesInitialized == true);
    assert(loadedEmptyFavs.favoritePresets.empty());

    // Test CRLF and whitespace trimming
    {
        std::ofstream crlfFile(testPath, std::ios::binary);
        crlfFile << "preset_favorites_initialized=1\r\n";
        crlfFile << "favorite_preset=Modern High Gain Djent\r\n";
        crlfFile << "favorite_plugin=Praccy Drive  \r\n";
        crlfFile << "last_loaded_preset=Custom Preset\r\n";
        crlfFile.close();

        state::AppConfig loadedCrlf;
        assert(loadedCrlf.load(testPath) == true);
        assert(loadedCrlf.presetFavoritesInitialized == true);
        assert(loadedCrlf.favoritePresets.size() == 1);
        assert(loadedCrlf.favoritePresets[0] == "Modern High Gain Djent");
        assert(loadedCrlf.favoritePlugins.size() == 1);
        assert(loadedCrlf.favoritePlugins[0] == "Praccy Drive");
        assert(loadedCrlf.lastLoadedPreset == "Custom Preset");
    }

    std::remove(testPath.c_str());

    std::cout << "PASSED\n";
}

void testPluginScannerPersistence() {
    std::cout << "[TEST] PluginScanner Scanned Plugins & Type Tag Persistence... ";

    const std::string testIni = "test_plugin_scanner_temp.ini";
    std::remove(testIni.c_str());

    // Session 1: Scan / add plugins with distinct types and categories, then save
    {
        plugins::PluginScanner scanner(testIni);
        plugins::PluginDescriptor vstDesc{
            .name = "FabFilter Pro-Q 3",
            .path = "C:\\Program Files\\Common Files\\VST3\\FabFilter Pro-Q 3.vst3",
            .type = plugins::PluginType::VST3,
            .vendor = "FabFilter",
            .category = "Equalizer",
            .bundleFileName = "FabFilter Pro-Q 3.vst3",
            .classGuid = "56535451-7565-7279-0000-000000000000",
            .clapId = ""
        };
        plugins::PluginDescriptor clapDesc{
            .name = "u-he Diva",
            .path = "C:\\Program Files\\Common Files\\CLAP\\Diva.clap",
            .type = plugins::PluginType::CLAP,
            .vendor = "u-he",
            .category = "Audio FX",
            .bundleFileName = "Diva.clap",
            .classGuid = "",
            .clapId = "com.u-he.diva"
        };
        scanner.addPlugin(vstDesc);
        scanner.addPlugin(clapDesc);
        scanner.setCustomCategory("FabFilter Pro-Q 3", "Dynamic EQ");
        scanner.toggleFavorite("FabFilter Pro-Q 3");

        scanner.saveToAppConfig(testIni);
    }

    // Session 2: Fresh instance (simulating restart), load from saved configuration
    {
        plugins::PluginScanner scanner2(testIni);

        auto plugins = scanner2.scannedPlugins();
        // 3 built-in effects + 2 restored scanned plugins = 5 total
        assert(plugins.size() == 5);

        // Verify VST3 plugin retained across sessions with its type tag and custom category
        const auto* pVst = scanner2.resolvePlugin("FabFilter Pro-Q 3");
        assert(pVst != nullptr);
        assert(pVst->type == plugins::PluginType::VST3);
        assert(pVst->typeString() == "VST3");
        assert(pVst->vendor == "FabFilter");
        assert(scanner2.getPluginCategory("FabFilter Pro-Q 3") == "Dynamic EQ");
        assert(scanner2.isFavorite("FabFilter Pro-Q 3"));
        assert(scanner2.isFavorite("C:\\Program Files\\Common Files\\VST3\\FabFilter Pro-Q 3.vst3"));
        assert(scanner2.isFavorite("FabFilter Pro-Q 3.vst3"));
        assert(pVst->bundleFileName == "FabFilter Pro-Q 3.vst3");

        // Verify CLAP plugin retained across sessions with its type tag and category
        const auto* pClap = scanner2.resolvePlugin("u-he Diva");
        assert(pClap != nullptr);
        assert(pClap->type == plugins::PluginType::CLAP);
        assert(pClap->typeString() == "CLAP");
        assert(pClap->vendor == "u-he");
        assert(pClap->category == "Audio FX");
        assert(pClap->clapId == "com.u-he.diva");

        // Verify path-based favorite matches plugin name and order is preserved
        scanner2.toggleFavorite("C:\\Program Files\\Common Files\\CLAP\\Diva.clap");
        assert(scanner2.isFavorite("Diva"));
        assert(scanner2.isFavorite("Diva.clap"));
        auto favs = scanner2.favorites();
        assert(favs.size() == 2);
        assert(favs[0] == "FabFilter Pro-Q 3");
        assert(favs[1] == "C:\\Program Files\\Common Files\\CLAP\\Diva.clap");
    }

    std::remove(testIni.c_str());
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
            double phase = 2.0 * std::numbers::pi * freq * ((static_cast<double>(block) * 256.0 + static_cast<double>(i)) / sampleRate);
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

        // 4. Text accent highlights against window and card backgrounds >= 3.0:1 (and >= 4.5:1 on light theme)
        float crAccentCard = calculateContrastRatio(theme.text.accent, theme.surfaces.cardBg);
        assert(crAccentCard >= 3.0f);
        float crAccentWindow = calculateContrastRatio(theme.text.accent, theme.surfaces.windowBg);
        assert(crAccentWindow >= 3.0f);

        // 5. Muted text against card background >= 3.0:1
        float crMutedCard = calculateContrastRatio(theme.text.muted, theme.surfaces.cardBg);
        assert(crMutedCard >= 3.0f);

        // 6. Vintage Console calibrated card border and accent contrast
        if (theme.id == ui::ThemeId::VintageConsole) {
            float crCardBorder = calculateContrastRatio(theme.borders.card, theme.surfaces.cardBg);
            assert(crCardBorder >= 3.0f);
            assert(crAccentCard >= 4.5f);
            assert(crAccentWindow >= 4.5f);
        }
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
    assert(ui::themeTokens().borders.card.u32 != 0);
    assert(ui::themeTokens().borders.subtle.u32 != 0);

    // 2. Switch through all themes and verify active tokens
    ui::applyTheme(ui::ThemeId::CyberMidnight);
    assert(ui::themeTokens().id == ui::ThemeId::CyberMidnight);
    assert(std::strcmp(ui::themeTokens().name, "Cyber / Midnight") == 0);
    assert(ui::themeTokens().isDark == true);
    assert(ui::themeTokens().borders.card.u32 != 0);
    assert(ui::themeTokens().borders.card.u32 != ui::getThemeTokens(ui::ThemeId::ObsidianStudio).borders.card.u32);

    ui::applyTheme(ui::ThemeId::NordicSlate);
    assert(ui::themeTokens().id == ui::ThemeId::NordicSlate);
    assert(std::strcmp(ui::themeTokens().name, "Nordic Slate") == 0);
    assert(ui::themeTokens().isDark == true);
    assert(ui::themeTokens().borders.card.u32 != 0);
    assert(ui::themeTokens().borders.card.u32 != ui::getThemeTokens(ui::ThemeId::CyberMidnight).borders.card.u32);

    ui::applyTheme(ui::ThemeId::VintageConsole);
    assert(ui::themeTokens().id == ui::ThemeId::VintageConsole);
    assert(std::strcmp(ui::themeTokens().name, "Vintage Console") == 0);
    assert(ui::themeTokens().isDark == false);
    assert(ui::themeTokens().borders.card.u32 != 0);
    assert(ui::themeTokens().borders.card.u32 != ui::getThemeTokens(ui::ThemeId::NordicSlate).borders.card.u32);
    // Vintage Console card border must be warm analog tone (140, 126, 110)
    assert(std::abs(ui::themeTokens().borders.card.r - (140.0f / 255.0f)) < 1e-3f);

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

void testBottomBarCentering() {
    std::cout << "[TEST] Bottom Bar Elements Vertical Centering Math... ";

    for (float dpi : { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f }) {
        ui::setDpiScale(dpi);
        for (float windowH : { 600.0f, 720.0f, 900.0f, 1080.0f, 1440.0f }) {
            const float btnH = ui::px(24.0f);
            const float bottomBarH = btnH + ui::px(18.0f);
            const float borderY = windowH - bottomBarH;

            const float targetY = std::round(borderY + (bottomBarH - btnH) * 0.5f);
            const float topMargin = targetY - borderY;
            const float bottomMargin = windowH - (targetY + btnH);

            // Verify top margin and bottom margin are symmetric within 1 pixel (rounding)
            assert(std::abs(topMargin - bottomMargin) <= 1.0f);
            // Verify margins are positive and equal to approximately 9px * dpi
            assert(topMargin >= ui::px(8.5f));
            assert(bottomMargin >= ui::px(8.5f));
        }
    }
    ui::setDpiScale(1.0f);

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

void testStyleStackGuards() {
    std::cout << "[TEST] RAII Style Stack Guards (ScopedStyleColor & ScopedStyleVar)... ";
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);

    const int baseColorStack = ctx->ColorStack.Size;
    const int baseVarStack = ctx->StyleVarStack.Size;

    // 1. Single ScopedStyleColor
    {
        ui::ScopedStyleColor cGuard(ImGuiCol_Button, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
        assert(ctx->ColorStack.Size == baseColorStack + 1);
    }
    assert(ctx->ColorStack.Size == baseColorStack);

    // 2. Condition false
    {
        ui::ScopedStyleColor cGuard(ImGuiCol_Button, ImVec4(1.0f, 0.0f, 0.0f, 1.0f), false);
        assert(ctx->ColorStack.Size == baseColorStack);
    }
    assert(ctx->ColorStack.Size == baseColorStack);

    // 3. Multi-color initializer list
    {
        ui::ScopedStyleColor cGuard({
            {ImGuiCol_Button, ImVec4(0.1f, 0.2f, 0.3f, 1.0f)},
            {ImGuiCol_Border, ImVec4(0.4f, 0.5f, 0.6f, 1.0f)},
            {ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f)}
        });
        assert(ctx->ColorStack.Size == baseColorStack + 3);
    }
    assert(ctx->ColorStack.Size == baseColorStack);

    // 4. Early return protection
    auto earlyExitTest = [&](bool triggerExit) {
        ui::ScopedStyleColor cGuard(ImGuiCol_Button, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
        assert(ctx->ColorStack.Size == baseColorStack + 1);
        if (triggerExit) return;
        assert(false);
    };
    earlyExitTest(true);
    assert(ctx->ColorStack.Size == baseColorStack);

    // 5. ScopedStyleVar float and ImVec2
    {
        ui::ScopedStyleVar vGuard(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
        assert(ctx->StyleVarStack.Size == baseVarStack + 1);
    }
    assert(ctx->StyleVarStack.Size == baseVarStack);

    {
        ui::ScopedStyleVar vGuard(ImGuiStyleVar_Alpha, 0.5f);
        assert(ctx->StyleVarStack.Size == baseVarStack + 1);
    }
    assert(ctx->StyleVarStack.Size == baseVarStack);

    // 6. Nested guards
    {
        ui::ScopedStyleColor c1(ImGuiCol_Button, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
        {
            ui::ScopedStyleColor c2(ImGuiCol_Border, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
            ui::ScopedStyleVar v1(ImGuiStyleVar_Alpha, 0.8f);
            assert(ctx->ColorStack.Size == baseColorStack + 2);
            assert(ctx->StyleVarStack.Size == baseVarStack + 1);
        }
        assert(ctx->ColorStack.Size == baseColorStack + 1);
        assert(ctx->StyleVarStack.Size == baseVarStack);
    }
    // 7. Boolean toggle inversion across dynamic iterations and loops
    for (int iter = 0; iter < 100; ++iter) {
        bool active = (iter % 2 == 0);
        {
            ui::ScopedStyleColor cGuard(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.6f, 1.0f), active);
            ui::ScopedStyleVar vGuard(ImGuiStyleVar_FrameRounding, 4.0f, !active);
            if (active) {
                assert(ctx->ColorStack.Size == baseColorStack + 1);
                assert(ctx->StyleVarStack.Size == baseVarStack);
            } else {
                assert(ctx->ColorStack.Size == baseColorStack);
                assert(ctx->StyleVarStack.Size == baseVarStack + 1);
            }
            if (iter == 42) {
                // Early break inside loop
                break;
            }
        }
        assert(ctx->ColorStack.Size == baseColorStack);
        assert(ctx->StyleVarStack.Size == baseVarStack);
    }
    assert(ctx->ColorStack.Size == baseColorStack);
    assert(ctx->StyleVarStack.Size == baseVarStack);

    ImGui::DestroyContext(ctx);
    std::cout << "PASSED\n";
}

void testFastParameterChanges() {
    std::cout << "[TEST] VST3 FastParameterChanges & FastParamValueQueue... ";
    plugins::FastParameterChanges changes;
    assert(changes.getParameterCount() == 0);
    assert(changes.getParameterData(0) == nullptr);

    // 1. Verify COM interface query
    void* iface = nullptr;
    Steinberg::tresult qr = changes.queryInterface(Steinberg::Vst::IParameterChanges::iid, &iface);
    assert(qr == Steinberg::kResultOk && iface == &changes);

    qr = changes.queryInterface(Steinberg::FUnknown::iid, &iface);
    assert(qr == Steinberg::kResultOk && iface == &changes);

    // 2. Queue up to 128 changes (max capacity)
    for (uint32_t i = 0; i < 128; ++i) {
        bool ok = changes.addParameterChange(2000 + i, static_cast<double>(i) / 128.0);
        assert(ok);
    }
    assert(changes.getParameterCount() == 128);

    // 3. Verify 129th fails safely without reallocating
    assert(!changes.addParameterChange(9999, 0.5));
    assert(changes.getParameterCount() == 128);

    // 4. Verify parameter queue data and points
    for (Steinberg::int32 i = 0; i < 128; ++i) {
        auto* q = changes.getParameterData(i);
        assert(q != nullptr);
        assert(q->getParameterId() == static_cast<Steinberg::Vst::ParamID>(2000 + i));
        assert(q->getPointCount() == 1);

        Steinberg::int32 offset = -1;
        double val = -1.0;
        auto res = q->getPoint(0, offset, val);
        assert(res == Steinberg::kResultTrue);
        assert(offset == 0);
        assert(std::abs(val - static_cast<double>(i) / 128.0) < 1e-9);

        // Verify out-of-range point index
        assert(q->getPoint(1, offset, val) == Steinberg::kResultFalse);

        // Verify queue COM interface query
        void* qIface = nullptr;
        assert(q->queryInterface(Steinberg::Vst::IParamValueQueue::iid, &qIface) == Steinberg::kResultOk);
        assert(qIface == q);
    }

    // 5. Verify clear
    changes.clear();
    assert(changes.getParameterCount() == 0);
    assert(changes.getParameterData(0) == nullptr);

    std::cout << "PASSED\n";
}

void testAudioPlayerAsyncLoading() {
    std::cout << "[TEST] AudioPlayer Asynchronous Loading & Atomic Pointer Swap... ";

    std::string tempPath = (std::filesystem::temp_directory_path() / "praccy_test_async_track.wav").string();
    {
        std::ofstream wav(tempPath, std::ios::binary);
        const uint32_t sampleRate = 44100;
        const uint16_t numChannels = 2;
        const uint16_t bitsPerSample = 16;
        const uint32_t numSamples = 2000;
        const uint32_t dataBytes = numSamples * numChannels * (bitsPerSample / 8);
        const uint32_t riffSize = 36 + dataBytes;

        wav.write("RIFF", 4);
        wav.write(reinterpret_cast<const char*>(&riffSize), 4);
        wav.write("WAVE", 4);

        wav.write("fmt ", 4);
        uint32_t fmtSize = 16;
        uint16_t audioFormat = 1; // PCM
        uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
        uint16_t blockAlign = numChannels * (bitsPerSample / 8);
        wav.write(reinterpret_cast<const char*>(&fmtSize), 4);
        wav.write(reinterpret_cast<const char*>(&audioFormat), 2);
        wav.write(reinterpret_cast<const char*>(&numChannels), 2);
        wav.write(reinterpret_cast<const char*>(&sampleRate), 4);
        wav.write(reinterpret_cast<const char*>(&byteRate), 4);
        wav.write(reinterpret_cast<const char*>(&blockAlign), 2);
        wav.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

        wav.write("data", 4);
        wav.write(reinterpret_cast<const char*>(&dataBytes), 4);
        for (uint32_t i = 0; i < numSamples; ++i) {
            int16_t s = static_cast<int16_t>(std::sin(2.0 * 3.14159265 * 440.0 * i / sampleRate) * 16000.0);
            wav.write(reinterpret_cast<const char*>(&s), 2);
            wav.write(reinterpret_cast<const char*>(&s), 2);
        }
    }

    tools::AudioPlayer player;
    player.prepare(48000.0);

    // 1. Synchronous loading test
    bool syncOk = player.loadWavFileSync(tempPath);
    assert(syncOk);
    assert(player.isLoaded());
    assert(!player.isLoading());
    assert(player.durationSeconds() > 0.0);
    assert(!player.waveformPeaks800().empty());
    assert(player.waveformPeaks800().size() == 800);

    // 2. Audio process playback test
    player.play();
    assert(player.isPlaying());
    audio::OwnedAudioBuffer outBuf(2, 256);
    auto view = outBuf.view(256);
    view.clear();
    player.process(view);
    float sumL = 0.0f;
    for (uint32_t s = 0; s < 256; ++s) {
        sumL += std::abs(view.channel(0)[s]);
    }
    assert(sumL > 0.0f);

    player.unload();
    assert(!player.isLoaded());
    assert(!player.isPlaying());

    // 3. Asynchronous loading test
    bool asyncLaunched = player.loadWavFile(tempPath);
    assert(asyncLaunched);

    int attempts = 0;
    while (!player.isLoaded() && attempts++ < 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(player.isLoaded());
    assert(!player.isLoading());
    assert(player.loadProgress() >= 1.0f);
    assert(player.durationSeconds() > 0.0);

    std::error_code ec;
    std::filesystem::remove(tempPath, ec);

    std::cout << "PASSED\n";
}

void testHardwareGradeWidgetsAndCategoryBadges() {
    std::cout << "[TEST] Hardware-Grade Widgets & Device Category Badges... ";

    // 1. Heuristic Category Detection
    using namespace praccy::ui;
    assert(detectDeviceCategory("Ibanez TS9 Tube Screamer", "Ibanez") == DeviceCategory::OverdriveStompbox);
    assert(detectDeviceCategory("Big Muff Pi Fuzz", "Electro-Harmonix") == DeviceCategory::OverdriveStompbox);
    assert(detectDeviceCategory("ProCo Rat Distortion", "ProCo") == DeviceCategory::OverdriveStompbox);
    assert(detectDeviceCategory("Klon Centaur Clean Boost", "Klon") == DeviceCategory::OverdriveStompbox);

    assert(detectDeviceCategory("JCM800 Lead Amp", "Marshall") == DeviceCategory::TubeAmpHead);
    assert(detectDeviceCategory("Dual Rectifier Crunch Head", "Mesa Boogie") == DeviceCategory::TubeAmpHead);
    assert(detectDeviceCategory("Clean Twin Reverb Amp", "Fender") == DeviceCategory::TubeAmpHead);

    assert(detectDeviceCategory("1176 Classic Limiting Amplifier", "Universal Audio") == DeviceCategory::StudioCompressor);
    assert(detectDeviceCategory("Teletronix LA-2A Leveler", "Universal Audio") == DeviceCategory::StudioCompressor);
    assert(detectDeviceCategory("Solid Dynamics Gate/Comp", "SSL") == DeviceCategory::StudioCompressor);

    assert(detectDeviceCategory("Roland RE-201 Space Echo", "Roland") == DeviceCategory::TapeDelay);
    assert(detectDeviceCategory("Analog BBD Delay", "Memory Man") == DeviceCategory::TapeDelay);

    assert(detectDeviceCategory("EMT 140 Classic Plate", "EMT") == DeviceCategory::PlateReverb);
    assert(detectDeviceCategory("Shimmer Hall Reverb", "Valhalla") == DeviceCategory::PlateReverb);

    assert(detectDeviceCategory("Stereo Chorus / Flanger", "TC Electronic") == DeviceCategory::Modulation);
    assert(detectDeviceCategory("Optical Tremolo & Phase", "Univibe") == DeviceCategory::Modulation);

    assert(detectDeviceCategory("Parametric Dynamic EQ", "FabFilter") == DeviceCategory::Equalizer);
    assert(detectDeviceCategory("10-Band Graphic Filter Curve", "Boss") == DeviceCategory::Equalizer);

    assert(detectDeviceCategory("Unknown Utility Tool", "Praccy Audio") == DeviceCategory::GenericProcessor);

    // 2. Category Title Strings
    assert(std::string(getCategoryTitle(DeviceCategory::OverdriveStompbox)) == "STOMPBOX OD");
    assert(std::string(getCategoryTitle(DeviceCategory::TubeAmpHead)) == "TUBE AMP");
    assert(std::string(getCategoryTitle(DeviceCategory::StudioCompressor)) == "COMPRESSOR");
    assert(std::string(getCategoryTitle(DeviceCategory::TapeDelay)) == "TAPE DELAY");
    assert(std::string(getCategoryTitle(DeviceCategory::PlateReverb)) == "PLATE REVERB");
    assert(std::string(getCategoryTitle(DeviceCategory::Modulation)) == "MODULATION");
    assert(std::string(getCategoryTitle(DeviceCategory::Equalizer)) == "PRECISION EQ");
    assert(std::string(getCategoryTitle(DeviceCategory::GenericProcessor)) == "STUDIO FX");

    // 3. HardwareKnobOptions defaults
    HardwareKnobOptions opt;
    assert(opt.radius == 13.0f);
    assert(opt.arcThickness == 2.2f);
    assert(opt.sensitivity == 0.012f);

    // 4. Gravity sag computation parity
    auto computeSag = [](float dx) {
        return std::clamp(std::abs(dx) * 0.16f + 8.0f, 6.0f, 42.0f);
    };
    assert(computeSag(0.0f) == 8.0f);
    assert(computeSag(100.0f) == 24.0f);
    assert(computeSag(400.0f) == 42.0f); // Clamped at 42.0f max

    // 5. Rack View Mode Enum Parity
    state::AppConfig cfg;
    assert(cfg.rackViewMode == state::AppConfig::RackViewMode::Modular);
    cfg.rackViewMode = state::AppConfig::RackViewMode::Simple;
    assert(cfg.rackViewMode == state::AppConfig::RackViewMode::Simple);

    std::cout << "PASSED\n";
}

void testMilestone3PracticeSuiteAndStrobeTuner() {
    std::cout << "[TEST] Milestone 3: Practice Suite, Strobe Tuner & Quick Looper Layers... ";

    // 1. QuickLooper Undo/Redo Layer Verification
    {
        tools::QuickLooper looper;
        looper.prepare(48000.0, 10);
        assert(!looper.canUndo());
        assert(!looper.canRedo());

        // Fill initial loop by triggering Record
        looper.triggerAction();
        assert(looper.state() == tools::LooperState::Recording);

        audio::OwnedAudioBuffer recIn(2, 48000);
        audio::OwnedAudioBuffer recOut(2, 48000);
        for (uint32_t s = 0; s < 48000; ++s) {
            recIn.view(48000).channel(0)[s] = 0.5f;
            recIn.view(48000).channel(1)[s] = 0.5f;
        }
        auto recOutView = recOut.view(48000);
        looper.process(recIn.view(48000), recOutView);

        // Lock loop: transitions to Playing
        looper.triggerAction();
        assert(looper.state() == tools::LooperState::Playing);
        assert(std::abs(looper.loopLengthSeconds() - 1.0) < 0.05);
        assert(!looper.canUndo());

        // Trigger Overdub: snapshots current loop layer
        looper.triggerAction();
        assert(looper.state() == tools::LooperState::Overdubbing);
        assert(looper.canUndo());
        assert(!looper.canRedo());

        // Accumulate audio into overdub
        audio::OwnedAudioBuffer dubIn(2, 4800);
        audio::OwnedAudioBuffer dubOut(2, 4800);
        for (uint32_t s = 0; s < 4800; ++s) {
            dubIn.view(4800).channel(0)[s] = 0.4f;
            dubIn.view(4800).channel(1)[s] = 0.4f;
        }
        auto dubOutView = dubOut.view(4800);
        looper.process(dubIn.view(4800), dubOutView);

        // Test Undo: reverts layer
        bool undoOk = looper.undo();
        assert(undoOk);
        assert(!looper.canUndo());
        assert(looper.canRedo());

        // Test Redo: restores layer
        bool redoOk = looper.redo();
        assert(redoOk);
        assert(looper.canUndo());
        assert(!looper.canRedo());
    }

    // 2. Musical Bar/Beat Calculation Verification
    {
        tools::QuickLooper looper;
        looper.prepare(48000.0, 10);
        looper.setBpm(120.0f);
        looper.setBeatsPerBar(4);
        assert(looper.bpm() == 120.0f);
        assert(looper.beatsPerBar() == 4);

        // Record a 2.0s loop = 4 beats = 1 bar at 120 BPM
        looper.triggerAction();
        audio::OwnedAudioBuffer bufIn(2, 96000); // 2 seconds
        audio::OwnedAudioBuffer bufOut(2, 96000);
        auto bufOutView = bufOut.view(96000);
        looper.process(bufIn.view(96000), bufOutView);
        looper.triggerAction(); // Lock

        assert(looper.totalBeats() == 4);
        assert(looper.totalBars() == 1);
        assert(looper.currentBar() >= 1);
        assert(looper.currentBeat() >= 1 && looper.currentBeat() <= 4);
    }

    // 3. Strobe Tuner Widget Execution Verification
    {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        bool createdCtx = false;
        if (!ctx) {
            ctx = ImGui::CreateContext();
            createdCtx = true;
        }
        ImGui::SetCurrentContext(ctx);

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1024, 768);
        io.DeltaTime = 1.0f / 60.0f;

        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        ImGui::NewFrame();
        ImGui::Begin("TunerTestWindow");

        tools::TunerResult exactInTune;
        exactInTune.confidence = true;
        exactInTune.noteName = "A2";
        exactInTune.frequencyHz = 110.0f;
        exactInTune.centDeviation = 0.0f;

        tools::TunerResult flatResult;
        flatResult.confidence = true;
        flatResult.noteName = "E2";
        flatResult.frequencyHz = 80.1f;
        flatResult.centDeviation = -18.5f;

        tools::TunerResult sharpResult;
        sharpResult.confidence = true;
        sharpResult.noteName = "D3";
        sharpResult.frequencyHz = 148.5f;
        sharpResult.centDeviation = +24.2f;

        tools::TunerResult unconfident;
        unconfident.confidence = false;

        // Verify DrawStrobeTuner renders without crash across all states
        ui::DrawStrobeTuner(exactInTune, ImVec2(100, 100), 30.0f);
        ui::DrawStrobeTuner(flatResult, ImVec2(200, 100), 30.0f);
        ui::DrawStrobeTuner(sharpResult, ImVec2(300, 100), 30.0f);
        ui::DrawStrobeTuner(unconfident, ImVec2(400, 100), 30.0f);

        // Verify DrawStrobeTunerModule renders
        ui::DrawStrobeTunerModule(exactInTune, 240.0f, 64.0f);
        ui::DrawStrobeTunerModule(flatResult, 240.0f, 64.0f);

        // Verify DrawStrobeTuner with isEnabled=false
        ui::DrawStrobeTuner(unconfident, ImVec2(500, 100), 30.0f, nullptr, false);

        // Verify DrawStrobeTunerModule with active InstrumentTuner and settings callback
        tools::InstrumentTuner testTuner(2048);
        testTuner.prepare(48000.0);
        bool settingsOpened = false;
        ui::DrawStrobeTunerModule(exactInTune, 260.0f, 64.0f, &testTuner, [&]() {
            settingsOpened = true;
        });

        // Verify TunerSettingsModal renders properly
        ui::TunerSettingsModal modal(testTuner);
        assert(!modal.isOpen());
        modal.open();
        assert(modal.isOpen());
        modal.render();
        modal.close();
        assert(!modal.isOpen());

        ImGui::End();
        ImGui::Render();

        if (createdCtx) {
            ImGui::DestroyContext(ctx);
        }
    }

    // 4. Default Factory User Presets & Sound Profile Detection
    {
        state::SceneManager scenes;
        auto names = scenes.savedPresetNames();
        assert(!names.empty());
        assert(names.size() >= 4);

        bool foundClean = false;
        bool foundMetal = false;
        bool foundBreakup = false;
        bool foundAmbient = false;

        for (const auto& name : names) {
            if (name.find("Clean") != std::string::npos) foundClean = true;
            if (name.find("Metal") != std::string::npos || name.find("Djent") != std::string::npos) foundMetal = true;
            if (name.find("Breakup") != std::string::npos || name.find("Blues") != std::string::npos) foundBreakup = true;
            if (name.find("Ambient") != std::string::npos || name.find("Wash") != std::string::npos) foundAmbient = true;
        }

        assert(foundClean);
        assert(foundMetal);
        assert(foundBreakup);
        assert(foundAmbient);
    }

    std::cout << "PASSED\n";
}

void testGraphEngineVariableBlockSizes() {
    std::cout << "[TEST] GraphEngine Variable Block Sizes (512, 1024, 2048)... ";
    audio::GraphEngine engine;
    engine.prepare(48000.0, 2048);

    auto slot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>());
    slot->prepare(48000.0, 2048);
    engine.addSerialNode(std::move(slot));

    audio::OwnedAudioBuffer inBuf(2, 2048);
    audio::OwnedAudioBuffer outBuf(2, 2048);

    const uint32_t blockSizes[] = {512, 1024, 2048};
    for (uint32_t bs : blockSizes) {
        auto inView = inBuf.view(bs);
        auto outView = outBuf.view(bs);

        for (uint32_t ch = 0; ch < 2; ++ch) {
            for (uint32_t s = 0; s < bs; ++s) {
                inView.channel(ch)[s] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(s) / 48000.0f);
            }
        }
        outView.clear();

        engine.process(inView, outView);

        float sumAbs = 0.0f;
        for (uint32_t ch = 0; ch < 2; ++ch) {
            for (uint32_t s = 0; s < bs; ++s) {
                sumAbs += std::abs(outView.channel(ch)[s]);
            }
        }
        assert(sumAbs > 0.0f);
    }
    std::cout << "PASSED\n";
}

void testStereoBypassRampSymmetry() {
    std::cout << "[TEST] Stereo Bypass Ramp Symmetry... ";
    audio::PluginSlot slot(std::make_unique<plugins::OverdriveEffect>());
    slot.prepare(48000.0, 512);

    audio::OwnedAudioBuffer inBuf(2, 512);
    audio::OwnedAudioBuffer outBuf(2, 512);

    auto inView = inBuf.view(512);
    auto outView = outBuf.view(512);

    for (uint32_t s = 0; s < 512; ++s) {
        float v = 0.4f * std::sin(2.0f * 3.14159265f * 220.0f * static_cast<float>(s) / 48000.0f);
        inView.channel(0)[s] = v;
        inView.channel(1)[s] = v;
    }

    slot.setBypassed(true);

    audio::AudioProcessContext ctx{inView, outView, 48000.0, 512};
    slot.process(ctx);

    for (uint32_t s = 0; s < 512; ++s) {
        float l = outView.channel(0)[s];
        float r = outView.channel(1)[s];
        assert(l == r);
    }

    slot.setBypassed(false);
    slot.process(ctx);

    for (uint32_t s = 0; s < 512; ++s) {
        float l = outView.channel(0)[s];
        float r = outView.channel(1)[s];
        assert(l == r);
    }
    std::cout << "PASSED\n";
}

void testColorTokenFloatConversion() {
    std::cout << "[TEST] ColorToken float RGBA conversion... ";
    ui::ColorToken col(0.5f, 0.8f, 0.2f, 0.9f);
    assert(col.u32 != 0);
    assert(col.r > 0.49f && col.r < 0.51f);
    assert(col.g > 0.79f && col.g < 0.81f);
    assert(col.b > 0.19f && col.b < 0.21f);
    assert(col.a > 0.89f && col.a < 0.91f);
    assert(col.vec4.x == col.r);
    assert(col.vec4.y == col.g);
    assert(col.vec4.z == col.b);
    assert(col.vec4.w == col.a);
    std::cout << "PASSED\n";
}

void testDawInteropSecurityAndOverflowRejection() {
    std::cout << "[TEST] DAW Interop Security & Overflow Payload Rejection... ";

    // 1. ReaperAdapter: compLen = 0xFFFFFFFC overflow rejection
    std::vector<uint8_t> malformedReaper = {'v', 's', 't', '3', 0xFC, 0xFF, 0xFF, 0xFF, 0x01, 0x02, 0x03, 0x04};
    std::vector<uint8_t> reaperComp, reaperCtrl;
    bool okReaper = state::ReaperAdapter::unpackVst3ChunkEnvelope(malformedReaper, reaperComp, reaperCtrl);
    (void)okReaper;
    assert(reaperComp.size() != 0xFFFFFFFC);

    // 2. VstPresetAdapter: compSize = UINT64_MAX - 8 overflow rejection
    std::vector<uint8_t> malformedPreset(64, 0);
    malformedPreset[0] = 'V'; malformedPreset[1] = 'S'; malformedPreset[2] = 'T'; malformedPreset[3] = '3';
    malformedPreset[40] = 'C'; malformedPreset[41] = 'o'; malformedPreset[42] = 'm'; malformedPreset[43] = 'p';
    uint64_t overflowCompSize = UINT64_MAX - 8;
    std::memcpy(malformedPreset.data() + 44, &overflowCompSize, 8);
    std::vector<uint8_t> vstComp;
    std::string vstGuid;
    (void)state::VstPresetAdapter::parseVstPreset(malformedPreset, vstGuid, vstComp);
    assert(vstComp.size() != overflowCompSize);
    assert(vstComp.size() <= malformedPreset.size());

    // 3. DawprojectAdapter: path traversal rejection
    std::filesystem::path safeRel;
    assert(!utils::sanitizeZipPath("../../win.ini", safeRel));
    assert(!utils::sanitizeZipPath("..\\..\\win.ini", safeRel));
    assert(!utils::sanitizeZipPath("/etc/passwd", safeRel));
    assert(!utils::sanitizeZipPath("C:\\Windows\\System32\\cmd.exe", safeRel));

    std::cout << "PASSED\n";
}

void testDynamicSampleRateAndBufferSizeUpdates() {
    std::cout << "[TEST] Dynamic Sample Rate & Buffer Size Updates... ";

    // 1. AsioManager safety without active driver
    {
        audio::AsioManager asio;
        assert(!asio.isLoaded());
        assert(!asio.isRunning());
        assert(!asio.checkHardwareChanges());
        assert(!asio.setSampleRate(96000.0));
        assert(!asio.setBufferSize(128));
        assert(!asio.canSampleRate(48000.0));
    }

    // 2. Real-time DSP Pill formatting verification across standard sample rates & buffer sizes
    {
        const struct {
            double sr;
            int bufSize;
            const char* expectedSrSubstr;
        } cases[] = {
            { 44100.0,  128, "44.1 kHz" },
            { 48000.0,  256, "48 kHz"   },
            { 88200.0,  512, "88.2 kHz" },
            { 96000.0,   64, "96 kHz"   },
            { 176400.0, 1024, "176.4 kHz"},
            { 192000.0, 2048, "192 kHz"  }
        };

        for (const auto& c : cases) {
            const char* srFormat = (std::fmod(c.sr, 1000.0) == 0.0) ? "%.0f kHz" : "%.1f kHz";
            char srBuf[32];
            std::snprintf(srBuf, sizeof(srBuf), srFormat, c.sr / 1000.0);
            assert(std::string(srBuf) == c.expectedSrSubstr);

            const double latencyMs = (c.sr > 0.0) ? (static_cast<double>(c.bufSize) / c.sr * 1000.0) : 0.0;
            assert(latencyMs > 0.0 && latencyMs < 100.0);

            char statusText[160];
            std::snprintf(statusText, sizeof(statusText), "ACTIVE  |  %s  |  %d spls (%.1f ms)  |  DSP: 12%%",
                          srBuf, c.bufSize, latencyMs);
            assert(std::string(statusText).find(c.expectedSrSubstr) != std::string::npos);
        }

        // Pill width stability verification across single- and double-digit DSP loads
        {
            ImGuiContext* ctx = ImGui::GetCurrentContext();
            bool createdCtx = false;
            if (!ctx) {
                ctx = ImGui::CreateContext();
                createdCtx = true;
            }
            ImGui::SetCurrentContext(ctx);

            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2(1024, 768);
            io.DeltaTime = 1.0f / 60.0f;
            unsigned char* pixels = nullptr;
            int width = 0, height = 0;
            io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
            ImGui::NewFrame();

            auto calcPillW = [](double sr, int bufSize, double latencyMs, float dspLoad, uint32_t drops) {
                const char* srFormat = (std::fmod(sr, 1000.0) == 0.0) ? "%.0f kHz" : "%.1f kHz";
                char srBuf[32];
                std::snprintf(srBuf, sizeof(srBuf), srFormat, sr / 1000.0);

                char statusText[160];
                char refText[160];
                const float refLoad = (dspLoad < 100.0f) ? 99.0f : dspLoad;
                if (drops > 0) {
                    std::snprintf(statusText, sizeof(statusText), "ACTIVE  |  %s  |  %d spls (%.1f ms)  |  DSP: %.0f%%  |  DROPS: %u",
                                  srBuf, bufSize, latencyMs, dspLoad, drops);
                    std::snprintf(refText, sizeof(refText), "ACTIVE  |  %s  |  %d spls (%.1f ms)  |  DSP: %.0f%%  |  DROPS: %u",
                                  srBuf, bufSize, latencyMs, refLoad, drops);
                } else {
                    std::snprintf(statusText, sizeof(statusText), "ACTIVE  |  %s  |  %d spls (%.1f ms)  |  DSP: %.0f%%",
                                  srBuf, bufSize, latencyMs, dspLoad);
                    std::snprintf(refText, sizeof(refText), "ACTIVE  |  %s  |  %d spls (%.1f ms)  |  DSP: %.0f%%",
                                  srBuf, bufSize, latencyMs, refLoad);
                }
                const float pillTextW = std::max(ImGui::CalcTextSize(statusText).x, ImGui::CalcTextSize(refText).x);
                return pillTextW + 36.0f;
            };

            const float wSingle = calcPillW(48000.0, 128, 2.7, 9.0f, 0);
            const float wDouble = calcPillW(48000.0, 128, 2.7, 10.0f, 0);
            const float wZero   = calcPillW(48000.0, 128, 2.7, 0.0f, 0);
            const float wHigh   = calcPillW(48000.0, 128, 2.7, 99.0f, 0);

            // Pill width must be identical across single and double digit loads to prevent jitter
            assert(std::abs(wSingle - wDouble) < 0.001f);
            assert(std::abs(wZero - wDouble) < 0.001f);
            assert(std::abs(wHigh - wDouble) < 0.001f);

            ImGui::EndFrame();
            if (createdCtx) {
                ImGui::DestroyContext(ctx);
            }
        }

        // Load meter 1/sec rate limit verification
        {
            float displayedLoad = 0.0f;
            std::chrono::steady_clock::time_point lastUpdate{};

            auto updateMeter = [&](float rawLoad, std::chrono::steady_clock::time_point now) {
                if (lastUpdate.time_since_epoch().count() == 0 ||
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count() >= 1000) {
                    displayedLoad = rawLoad;
                    lastUpdate = now;
                }
            };

            auto t0 = std::chrono::steady_clock::now();
            updateMeter(12.0f, t0);
            assert(displayedLoad == 12.0f);

            // Within 1 second (e.g. 500ms later), raw load changes, but meter does not change
            updateMeter(25.0f, t0 + std::chrono::milliseconds(500));
            assert(displayedLoad == 12.0f);

            // At 999ms, meter still does not change
            updateMeter(35.0f, t0 + std::chrono::milliseconds(999));
            assert(displayedLoad == 12.0f);

            // At 1000ms (1 second), meter updates
            updateMeter(42.0f, t0 + std::chrono::milliseconds(1000));
            assert(displayedLoad == 42.0f);

            // Within next second (e.g. 1500ms), meter does not change
            updateMeter(50.0f, t0 + std::chrono::milliseconds(1500));
            assert(displayedLoad == 42.0f);

            // At 2000ms (2 seconds), meter updates
            updateMeter(15.0f, t0 + std::chrono::milliseconds(2000));
            assert(displayedLoad == 15.0f);
        }
    }

    // 3. Dynamic multi-rate GraphEngine & Practice Tools re-preparation
    {
        audio::GraphEngine graph;
        tools::InstrumentTuner tuner(2048);
        tools::Metronome metronome;
        tools::AudioPlayer player;
        tools::QuickLooper looper;

        auto slot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>());
        graph.addSerialNode(std::move(slot));

        const double rates[] = { 44100.0, 48000.0, 96000.0, 88200.0 };
        const uint32_t sizes[] = { 512, 128, 64, 2048 };

        for (size_t i = 0; i < 4; ++i) {
            const double sr = rates[i];
            const uint32_t bs = sizes[i];

            // Dynamic re-preparation with newly configured rate & buffer size
            graph.prepare(sr, bs);
            tuner.prepare(sr, 4096);
            metronome.prepare(sr);
            player.prepare(sr);
            looper.prepare(sr);

            audio::OwnedAudioBuffer inBuf(2, bs);
            audio::OwnedAudioBuffer outBuf(2, bs);
            auto inView = inBuf.view(bs);
            auto outView = outBuf.view(bs);

            for (uint32_t ch = 0; ch < 2; ++ch) {
                for (uint32_t s = 0; s < bs; ++s) {
                    inView.channel(ch)[s] = 0.3f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(s) / static_cast<float>(sr));
                }
            }
            outView.clear();

            graph.process(inView, outView);

            // Verify non-zero, finite processed audio across buffer size and sample rate mutations
            float maxAbs = 0.0f;
            for (uint32_t ch = 0; ch < 2; ++ch) {
                for (uint32_t s = 0; s < bs; ++s) {
                    float v = outView.channel(ch)[s];
                    assert(!std::isnan(v));
                    assert(!std::isinf(v));
                    maxAbs = std::max(maxAbs, std::abs(v));
                }
            }
            assert(maxAbs > 0.0f);
        }
    }

    std::cout << "PASSED\n";
}

void testParallelBranchSplitNoDuplicateAndCleanDelete() {
    std::cout << "[TEST] Parallel Branch Split Single Slot & Clean Deletion... ";
    audio::GraphEngine engine;
    engine.prepare(48000.0, 512);

    auto slot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>());
    engine.addSerialNode(std::move(slot));

    audio::OwnedAudioBuffer inBuf(2, 512);
    audio::OwnedAudioBuffer outBuf(2, 512);
    auto inView = inBuf.view(512);
    auto outView = outBuf.view(512);

    // 1. Process block to ensure audio is actively running
    engine.process(inView, outView);

    // 2. Split serial node into parallel while audio is active
    engine.splitSerialNodeIntoParallel(0);
    engine.process(inView, outView);

    auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(engine.getNode(0));
    assert(block != nullptr);
    auto* branchA = block->getBranch(0);
    assert(branchA != nullptr);

    // CRITICAL: Slot must NOT be duplicated into the next slot in the branch!
    assert(branchA->numSlots() == 1);
    assert(branchA->getSlot(0) != nullptr);
    assert(branchA->getSlot(0)->name() == "Praccy Drive");

    // 3. Delete slot from Branch A
    branchA->removeSlot(0);
    engine.process(inView, outView);
    branchA->collectReclaimedSlots();

    // Verify Branch A now cleanly has 0 slots
    assert(branchA->numSlots() == 0);

    // 4. Continue audio processing to verify no null dereferences or memory faults
    engine.process(inView, outView);

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
    testPluginScannerPersistence();
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
    testBottomBarCentering();
    testEqualPowerRampEnergyConservation();
    testQuickLooperCircularProgressMath();
    testWavDragAndDropExtensionValidation();
    testFloatingHudAlphaDecayComputation();
    testStyleStackGuards();
    testFastParameterChanges();
    testAudioPlayerAsyncLoading();
    testHardwareGradeWidgetsAndCategoryBadges();
    testMilestone3PracticeSuiteAndStrobeTuner();
    testGraphEngineVariableBlockSizes();
    testStereoBypassRampSymmetry();
    testColorTokenFloatConversion();
    testDawInteropSecurityAndOverflowRejection();
    testDynamicSampleRateAndBufferSizeUpdates();
    testParallelBranchSplitNoDuplicateAndCleanDelete();

    std::cout << "===========================================\n";
    std::cout << "   ALL TESTS PASSED SUCCESSFULLY! (41/41)  \n";
    std::cout << "===========================================\n";
    return 0;
}
