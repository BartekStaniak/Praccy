#include "rack_view.h"
#include "thumbnail_manager.h"
#include "update_checker.h"
#include "design_tokens.h"
#include "theme.h"
#include "ui_helpers.h"
#include "../version.h"
#include "../plugins/builtin_dsp.h"
#include "../plugins/clap_host.h"
#include "../plugins/vst3_host.h"
#include "../plugins/plugin_window.h"
#include "../plugins/async_plugin_loader.h"
#include "../state/app_config.h"
#include "../utils/wav_writer.h"
#include "widgets/rotary_knob.h"
#include "widgets/device_badges.h"
#include "widgets/strobe_tuner.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <regex>
#include <iostream>
#include <filesystem>
#include <ctime>

namespace praccy::ui {

static inline ImVec2 evaluateCubicBezier(ImVec2 p0, ImVec2 c0, ImVec2 c1, ImVec2 p1, float u) {
    const float u1 = 1.0f - u;
    const float w0 = u1 * u1 * u1;
    const float w1 = 3.0f * u1 * u1 * u;
    const float w2 = 3.0f * u1 * u * u;
    const float w3 = u * u * u;
    return ImVec2(
        w0 * p0.x + w1 * c0.x + w2 * c1.x + w3 * p1.x,
        w0 * p0.y + w1 * c0.y + w2 * c1.y + w3 * p1.y
    );
}

static void drawCubicHermiteCable(ImDrawList* dl, ImVec2 p0, ImVec2 p1,
                                 float signalPeak = 0.0f, bool isBypassed = false,
                                 float animTime = 0.0f) {
    const float dx = p1.x - p0.x;
    const float dy = p1.y - p0.y;

    // Distance-adaptive tangent magnitude with short-dx safety clamp
    float tMag = std::max(20.0f, 0.45f * dx + 0.25f * std::abs(dy));
    if (dx < 36.0f && dx > 0.0f) {
        tMag = std::min(tMag, std::max(10.0f, dx * 0.9f));
    }

    // Reduced gravity sag: clean taut curve without excessive droop
    const float sagFactor = (std::abs(dy) > 10.0f) ? std::max(0.0f, 1.0f - std::abs(dy) / 120.0f) : 1.0f;
    const float sag = std::clamp(std::abs(dx) * 0.04f + 1.5f, 1.0f, 8.0f) * sagFactor;

    // Hermite to Cubic Bezier control points draped under gravity
    const float inv3 = 1.0f / 3.0f;
    const ImVec2 c0(p0.x + tMag * inv3, p0.y + sag);
    const ImVec2 c1(p1.x - tMag * inv3, p1.y + sag);

    const auto& tokens = themeTokens();
    const ImU32 shadowCol = isBypassed ? static_cast<ImU32>(tokens.surfaces.cardShadow.withAlpha(15.0f / 255.0f).u32)
                                       : static_cast<ImU32>(tokens.surfaces.cardShadow.u32);
    const ImU32 sleeveCol = isBypassed ? static_cast<ImU32>(tokens.cables.bypassedSleeve) : static_cast<ImU32>(tokens.cables.sleeve);
    const ImU32 coreCol   = isBypassed ? static_cast<ImU32>(tokens.cables.bypassedCore)   : static_cast<ImU32>(tokens.cables.core);
    const ImU32 socketCol = static_cast<ImU32>(tokens.cables.socketRing);
    const ImU32 pinCol    = static_cast<ImU32>(tokens.cables.socketPin);

    const float safePeak = (!std::isfinite(signalPeak) || signalPeak < 0.0f) ? 0.0f : std::clamp(signalPeak, 0.0f, 1.0f);
    const float normPeak = safePeak;

    // Modulate sleeve and core thickness by live audio peak level
    const float sleeveThickness = isBypassed ? 3.0f : (3.5f + 1.5f * normPeak);
    const float coreThickness   = isBypassed ? 1.5f : (1.8f + 1.2f * normPeak);

    // Outer reactive glow for active cables
    if (!isBypassed && normPeak > 0.02f) {
        const float glowAlpha = std::clamp(normPeak * 80.0f, 0.0f, 110.0f);
        const ImU32 glowCol = tokens.signal.active.withAlpha(glowAlpha / 255.0f).u32;
        dl->AddBezierCubic(p0, c0, c1, p1, glowCol, sleeveThickness + 4.5f, 24);
    }

    // 1. Layer 1: Drop Shadow (+2.5px Y offset, alpha 48)
    const ImVec2 shOffset(0.0f, 2.5f);
    dl->AddBezierCubic(
        ImVec2(p0.x + shOffset.x, p0.y + shOffset.y),
        ImVec2(c0.x + shOffset.x, c0.y + shOffset.y),
        ImVec2(c1.x + shOffset.x, c1.y + shOffset.y),
        ImVec2(p1.x + shOffset.x, p1.y + shOffset.y),
        shadowCol, sleeveThickness + 2.0f, 24
    );

    // 2. Layer 2: Outer Sleeve (~3.5px gauge)
    dl->AddBezierCubic(p0, c0, c1, p1, sleeveCol, sleeveThickness, 24);

    // 3. Layer 3: Core Wire (~1.8px)
    dl->AddBezierCubic(p0, c0, c1, p1, coreCol, coreThickness, 24);

    // 4. Layer 4: Socket Endpoint Pins
    dl->AddCircleFilled(p0, 4.5f, socketCol);
    dl->AddCircleFilled(p0, 2.5f, pinCol);
    dl->AddCircleFilled(p1, 4.5f, socketCol);
    dl->AddCircleFilled(p1, 2.5f, pinCol);

    // 5. Layer 5: Audio-Reactive Signal Pulse Dots
    if (!isBypassed) {
        const float safeTime = (!std::isfinite(animTime)) ? 0.0f : animTime;
        const float dotSpeed = 0.75f;
        float u0 = std::fmod(safeTime * dotSpeed, 1.0f);
        if (u0 < 0.0f) {
            u0 += 1.0f;
        }

        const float baseR = 1.8f + 2.4f * std::sqrt(normPeak);
        const float alphaFactor = 0.35f + 0.65f * normPeak;

        auto drawDot = [&](float uVal) {
            ImVec2 dotPos = evaluateCubicBezier(p0, c0, c1, p1, uVal);

            // Halo glow
            const float glowA = std::clamp(56.0f * alphaFactor, 0.0f, 255.0f) / 255.0f;
            dl->AddCircleFilled(dotPos, baseR * 2.2f, tokens.cables.pulseGlow.withAlpha(glowA).u32);

            // Main dot body
            const float dotA = std::clamp(230.0f * alphaFactor, 0.0f, 255.0f) / 255.0f;
            dl->AddCircleFilled(dotPos, baseR, tokens.cables.pulseDot.withAlpha(dotA).u32);

            // Specular center
            dl->AddCircleFilled(dotPos, baseR * 0.45f, static_cast<ImU32>(tokens.text.primary));
        };

        drawDot(u0);

        if (dx > 100.0f) {
            const float u1 = std::fmod(u0 + 0.5f, 1.0f);
            drawDot(u1);
        }
    }
}

static void drawRoutingWire(ImDrawList* dl, ImVec2 p1, ImVec2 p2, bool /*withArrow*/ = false) {
    drawCubicHermiteCable(dl, p1, p2, 0.3f, false, static_cast<float>(ImGui::GetTime()));
}

static bool renderCenteredPlusButton(const char* id, const ImVec2& size = ImVec2(52, 52), float iconRadius = 11.0f) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();

    ImU32 bgCol;
    if (held)         bgCol = tokens.surfaces.buttonBgActive;
    else if (hovered) bgCol = tokens.surfaces.buttonBgHovered;
    else              bgCol = tokens.surfaces.buttonBg;

    ImU32 borderCol = hovered ? static_cast<ImU32>(tokens.borders.focus) : static_cast<ImU32>(tokens.borders.subtle);
    ImU32 iconCol   = hovered ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.secondary);

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bgCol, 8.0f);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), borderCol, 8.0f, 0, hovered ? 1.5f : 1.0f);

    float cx = std::floor(pos.x + size.x * 0.5f);
    float cy = std::floor(pos.y + size.y * 0.5f);

    const float halfBar = iconRadius;
    const float thick = 1.5f;
    dl->AddRectFilled(ImVec2(cx - halfBar, cy - thick), ImVec2(cx + halfBar, cy + thick), iconCol, 1.0f);
    dl->AddRectFilled(ImVec2(cx - thick, cy - halfBar), ImVec2(cx + thick, cy + halfBar), iconCol, 1.0f);

    return clicked;
}

RackView::RackView(audio::GraphEngine& graph,
                   audio::AsioManager& asio,
                   tools::InstrumentTuner& tuner,
                   tools::Metronome& metronome,
                   tools::AudioPlayer& player,
                   tools::QuickLooper& looper,
                   midi::MidiManager& midi,
                   state::SceneManager& scenes,
                   plugins::PluginScanner& scanner)
    : m_graph(graph),
      m_asio(asio),
      m_tuner(tuner),
      m_metronome(metronome),
      m_player(player),
      m_looper(looper),
      m_midi(midi),
      m_scenes(scenes),
      m_scanner(scanner),
      m_pluginBrowserModal(std::make_unique<PluginBrowserModal>(scanner, graph)),
      m_settingsModal(std::make_unique<SettingsModal>(asio, scanner, [this]() {
          if (m_settingsModal) m_settingsModal->close();
          m_pluginBrowserModal->open();
      })),
      m_practiceToolsModal(std::make_unique<PracticeToolsModal>(looper, player)),
      m_tunerSettingsModal(std::make_unique<TunerSettingsModal>(tuner)) {
    m_thumbnailMgr = &ThumbnailManager::instance();
    m_cachedUserPresetNames = m_scenes.savedPresetNames();
    m_userPresetsValid = true;

    state::AppConfig cfg;
    cfg.load();
    m_cachedConfig = cfg;

    // Restore Tuner Configuration
    m_tuner.setEnabled(cfg.tunerEnabled);
    m_tuner.setA4Reference(cfg.tunerA4Reference);
    {
        tools::TunerConfig tcfg;
        tcfg.instrument = static_cast<tools::InstrumentType>(std::clamp(cfg.tunerInstrument, 0, static_cast<int>(tools::InstrumentType::Count) - 1));
        tcfg.stringCount = cfg.tunerStringCount;
        tcfg.mode = static_cast<tools::TuningMode>(cfg.tunerTuningMode);
        tcfg.tuningType = static_cast<tools::TuningType>(std::clamp(cfg.tunerTuningType, 0, static_cast<int>(tools::TuningType::Count) - 1));
        tcfg.rootNoteIndex = cfg.tunerRootNote;
        tcfg.a4ReferenceHz = cfg.tunerA4Reference;
        tcfg.applyPreset();

        if (tcfg.mode == tools::TuningMode::Custom && !cfg.tunerCustomStrings.empty()) {
            std::stringstream ss(cfg.tunerCustomStrings);
            std::string item;
            std::vector<int> customNotes;
            while (std::getline(ss, item, ',')) {
                if (!item.empty()) {
                    try {
                        customNotes.push_back(std::stoi(item));
                    } catch (...) {}
                }
            }
            if (customNotes.size() == static_cast<size_t>(tcfg.stringCount)) {
                tcfg.stringMidiNotes = customNotes;
            }
        }
        m_tuner.setConfig(tcfg);
    }

    if (!cfg.presetFavoritesInitialized) {
        m_favoritePresets = {
            "Modern High Gain Djent",
            "California Glassy Clean",
            "Edge of Breakup Crunch",
            "Post-Rock Ambient FX"
        };
        cfg.presetFavoritesInitialized = true;
        cfg.favoritePresets = m_favoritePresets;
        cfg.save();
        m_cachedConfig = cfg;
    } else {
        m_favoritePresets = cfg.favoritePresets;
    }
    m_rackViewMode = state::AppConfig::RackViewMode::Modular;
    m_riffBuffer.prepare(48000.0, static_cast<int>(cfg.retroactiveBufferMinutes));

    if (!cfg.lastLoadedPreset.empty()) {
        if (m_scenes.loadPresetChain(cfg.lastLoadedPreset, m_graph, true /* allowAsync */)) {
            m_auditionedPreset = cfg.lastLoadedPreset;
        }
    }

    m_pluginBrowserModal->setOnPluginInserted([this](const std::string& pluginName) {
        syncMacrosFromGraph();
        m_undoManager.pushSnapshot(
            state::SceneManager::captureGraphSnapshot(m_graph, "Add " + pluginName),
            "Add " + pluginName,
            state::UndoActionType::StructuralMutation
        );
        triggerHudToast("ADDED: " + pluginName);
    });

    syncMacrosFromGraph();
    m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, m_auditionedPreset.empty() ? "Initial Scene" : "Preset: " + m_auditionedPreset),
                               m_auditionedPreset.empty() ? "Initial Scene" : "Preset: " + m_auditionedPreset,
                               state::UndoActionType::SceneSwitch);

    m_settingsModal->setOnDriverChanged([this](double sRate, uint32_t bufSize) {
        applyAudioSettings(sRate, bufSize);
    });
}

void RackView::applyAudioSettings(double sRate, uint32_t bufSize) {
    if (sRate <= 0.0) sRate = 48000.0;
    if (bufSize == 0) bufSize = 256;
    m_asio.stop();
    m_graph.setAudioRunning(false);
    if (m_asio.canSampleRate(sRate)) {
        m_asio.setSampleRate(sRate);
    }
    double actualSr = m_asio.currentSampleRate();
    if (actualSr <= 0.0) actualSr = sRate;
    m_graph.prepare(actualSr, bufSize);
    m_tuner.prepare(actualSr, 4096);
    m_metronome.prepare(actualSr);
    m_player.prepare(actualSr);
    m_looper.prepare(actualSr);
    m_riffBuffer.prepare(actualSr, static_cast<int>(m_cachedConfig.retroactiveBufferMinutes));
    m_asio.start(bufSize);
}

void RackView::recallScene(size_t index) {
    if (m_scenes.applyScene(static_cast<int>(index), m_graph, true /* allowAsync */)) {
        syncMacrosFromGraph();
        const auto* sc = m_scenes.getScene(index);
        std::string name = sc ? sc->name : ("Preset " + std::to_string(index + 1));
        triggerHudToast("SCENE: " + name);
        m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, name),
                                   name,
                                   state::UndoActionType::SceneSwitch);
    }
}

bool RackView::isPresetFavorite(const std::string& name) const {
    return std::find(m_favoritePresets.begin(), m_favoritePresets.end(), name) != m_favoritePresets.end();
}

void RackView::toggleFavoritePreset(const std::string& name) {
    auto it = std::find(m_favoritePresets.begin(), m_favoritePresets.end(), name);
    if (it != m_favoritePresets.end()) {
        m_favoritePresets.erase(it);
    } else {
        m_favoritePresets.push_back(name);
    }
    state::AppConfig cfg;
    cfg.load();
    cfg.presetFavoritesInitialized = true;
    cfg.favoritePresets = m_favoritePresets;
    cfg.save();
    m_cachedConfig = cfg;
}

bool RackView::loadPresetChain(const std::string& name) {
    if (m_scenes.loadPresetChain(name, m_graph, true /* allowAsync */)) {
        m_auditionedPreset = name;
        syncMacrosFromGraph();
        m_undoManager.pushSnapshot(
            state::SceneManager::captureGraphSnapshot(m_graph, "Preset: " + name),
            "Preset: " + name,
            state::UndoActionType::SceneSwitch
        );
        state::AppConfig cfg;
        cfg.load();
        cfg.lastLoadedPreset = name;
        cfg.save();
        m_cachedConfig = cfg;
        triggerHudToast("Loaded " + name);
        return true;
    }
    return false;
}

void RackView::render() {
    if (m_asio.checkHardwareChanges()) {
        applyAudioSettings(m_asio.currentSampleRate(), static_cast<uint32_t>(m_asio.currentBufferSize()));
    }

    if (m_config.enableLiveThumbnails && m_thumbnailMgr) {
        m_thumbnailMgr->update();
    }

    plugins::AsyncPluginLoader::instance().update([this](audio::PluginSlot* slot, std::unique_ptr<plugins::IPluginInstance> inst, bool openGuiWhenDone) {
        if (!slot || !inst) return;
        std::unique_ptr<audio::AudioNode> oldNode;
        plugins::IPluginInstance* instPtr = inst.get();
        std::string name = inst->name();
        slot->replaceInnerNode(std::move(inst), oldNode);

        // Reinitialize macros for newly available parameters
        initMacrosForSlot(slot);

        triggerHudToast("LOADED: " + name);

        // Option 2.A: Automatically open GUI window if requested
        if (openGuiWhenDone && instPtr->hasCustomGui()) {
            refreshGuiParamBaseline(slot);
            plugins::PluginWindowManager::instance().openPluginWindow(instPtr);
            HWND hWin = plugins::PluginWindowManager::instance().getWindow(instPtr);
            if (hWin && m_config.enableLiveThumbnails && m_thumbnailMgr) {
                m_thumbnailMgr->captureWindowAsync(name, hWin, 30);
            }
        }
    });

    pollPluginGuiEdits(false);

    if (m_expandPulseTimer > 0.0f) {
        m_expandPulseTimer -= ImGui::GetIO().DeltaTime * 2.8f;
        if (m_expandPulseTimer <= 0.0f) {
            m_expandPulseTimer = 0.0f;
            m_expandedSlot = nullptr;
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus |
                                  ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse;

    const auto& mainTokens = themeTokens();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, mainTokens.surfaces.windowBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, mainTokens.borders.subtle.vec4);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
    if (ImGui::Begin("PraccyMainWindow", nullptr, windowFlags)) {
        renderPracticeRibbon();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        renderSceneBar();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        renderSignalRack();
        renderBottomBar();

        // Keyboard shortcuts - strictly guarded by !io.WantTextInput
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            // 1. Undo / Redo: Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z
            if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                pollPluginGuiEdits(true);
                if (m_undoManager.undoCount() > 1) {
                    m_undoManager.undo(m_graph, m_scenes);
                    syncMacrosFromGraph();
                    triggerHudToast("UNDO: " + m_undoManager.lastActionDescription());
                } else {
                    triggerHudToast("NOTHING TO UNDO");
                }
            } else if ((io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) ||
                       (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))) {
                pollPluginGuiEdits(true);
                if (m_undoManager.canRedo()) {
                    m_undoManager.redo(m_graph, m_scenes);
                    syncMacrosFromGraph();
                    triggerHudToast("REDO: " + m_undoManager.lastActionDescription());
                } else {
                    triggerHudToast("NOTHING TO REDO");
                }
            }

            // 2. Global Map Mode: Ctrl+M, Ctrl+K
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M, false)) {
                m_midiMapMode = !m_midiMapMode;
                if (m_midiMapMode) {
                    m_keyMapMode = false;
                    m_awaitingKeyTarget.active = false;
                }
                triggerHudToast(m_midiMapMode ? "MIDI MAP MODE: ACTIVE" : "MIDI MAP MODE: OFF");
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) {
                m_keyMapMode = !m_keyMapMode;
                if (m_keyMapMode) {
                    m_midiMapMode = false;
                } else {
                    m_awaitingKeyTarget.active = false;
                }
                triggerHudToast(m_keyMapMode ? "KEY MAP MODE: ACTIVE" : "KEY MAP MODE: OFF");
            }

            if (m_keyMapMode && m_awaitingKeyTarget.active) {
                processKeyMapCapture();
            } else if (!m_keyMapMode) {
                dispatchKeyBindings();
            }

            // 3. Retroactive Riff capture: Ctrl+Shift+R
            if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_R, false)) {
                saveLastRiff();
            }

            // 4. Spotlight palette: Ctrl+P
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_P, false)) {
                m_pluginBrowserModal->open();
            }

            // 5. Export DAW Chain: Ctrl+E
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_E, false)) {
                m_exportDawModal.open();
            }

            auto isKeyCustomBound = [this](ImGuiKey key) -> bool {
                for (const auto& kb : m_config.keyBindings) {
                    if (kb.key == static_cast<int>(key)) return true;
                }
                return false;
            };

            // 6. Presets 1-8 (suppressed while Key Map mode is capturing or when key is bound to a custom target)
            if (!m_keyMapMode) {
                for (int k = 0; k < 8 && k < static_cast<int>(m_scenes.numScenes()); ++k) {
                    const auto presetKey = static_cast<ImGuiKey>(ImGuiKey_1 + k);
                    if (!isKeyCustomBound(presetKey) && ImGui::IsKeyPressed(presetKey, false)) {
                        if (m_scenes.applyScene(k, m_graph, true /* allowAsync */)) {
                            syncMacrosFromGraph();
                            triggerHudToast("PRESET " + std::to_string(k + 1) + " RECALLED");
                            m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Recall Preset " + std::to_string(k + 1)),
                                                       "Preset " + std::to_string(k + 1),
                                                       state::UndoActionType::SceneSwitch);
                        }
                    }
                }
            }

            // 7. Focused Card Shortcuts: [, ], Q, 0
            if (!m_focusedSlot && m_focusedSlotIndex < 0 && m_graph.numNodes() > 0) {
                for (size_t n = 0; n < m_graph.numNodes(); ++n) {
                    auto* node = m_graph.getNode(n);
                    if (node && node->type() == audio::NodeType::Plugin) {
                        m_focusedSlot = dynamic_cast<audio::PluginSlot*>(node);
                        m_focusedSlotIndex = static_cast<int>(n);
                        break;
                    }
                }
            }

            if (!m_keyMapMode && (m_focusedSlot || m_focusedSlotIndex >= 0)) {
                if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) {
                    cycleAdjacentScenePreset(-1);
                } else if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) {
                    cycleAdjacentScenePreset(+1);
                }

                if (!isKeyCustomBound(ImGuiKey_Q) && ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
                    int blockIdx = -1;
                    int branchIdx = -1;
                    if (m_focusedBlockIndex >= 0) {
                        blockIdx = m_focusedBlockIndex;
                        branchIdx = (m_focusedBranchIndex >= 0) ? m_focusedBranchIndex : 0;
                    } else if (m_focusedSlotIndex >= 0 && static_cast<size_t>(m_focusedSlotIndex) < m_graph.numNodes()) {
                        auto* node = m_graph.getNode(static_cast<size_t>(m_focusedSlotIndex));
                        if (dynamic_cast<audio::ParallelSplitMergeBlock*>(node)) {
                            blockIdx = m_focusedSlotIndex;
                            branchIdx = (m_focusedBranchIndex >= 0) ? m_focusedBranchIndex : 0;
                        }
                    }
                    if (blockIdx >= 0) {
                        m_pluginBrowserModal->open(blockIdx, branchIdx);
                    } else {
                        m_pluginBrowserModal->open();
                    }
                }

                if (m_focusedSlot && !isKeyCustomBound(ImGuiKey_0) && ImGui::IsKeyPressed(ImGuiKey_0, false)) {
                    m_focusedSlot->setBypassed(!m_focusedSlot->isBypassed());
                    triggerHudToast(m_focusedSlot->isBypassed() ? ("BYPASS: " + m_focusedSlot->name())
                                                                : ("ACTIVE: " + m_focusedSlot->name()));
                    m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Bypass " + m_focusedSlot->name()),
                                               "Bypass: " + m_focusedSlot->name(),
                                               state::UndoActionType::ParameterTweak,
                                               m_focusedSlot->name() + ":Bypass",
                                               true);
                    m_undoManager.commitActiveTransaction();
                }
            }
        }

        // Global Map Mode Pulsating Violet Canvas Border
        if (m_midiMapMode || m_keyMapMode) {
            const auto& tokens = themeTokens();
            const ImGuiViewport* vp = ImGui::GetMainViewport();
            if (vp) {
                float t = static_cast<float>(ImGui::GetTime());
                float alpha = 0.45f + 0.45f * std::sin(4.0f * t);
                ImU32 violet = static_cast<ImU32>(tokens.borders.mapModeViolet.withAlpha(alpha));

                ImDrawList* fg = ImGui::GetForegroundDrawList();
                fg->AddRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), violet, 0.0f, 0, 4.0f);

                char bannerBuf[128];
                const char* mapBanner = nullptr;
                if (m_midiMapMode) {
                    mapBanner = "● MIDI MAP MODE ACTIVE — CLICK ANY CONTROL TO LEARN MIDI CC";
                } else if (m_awaitingKeyTarget.active) {
                    std::snprintf(bannerBuf, sizeof(bannerBuf), "● KEY MAP: PRESS KEY FOR [%s] (ESC TO CANCEL)", m_awaitingKeyTarget.targetLabel.c_str());
                    mapBanner = bannerBuf;
                } else {
                    mapBanner = "● KEY MAP MODE ACTIVE — CLICK ANY CONTROL TO ASSIGN KEY";
                }
                ImVec2 bSz = ImGui::CalcTextSize(mapBanner);
                ImVec2 bPos(vp->Pos.x + (vp->Size.x - bSz.x) * 0.5f, vp->Pos.y + 10.0f);
                fg->AddRectFilled(ImVec2(bPos.x - 12.0f, bPos.y - 4.0f), ImVec2(bPos.x + bSz.x + 12.0f, bPos.y + bSz.y + 4.0f),
                                  tokens.surfaces.popupBg, 4.0f);
                fg->AddRect(ImVec2(bPos.x - 12.0f, bPos.y - 4.0f), ImVec2(bPos.x + bSz.x + 12.0f, bPos.y + bSz.y + 4.0f),
                            violet, 4.0f, 0, 1.5f);
                fg->AddText(bPos, tokens.text.accent.u32, mapBanner);
            }
        }

        if (m_pluginBrowserModal && m_pluginBrowserModal->isOpen()) {
            ImDrawList* winDl = ImGui::GetWindowDrawList();
            const ImGuiViewport* mainVp = ImGui::GetMainViewport();
            if (mainVp) {
                winDl->AddRectFilled(mainVp->Pos, ImVec2(mainVp->Pos.x + mainVp->Size.x, mainVp->Pos.y + mainVp->Size.y), mainTokens.surfaces.modalOverlay.u32);
            }
        }
        m_pluginBrowserModal->render();
        renderDspTweakModal();
        renderUpdateModal();
        if (m_settingsModal) {
            bool wasOpen = m_settingsModal->isOpen();
            m_settingsModal->render();
            if (wasOpen && !m_settingsModal->isOpen()) {
                m_config.load();
                double sr = m_asio.currentSampleRate();
                if (sr <= 0.0) sr = 48000.0;
                m_riffBuffer.prepare(sr, static_cast<int>(m_config.retroactiveBufferMinutes));
            }
        }
        m_practiceToolsModal->render();
        if (m_tunerSettingsModal) m_tunerSettingsModal->render();
        m_exportDawModal.render(m_scenes, m_graph);
        renderPresetBrowserModal();
        renderFloatingHudToast();
        if (m_showHotkeyLegend || ImGui::GetIO().KeyAlt) {
            renderHotkeyLegendOverlay();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

bool RackView::renderStatusPill(bool isRunning, const char* statusText) {
    if (!statusText) return false;

    const ImVec2 textSize = ImGui::CalcTextSize(statusText);
    const float pillH = Metrics::pillHeight();
    const float pillW = textSize.x + 36.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    bool clicked = ImGui::InvisibleButton("##DspStatusPill", ImVec2(pillW, pillH));
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();

    if (clicked) {
        resetDspDropouts();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
        if (m_settingsModal) m_settingsModal->open();
    }

    uint32_t drops = m_dspDropouts ? m_dspDropouts->load(std::memory_order_relaxed) : 0;

    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (drops > 0) {
            ImGui::SetTooltip("Real-time DSP Load %% & Dropout Meter\nDropouts: %u\nClick to reset dropout counter\nDouble-click to open Audio Settings", drops);
        } else {
            ImGui::SetTooltip("Real-time DSP Load %% & Dropout Meter\nClick to reset dropout counter\nDouble-click to open Audio Settings");
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();

    // Background capsule pill
    ImU32 bgCol;
    ImU32 borderCol;
    if (isRunning) {
        if (drops > 0) {
            bgCol = held ? static_cast<ImU32>(tokens.surfaces.frameBgActive) : (hovered ? static_cast<ImU32>(tokens.surfaces.frameBgHovered) : static_cast<ImU32>(tokens.surfaces.frameBg));
            borderCol = held ? static_cast<ImU32>(tokens.borders.focus) : (hovered ? static_cast<ImU32>(tokens.borders.cardGlowActive) : static_cast<ImU32>(tokens.borders.strong));
        } else {
            bgCol = held ? static_cast<ImU32>(tokens.surfaces.frameBgActive) : (hovered ? static_cast<ImU32>(tokens.surfaces.frameBgHovered) : static_cast<ImU32>(tokens.surfaces.frameBg));
            borderCol = held ? static_cast<ImU32>(tokens.borders.cardGlowOpen) : (hovered ? static_cast<ImU32>(tokens.borders.focus) : static_cast<ImU32>(tokens.borders.subtle));
        }
    } else {
        bgCol = tokens.surfaces.cardBgFaulted;
        borderCol = tokens.borders.cardFaulted;
    }

    const float radius = pillH * 0.5f;
    dl->AddRectFilled(pos, ImVec2(pos.x + pillW, pos.y + pillH), bgCol, radius);
    dl->AddRect(pos, ImVec2(pos.x + pillW, pos.y + pillH), borderCol, radius);

    // Glowing LED Dot
    ImVec2 dotCenter(pos.x + 14.0f, pos.y + (pillH * 0.5f));
    if (isRunning) {
        if (drops > 0) {
            dl->AddCircleFilled(dotCenter, 6.0f, static_cast<ImU32>(tokens.signal.meterWarning.withAlpha(80.0f / 255.0f)));
            dl->AddCircleFilled(dotCenter, 3.5f, tokens.signal.meterWarning);
        } else {
            dl->AddCircleFilled(dotCenter, 6.0f, static_cast<ImU32>(tokens.signal.active.withAlpha(80.0f / 255.0f)));
            dl->AddCircleFilled(dotCenter, 3.5f, tokens.signal.active);
        }
    } else {
        dl->AddCircleFilled(dotCenter, 3.5f, tokens.signal.faulted);
    }

    // Text Label inside pill
    ImVec2 textPos(pos.x + 26.0f, pos.y + ((pillH - textSize.y) * 0.5f));
    ImU32 textCol;
    if (isRunning) {
        if (drops > 0) {
            textCol = hovered ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.warning);
        } else {
            textCol = hovered ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.success);
        }
    } else {
        textCol = hovered ? static_cast<ImU32>(tokens.text.primary) : static_cast<ImU32>(tokens.text.error);
    }
    dl->AddText(textPos, textCol, statusText);

    return clicked;
}

void RackView::renderPracticeRibbon() {
    const auto& tokens = themeTokens();
    const float btnH = Metrics::buttonHeight();
    const float modH = ui::px(98.0f);
    const float kRow2Y = 6.0f + btnH + 8.0f;

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 0.0f));

    // Responsive 4-column layout: Tuner, Metro, Gate, Master — all stretch
    if (ImGui::BeginTable("PracticeRibbonTable", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("TunerCol",  ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("MetroCol",  ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("GateCol",   ImGuiTableColumnFlags_WidthStretch, 0.20f);
        ImGui::TableSetupColumn("MasterCol", ImGuiTableColumnFlags_WidthStretch, 0.20f);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.panelBg.vec4);
        ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.card.vec4);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));

        // ----------------------------------------------------
        // Column 1: Strobe Tuner Module
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::BeginChild("Mod_Tuner", ImVec2(0, modH), true, ImGuiWindowFlags_NoScrollbar);
        {
            auto result = m_tuner.currentResult();
            const float availW = ImGui::GetContentRegionAvail().x;
            const float availH = ImGui::GetContentRegionAvail().y;
            DrawStrobeTunerModule(result, availW, availH, &m_tuner, [this]() {
                if (m_tunerSettingsModal) m_tunerSettingsModal->open();
            });
        }
        ImGui::EndChild();

        // ----------------------------------------------------
        // Column 2: Metronome Module
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::BeginChild("Mod_Metro", ImVec2(0, modH), true, ImGuiWindowFlags_NoScrollbar);
        {
            const float fontH = ImGui::GetFontSize();
            ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, 6.0f + (btnH - fontH) * 0.5f));
            ImGui::TextColored(tokens.text.muted.vec4, "METRONOME");
            ImGui::SameLine(0, 8);
            bool metroPlaying = m_metronome.isPlaying();

            // Beat LEDs beside title
            {
                const int totalBeats = m_metronome.beatsPerBar();
                const int currentBeat = m_metronome.currentBeat();
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 curPos = ImGui::GetCursorScreenPos();
                const float dotRadius = 4.0f;
                const float dotSpacing = 11.0f;
                float cy = curPos.y + (fontH * 0.5f);

                for (int b = 0; b < totalBeats; ++b) {
                    float cx = curPos.x + 4.0f + (static_cast<float>(b) * dotSpacing);
                    bool active = metroPlaying && (b == currentBeat);
                    if (active) {
                        ImU32 haloColor = (b == 0) ?
                            static_cast<ImU32>(tokens.signal.faulted.withAlpha(80.0f / 255.0f)) :
                            static_cast<ImU32>(tokens.signal.meterWarning.withAlpha(80.0f / 255.0f));
                        dl->AddCircleFilled(ImVec2(cx, cy), dotRadius + 2.5f, haloColor);
                        ImU32 ledColor = (b == 0) ? static_cast<ImU32>(tokens.signal.faulted) : static_cast<ImU32>(tokens.signal.meterWarning);
                        dl->AddCircleFilled(ImVec2(cx, cy), dotRadius, ledColor);
                    } else {
                        dl->AddCircleFilled(ImVec2(cx, cy), dotRadius, tokens.surfaces.windowBg);
                        dl->AddCircle(ImVec2(cx, cy), dotRadius, tokens.borders.strong, 0, 1.0f);
                    }
                }
                ImGui::Dummy(ImVec2(static_cast<float>(totalBeats) * dotSpacing + 4.0f, fontH));
            }

            const float gap = 6.0f;
            const float playW = 54.0f;
            const float sigW = 68.0f;

            // Position TAP button in top row directly aligned above the TimeSig dropdown with standard button size
            const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            const float comboStartX = rightEdge - sigW;

            ImGui::SetCursorPos(ImVec2(comboStartX, 6.0f));

            auto now = std::chrono::steady_clock::now();
            bool tapFlash = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastTapFlash).count() < 120;
            {
                ScopedStyleColor tapGuard({
                    {ImGuiCol_Button, tokens.text.accent.vec4},
                    {ImGuiCol_Text, tokens.text.inverse.vec4}
                }, tapFlash);
                if (CenteredButton("TAP", ImVec2(sigW, btnH))) {
                    m_lastTapFlash = now;
                    // Remove tap times older than 2.5 seconds
                    while (!m_tapTimes.empty() && std::chrono::duration<double>(now - m_tapTimes.front()).count() > 2.5) {
                        m_tapTimes.erase(m_tapTimes.begin());
                    }
                    if (!m_tapTimes.empty() && std::chrono::duration<double>(now - m_tapTimes.back()).count() > 2.5) {
                        m_tapTimes.clear();
                    }
                    m_tapTimes.push_back(now);
                    if (m_tapTimes.size() > 6) {
                        m_tapTimes.erase(m_tapTimes.begin());
                    }
                    if (m_tapTimes.size() >= 2) {
                        double totalDurationSec = std::chrono::duration<double>(m_tapTimes.back() - m_tapTimes.front()).count();
                        double avgIntervalSec = totalDurationSec / static_cast<double>(m_tapTimes.size() - 1);
                        if (avgIntervalSec > 0.05) {
                            float computedBpm = static_cast<float>(60.0 / avgIntervalSec);
                            computedBpm = std::clamp(std::round(computedBpm), 40.0f, 260.0f);
                            m_metronome.setBpm(computedBpm);
                            m_looper.setBpm(computedBpm);
                        }
                    }
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tap Tempo (Tap repeatedly to set BPM)");

            const float ctrlH = Metrics::buttonHeight();
            ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, kRow2Y));
            float bpmW = std::max(60.0f, comboStartX - gap - (ImGui::GetStyle().WindowPadding.x + playW + gap));

            {
                ScopedStyleColor metroGuard({
                    {ImGuiCol_Button, tokens.signal.faulted.vec4},
                    {ImGuiCol_Text, tokens.text.primary.vec4}
                }, metroPlaying);
                if (CenteredButton(metroPlaying ? "STOP##M" : "PLAY##M", ImVec2(playW, ctrlH))) {
                    m_metronome.setPlaying(!metroPlaying);
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(metroPlaying ? "Stop Metronome" : "Start Metronome");

            ImGui::SameLine(0, gap);

            // Tempo Button / Direct Click-to-Type Input
            float curBpm = m_metronome.bpm();
            if (m_tempoEditing) {
                ImGui::SetNextItemWidth(bpmW);
                ImGui::SetKeyboardFocusHere();
                if (ImGui::InputInt("##BPM_Edit", &m_tempoEditValue, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    m_tempoEditValue = std::clamp(m_tempoEditValue, 40, 260);
                    m_metronome.setBpm(static_cast<float>(m_tempoEditValue));
                    m_looper.setBpm(static_cast<float>(m_tempoEditValue));
                    m_tempoEditing = false;
                }
                if (ImGui::IsItemDeactivated()) {
                    m_tempoEditValue = std::clamp(m_tempoEditValue, 40, 260);
                    m_metronome.setBpm(static_cast<float>(m_tempoEditValue));
                    m_looper.setBpm(static_cast<float>(m_tempoEditValue));
                    m_tempoEditing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                    m_tempoEditing = false;
                }
            } else {
                char bpmLabel[32];
                std::snprintf(bpmLabel, sizeof(bpmLabel), "%.0f BPM##BPMBtn", curBpm);
                if (CenteredButton(bpmLabel, ImVec2(bpmW, ctrlH))) {
                    m_tempoEditing = true;
                    m_tempoEditValue = static_cast<int>(std::round(curBpm));
                }
                if (ImGui::IsItemHovered()) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        m_metronome.setBpm(120.0f);
                        m_looper.setBpm(120.0f);
                        m_tempoEditing = false;
                    }
                    ImGui::SetTooltip("Tempo: %.0f BPM\nSingle-click to type tempo\nDouble-click to reset to 120 BPM", curBpm);
                }
            }

            ImGui::SameLine(comboStartX);

            const char* timeSigOptions[] = { "4/4", "3/4", "2/4", "6/8" };
            const int timeSigBeats[] = { 4, 3, 2, 6 };
            const int timeSigDenoms[] = { 4, 4, 4, 8 };
            int currentSigIdx = 0;
            int currentBeats = m_metronome.beatsPerBar();
            for (int i = 0; i < 4; ++i) {
                if (timeSigBeats[i] == currentBeats) { currentSigIdx = i; break; }
            }
            ImGui::SetNextItemWidth(sigW);
            if (ImGui::Combo("##TimeSig", &currentSigIdx, timeSigOptions, 4)) {
                m_metronome.setBeatsPerBar(timeSigBeats[currentSigIdx]);
                m_metronome.setTimeSigDenominator(timeSigDenoms[currentSigIdx]);
                m_looper.setBeatsPerBar(timeSigBeats[currentSigIdx]);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Time Signature");
        }
        ImGui::EndChild();

        // ----------------------------------------------------
        // Column 3: Noise Gate Module
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::BeginChild("Mod_Gate", ImVec2(0, modH), true, ImGuiWindowFlags_NoScrollbar);
        {
            auto& gate = m_graph.inputNoiseGate();
            bool gateOn = gate.isEnabled();

            const float btnW = btnH; // Standard square button matching power/options
            const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            const float powerBtnX = rightEdge - btnW;
            const float powerBtnY = 6.0f;

            ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, powerBtnY + (btnH - ImGui::GetFontSize()) * 0.5f));
            ImGui::TextColored(tokens.text.muted.vec4, "NOISE GATE");

            ImGui::SetCursorPos(ImVec2(powerBtnX, powerBtnY));
            if (ImGui::InvisibleButton("##GatePowerToggleBtn", ImVec2(btnW, btnH))) {
                gate.setEnabled(!gateOn);
            }
            const bool powerHovered = ImGui::IsItemHovered();
            const bool powerActive = ImGui::IsItemActive();
            if (powerHovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip(gateOn ? "Noise Gate: Active (Click to Bypass)" : "Noise Gate: Bypassed (Click to Enable)");
            }

            const ImU32 powerBg = powerActive ? tokens.surfaces.buttonBgActive.u32
                                : (powerHovered ? tokens.surfaces.buttonBgHovered.u32 : tokens.surfaces.frameBg.u32);
            const ImU32 powerBorder = gateOn ? tokens.signal.tunerInTune.u32 : tokens.borders.subtle.u32;
            const ImU32 powerIconCol = gateOn ? tokens.signal.tunerInTune.u32
                                    : (powerHovered ? tokens.text.secondary.u32 : tokens.text.muted.u32);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 btnMin = ImGui::GetItemRectMin();
            const ImVec2 btnMax = ImGui::GetItemRectMax();
            dl->AddRectFilled(btnMin, btnMax, powerBg, 4.0f);
            dl->AddRect(btnMin, btnMax, powerBorder, 4.0f, 0, 1.0f);
            DrawPowerGlyph(dl, ImVec2((btnMin.x + btnMax.x) * 0.5f, (btnMin.y + btnMax.y) * 0.5f), 6.5f, powerIconCol, 2.0f);

            ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, kRow2Y));
            float thresh = gate.thresholdDb();
            const float availGateW = ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemWidth(std::max(80.0f, availGateW));
            if (ResettableSliderFloat("##GateThresh", &thresh, -80.0f, -20.0f, -60.0f, "Thresh: %.0f dB")) {
                gate.setThresholdDb(thresh);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gate Threshold (Double-click to reset -60 dB)");
        }
        ImGui::EndChild();

        // ----------------------------------------------------
        // Column 4: Master Output Module
        // ----------------------------------------------------
        ImGui::TableNextColumn();
        ImGui::BeginChild("Mod_Master", ImVec2(0, modH), true, ImGuiWindowFlags_NoScrollbar);
        {
            float masterVol = m_graph.masterVolumeDb();

            ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, 6.0f + (btnH - ImGui::GetFontSize()) * 0.5f));
            ImGui::TextColored(tokens.text.primary.vec4, "MASTER OUTPUT");

            char valStr[32];
            std::snprintf(valStr, sizeof(valStr), "%+.1f dB", masterVol);
            float valTextW = ImGui::CalcTextSize(valStr).x;
            const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
            if (rightEdge - valTextW > ImGui::GetStyle().WindowPadding.x + ImGui::CalcTextSize("MASTER OUTPUT").x + 8.0f) {
                ImGui::SetCursorPos(ImVec2(rightEdge - valTextW, 6.0f + (btnH - ImGui::GetFontSize()) * 0.5f));
                ImGui::TextColored(tokens.text.primary.vec4, "%s", valStr);
            }


            ImGui::SetCursorPos(ImVec2(ImGui::GetStyle().WindowPadding.x, kRow2Y));
            const float meterW = 9.0f;
            const float meterGap = 3.0f;
            const float availMasterW = ImGui::GetContentRegionAvail().x;
            const float sliderW = std::max(60.0f, availMasterW - (meterW * 2.0f + meterGap + 8.0f));

            ImGui::SetNextItemWidth(sliderW);
            if (ResettableSliderFloat("##MasterVol", &masterVol, -36.0f, +6.0f, 0.0f, "Vol: %+.1f dB")) {
                m_graph.setMasterVolumeDb(masterVol);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Master Volume (Double-click to reset 0.0 dB)");

            const float meterH = ImGui::GetItemRectSize().y;
            const float meterTopY = ImGui::GetItemRectMin().y;

            ImGui::SameLine(0, 6);
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, meterTopY));
            bool clipL = false;
            renderMeter("OutMeterL", m_graph.outputMeter().peakLeft(), m_graph.outputMeter().holdLeft(),
                        m_graph.outputMeter().isClipped(), meterW, meterH, &clipL);
            if (clipL) m_graph.outputMeter().resetClip();
            ImGui::SameLine(0, meterGap);
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, meterTopY));
            bool clipR = false;
            renderMeter("OutMeterR", m_graph.outputMeter().peakRight(), m_graph.outputMeter().holdRight(),
                        m_graph.outputMeter().isClipped(), meterW, meterH, &clipR);
            if (clipR) m_graph.outputMeter().resetClip();
        }
        ImGui::EndChild();

        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        ImGui::EndTable();
    }

    ImGui::PopStyleVar(); // CellPadding
}

void RackView::renderSceneBar() {
    const auto& tokens = themeTokens();
    const float barBtnH = Metrics::buttonHeight();
    const float fontSize = ImGui::GetFontSize();
    const float padY = std::max(0.0f, (barBtnH - fontSize) * 0.5f);
    const float padX = ImGui::GetStyle().FramePadding.x;
    const float kBtnSpacing = 6.0f;
    const float kGroupDividerSpacing = 10.0f;
    const float kLabelSpacing = 8.0f;

    const float availTotalW = ImGui::GetContentRegionAvail().x;
    const float toneSlotBtnW = std::max(barBtnH, std::max(ImGui::CalcTextSize("A").x, ImGui::CalcTextSize("B").x) + padX * 2.0f);
    const float copyBtnW = ImGui::CalcTextSize("Copy A \xE2\x86\x92 B##ToneCopy").x + padX * 2.0f;
    const float abSectionW = (kGroupDividerSpacing * 2.0f + 6.0f) + ImGui::CalcTextSize("A/B:").x + kLabelSpacing + 2.0f * toneSlotBtnW + copyBtnW + 2.0f * kBtnSpacing;
    const float undoRedoBtnW = std::max(ImGui::CalcTextSize("Undo").x, ImGui::CalcTextSize("Redo").x) + padX * 2.0f;
    const float undoRedoSectionW = (kGroupDividerSpacing * 2.0f + 6.0f) + 2.0f * undoRedoBtnW + kBtnSpacing;

    if (!m_userPresetsValid) {
        m_cachedUserPresetNames = m_scenes.savedPresetNames();
        m_userPresetsValid = true;
    }
    const auto& userPresets = m_cachedUserPresetNames;
    const float actionsCompactW = (kGroupDividerSpacing * 2.0f + 6.0f) + ImGui::CalcTextSize("Actions \xE2\x96\xBE").x + padX * 2.0f;
    const float favStarBtnW = barBtnH;
    const float fullActionsW = (kGroupDividerSpacing * 2.0f + 6.0f) + ImGui::CalcTextSize("Save Changes").x + ImGui::CalcTextSize("Save Preset As...").x + favStarBtnW + ImGui::CalcTextSize("Export DAW...").x + ImGui::CalcTextSize("Preset Browser...").x + 10.0f * padX + 5.0f * kBtnSpacing + (userPresets.empty() ? 0.0f : (130.0f + kBtnSpacing));

    const float favLabelW = ImGui::CalcTextSize("FAVORITES:").x + kLabelSpacing;
    const bool isCompact = (availTotalW - favLabelW - (abSectionW + undoRedoSectionW + fullActionsW) < 220.0f);
    const float reservedRightW = (isCompact ? (abSectionW + undoRedoSectionW + actionsCompactW) : (abSectionW + undoRedoSectionW + fullActionsW)) + 16.0f;
    const float maxFavAreaW = std::max(140.0f, availTotalW - favLabelW - reservedRightW);

    float totalFavContentW = 0.0f;
    if (m_favoritePresets.empty()) {
        totalFavContentW = ImGui::CalcTextSize("(No favorites)").x + kBtnSpacing + ImGui::CalcTextSize("+ Browse Presets").x + padX * 2.0f;
    } else {
        for (size_t i = 0; i < m_favoritePresets.size(); ++i) {
            if (i > 0) totalFavContentW += kBtnSpacing;
            totalFavContentW += ImGui::CalcTextSize(m_favoritePresets[i].c_str()).x + padX * 2.0f;
        }
    }

    const bool collapseFavsToDropdown = (maxFavAreaW < 220.0f);
    const bool needsFavScroll = !collapseFavsToDropdown && (totalFavContentW > maxFavAreaW);
    const float favAreaW = needsFavScroll ? maxFavAreaW : totalFavContentW;
    const float favChildH = needsFavScroll ? (barBtnH + 6.0f) : barBtnH;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padX, padY));

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(tokens.text.secondary.vec4, "FAVORITES:");
    ImGui::SameLine(0, kLabelSpacing);

    if (collapseFavsToDropdown) {
        std::string comboPreview = m_auditionedPreset.empty() ? "Favorites..." : m_auditionedPreset;
        ImGui::SetNextItemWidth(std::max(120.0f, maxFavAreaW));
        if (ImGui::BeginCombo("##FavPresetsDropdown", comboPreview.c_str())) {
            if (m_favoritePresets.empty()) {
                ImGui::TextDisabled("No favorites added");
            } else {
                for (size_t i = 0; i < m_favoritePresets.size(); ++i) {
                    const auto& favName = m_favoritePresets[i];
                    bool isSelected = (m_auditionedPreset == favName);
                    if (ImGui::Selectable(favName.c_str(), isSelected)) {
                        loadPresetChain(favName);
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::Selectable("+ Browse Presets...")) {
                m_showPresetBrowserModal = true;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Quick Favorites (Collapses to dropdown on smaller displays)");
        }
    } else {
        // Scrollable Horizontal Favorites Container
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 3.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, tokens.surfaces.scrollbarBg.vec4);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, tokens.borders.focus.vec4);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, tokens.signal.active.vec4);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, tokens.surfaces.sceneActiveEmerald.vec4);

        ImGuiWindowFlags favFlags = ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse;
        if (needsFavScroll) {
            favFlags |= ImGuiWindowFlags_HorizontalScrollbar;
        } else {
            favFlags |= ImGuiWindowFlags_NoScrollbar;
        }

        ImGui::BeginChild("FavPresetsScroll", ImVec2(favAreaW, favChildH), false, favFlags);

        if (ImGui::IsWindowHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f) {
                ImGui::SetScrollX(ImGui::GetScrollX() - wheel * 45.0f);
            }
        }

        if (m_favoritePresets.empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(tokens.text.muted.vec4, "(No favorites)");
            ImGui::SameLine(0, 6.0f);
            if (CenteredButton("+ Browse Presets", ImVec2(0, barBtnH))) {
                m_showPresetBrowserModal = true;
            }
        } else {
            for (size_t i = 0; i < m_favoritePresets.size(); ++i) {
                const auto& favName = m_favoritePresets[i];
                if (i > 0) {
                    ImGui::SameLine(0, kBtnSpacing);
                }

                bool isActive = (m_auditionedPreset == favName);
                {
                    ScopedStyleColor favGuard({
                        {ImGuiCol_Button, tokens.surfaces.sceneActiveEmerald.vec4},
                        {ImGuiCol_Border, tokens.borders.focus.vec4},
                        {ImGuiCol_Text, tokens.text.primary.vec4}
                    }, isActive);

                    char btnLabel[96];
                    std::snprintf(btnLabel, sizeof(btnLabel), "%s##FavBtn_%zu", favName.c_str(), i);
                    if (CenteredButton(btnLabel, ImVec2(0, barBtnH))) {
                        if (m_midiMapMode) {
                            m_midi.startLearning(midi::BindingTargetType::SceneSelect, -1, -1, static_cast<int>(i));
                            triggerHudToast("MIDI LEARN: SCENE " + std::to_string(i + 1));
                        } else {
                            loadPresetChain(favName);
                        }
                    }
                }

                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Preset Chain: %s\nClick to load (10ms crossfade)\nRight-click to manage", favName.c_str());
                }

                char ctxId[96];
                std::snprintf(ctxId, sizeof(ctxId), "FavCtx_%zu", i);
                if (ImGui::BeginPopupContextItem(ctxId)) {
                    if (ImGui::MenuItem("Remove from Favorites")) {
                        toggleFavoritePreset(favName);
                    }
                    if (ImGui::MenuItem("Preset Browser...")) {
                        m_showPresetBrowserModal = true;
                    }
                    ImGui::EndPopup();
                }
            }
        }

        ImGui::EndChild();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(3);
    }

    // Instant A/B Tone Snapshot Comparison Toggle
    ImGui::SameLine(0, kGroupDividerSpacing);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(tokens.text.muted.vec4, "|");
    ImGui::SameLine(0, kGroupDividerSpacing);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(tokens.text.secondary.vec4, "A/B:");
    ImGui::SameLine(0, kLabelSpacing);

    const bool isToneAActive = (m_scenes.activeToneSnapshot() == state::SceneManager::ToneSnapshotSlot::A);
    const bool isToneBActive = (m_scenes.activeToneSnapshot() == state::SceneManager::ToneSnapshotSlot::B);

    {
        ScopedStyleColor toneAGuard({
            {ImGuiCol_Button, tokens.surfaces.sceneActiveEmerald.vec4},
            {ImGuiCol_Border, tokens.borders.focus.vec4},
            {ImGuiCol_Text, tokens.text.primary.vec4}
        }, isToneAActive);
        if (CenteredButton("A##ToneA", ImVec2(toneSlotBtnW, barBtnH))) {
            if (m_midiMapMode) {
                m_midi.startLearning(midi::BindingTargetType::SceneSelect, -1, -1, 0);
                triggerHudToast("MIDI LEARN: SCENE A");
            } else if (!isToneAActive) {
                if (isToneBActive) {
                    m_scenes.captureToneSnapshotB(m_graph);
                }
                if (m_scenes.hasToneSnapshotA()) {
                    m_scenes.applyToneSnapshotA(m_graph);
                    syncMacrosFromGraph();
                    m_undoManager.pushSnapshot(
                        state::SceneManager::captureGraphSnapshot(m_graph, "Tone A"),
                        "Tone A",
                        state::UndoActionType::SceneSwitch
                    );
                    triggerHudToast("A/B: SWITCHED TO TONE A");
                } else {
                    m_scenes.captureToneSnapshotA(m_graph);
                    m_scenes.setActiveToneSnapshot(state::SceneManager::ToneSnapshotSlot::A);
                    triggerHudToast("A/B: STORED TO TONE A");
                }
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Instant A/B Tone Memory A (Click to compare with 10ms crossfade)");

    ImGui::SameLine(0, kBtnSpacing);
    {
        ScopedStyleColor toneBGuard({
            {ImGuiCol_Button, tokens.surfaces.sceneActiveEmerald.vec4},
            {ImGuiCol_Border, tokens.borders.focus.vec4},
            {ImGuiCol_Text, tokens.text.primary.vec4}
        }, isToneBActive);
        if (CenteredButton("B##ToneB", ImVec2(toneSlotBtnW, barBtnH))) {
            if (m_midiMapMode) {
                m_midi.startLearning(midi::BindingTargetType::SceneSelect, -1, -1, 1);
                triggerHudToast("MIDI LEARN: SCENE B");
            } else if (!isToneBActive) {
                if (isToneAActive || m_scenes.activeToneSnapshot() == state::SceneManager::ToneSnapshotSlot::None) {
                    m_scenes.captureToneSnapshotA(m_graph);
                }
                if (m_scenes.hasToneSnapshotB()) {
                    m_scenes.applyToneSnapshotB(m_graph);
                    syncMacrosFromGraph();
                    m_undoManager.pushSnapshot(
                        state::SceneManager::captureGraphSnapshot(m_graph, "Tone B"),
                        "Tone B",
                        state::UndoActionType::SceneSwitch
                    );
                    triggerHudToast("A/B: SWITCHED TO TONE B");
                } else {
                    m_scenes.copyToneSnapshotAToB();
                    m_scenes.setActiveToneSnapshot(state::SceneManager::ToneSnapshotSlot::B);
                    triggerHudToast("A/B: INITIALIZED TONE B FROM TONE A");
                }
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Instant A/B Tone Memory B (Click to compare with 10ms crossfade)");

    ImGui::SameLine(0, kBtnSpacing);
    if (CenteredButton("Copy A \xE2\x86\x92 B##ToneCopy", ImVec2(0, barBtnH))) {
        if (isToneAActive || !m_scenes.hasToneSnapshotA()) {
            m_scenes.captureToneSnapshotA(m_graph);
        }
        m_scenes.copyToneSnapshotAToB();
        if (isToneBActive) {
            m_scenes.applyToneSnapshotB(m_graph);
            syncMacrosFromGraph();
            m_undoManager.pushSnapshot(
                state::SceneManager::captureGraphSnapshot(m_graph, "Copy A to B"),
                "Copy Tone A to B",
                state::UndoActionType::SceneSwitch
            );
        }
        triggerHudToast("A/B: COPIED TONE A TO B");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy current Tone A snapshot memory to Tone B");

    // Instant Undo / Redo in SceneBar
    ImGui::SameLine(0, kGroupDividerSpacing);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(tokens.text.muted.vec4, "|");
    ImGui::SameLine(0, kGroupDividerSpacing);

    bool canUndo = (m_undoManager.undoCount() > 1) || (m_pendingGuiEditSlot != nullptr);
    if (!canUndo) ImGui::BeginDisabled();
    if (CenteredButton("Undo##SceneUndo", ImVec2(undoRedoBtnW, barBtnH))) {
        pollPluginGuiEdits(true);
        if (m_undoManager.undoCount() > 1) {
            m_undoManager.undo(m_graph, m_scenes);
            syncMacrosFromGraph();
            triggerHudToast("UNDO: " + m_undoManager.lastActionDescription());
        }
    }
    if (!canUndo) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo last parameter or chain edit (Ctrl+Z)");

    ImGui::SameLine(0, kBtnSpacing);
    bool canRedo = m_undoManager.canRedo();
    if (!canRedo) ImGui::BeginDisabled();
    if (CenteredButton("Redo##SceneRedo", ImVec2(undoRedoBtnW, barBtnH))) {
        pollPluginGuiEdits(true);
        m_undoManager.redo(m_graph, m_scenes);
        syncMacrosFromGraph();
        triggerHudToast("REDO: " + m_undoManager.lastActionDescription());
    }
    if (!canRedo) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Redo last undone edit (Ctrl+Y / Ctrl+Shift+Z)");

    ImGui::SameLine(0, kGroupDividerSpacing);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(tokens.text.muted.vec4, "|");
    ImGui::SameLine(0, kGroupDividerSpacing);

    const float availW = ImGui::GetContentRegionAvail().x;
    const float requiredPresetBarW = userPresets.empty() ? 440.0f : 560.0f;
    const bool isBarCompact = isCompact || (availW < requiredPresetBarW);

    if (isBarCompact) {
        if (CenteredButton("Actions \xE2\x96\xBE", ImVec2(0, barBtnH))) {
            ImGui::OpenPopup("SceneActionsOverflowPopup");
        }
        if (ImGui::BeginPopup("SceneActionsOverflowPopup")) {
            if (ImGui::MenuItem("Save Changes")) {
                if (!m_auditionedPreset.empty()) {
                    m_scenes.savePresetChain(m_auditionedPreset, m_graph);
                    m_userPresetsValid = false;
                    triggerHudToast("Updated preset: " + m_auditionedPreset);
                } else {
                    m_showSavePresetModal = true;
                    m_presetNameBuffer[0] = '\0';
                }
            }
            if (ImGui::MenuItem("Save Preset As...")) {
                m_showSavePresetModal = true;
                m_presetNameBuffer[0] = '\0';
            }
            const bool isFav = !m_auditionedPreset.empty() && isPresetFavorite(m_auditionedPreset);
            std::string favMenuLabel = isFav ? "\xE2\x98\x85 Remove from Favorites" : "\xE2\x98\x86 Add to Favorites";
            if (!m_auditionedPreset.empty()) {
                favMenuLabel += " (" + m_auditionedPreset + ")";
            }
            if (ImGui::MenuItem(favMenuLabel.c_str())) {
                if (!m_auditionedPreset.empty()) {
                    toggleFavoritePreset(m_auditionedPreset);
                    triggerHudToast(isFav ? "Removed from favorites: " + m_auditionedPreset : "Added to favorites: " + m_auditionedPreset);
                } else {
                    triggerHudToast("Save or load a preset first");
                }
            }
            if (ImGui::MenuItem("Export DAW...")) {
                m_exportDawModal.open();
            }
            if (ImGui::MenuItem("Preset Browser...")) {
                m_showPresetBrowserModal = true;
            }
            if (!userPresets.empty()) {
                ImGui::Separator();
                if (ImGui::BeginMenu("Quick Load Preset")) {
                    for (const auto& pName : userPresets) {
                        if (ImGui::MenuItem(pName.c_str())) {
                            loadPresetChain(pName);
                        }
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::EndPopup();
        }
    } else {
        if (CenteredButton("Save Changes", ImVec2(0, barBtnH))) {
            if (!m_auditionedPreset.empty()) {
                m_scenes.savePresetChain(m_auditionedPreset, m_graph);
                m_userPresetsValid = false;
                triggerHudToast("Updated preset: " + m_auditionedPreset);
            } else {
                m_showSavePresetModal = true;
                m_presetNameBuffer[0] = '\0';
            }
        }
        if (ImGui::IsItemHovered()) {
            if (!m_auditionedPreset.empty()) {
                ImGui::SetTooltip("Save current plugin chain changes to active preset '%s'", m_auditionedPreset.c_str());
            } else {
                ImGui::SetTooltip("Save current plugin chain as a new preset");
            }
        }

        ImGui::SameLine(0, kBtnSpacing);
        if (CenteredButton("Save Preset As...", ImVec2(0, barBtnH))) {
            m_showSavePresetModal = true;
            m_presetNameBuffer[0] = '\0';
        }

        ImGui::SameLine(0, kBtnSpacing);
        const bool isFav = !m_auditionedPreset.empty() && isPresetFavorite(m_auditionedPreset);
        const float starBtnW = barBtnH;
        ImVec2 starPos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(starPos, ImVec2(starPos.x + starBtnW, starPos.y + barBtnH), tokens.surfaces.buttonBg.u32, 4.0f);
        dl->AddRect(starPos, ImVec2(starPos.x + starBtnW, starPos.y + barBtnH), isFav ? tokens.text.accent.u32 : tokens.borders.subtle.u32, 4.0f, 0, 1.0f);
        if (renderCenteredStarButton("##ToggleFavPresetBar", ImVec2(starBtnW, barBtnH), isFav)) {
            if (!m_auditionedPreset.empty()) {
                toggleFavoritePreset(m_auditionedPreset);
                triggerHudToast(isFav ? "Removed from favorites: " + m_auditionedPreset : "Added to favorites: " + m_auditionedPreset);
            } else {
                triggerHudToast("Save or load a preset first");
            }
        }
        if (ImGui::IsItemHovered()) {
            if (!m_auditionedPreset.empty()) {
                ImGui::SetTooltip(isFav ? "Remove '%s' from Favorites" : "Add '%s' to Favorites", m_auditionedPreset.c_str());
            } else {
                ImGui::SetTooltip("Add current preset to Favorites (save or load a preset first)");
            }
        }

        ImGui::SameLine(0, kBtnSpacing);
        if (CenteredButton("Export DAW...", ImVec2(0, barBtnH))) {
            m_exportDawModal.open();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Export chain to Reaper (.RfxChain), Ableton (.adg), DAWproject (.dawproject), or VST3 (.vstpreset)");
        }

        ImGui::SameLine(0, kBtnSpacing);
        if (CenteredButton("Preset Browser...", ImVec2(0, barBtnH))) {
            m_showPresetBrowserModal = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Open categorized preset chain browser with sound profiles, search, and favorites");
        }

        if (!userPresets.empty()) {
            ImGui::SameLine(0, kBtnSpacing);
            ImGui::SetNextItemWidth(130);
            if (ImGui::BeginCombo("##UserPresetsCombo", "Quick Load...")) {
                for (const auto& pName : userPresets) {
                    if (ImGui::Selectable(pName.c_str())) {
                        loadPresetChain(pName);
                    }
                }
                ImGui::EndCombo();
            }
        }
    }

    ImGui::PopStyleVar(); // FramePadding

    // Modal for Save Preset As... (true centered PopupModal with dimmed background & Enter-to-save)
    if (m_showSavePresetModal) {
        ImGui::OpenPopup("Save Preset Chain##Modal");
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 center = vp ? vp->GetCenter() : ImVec2(640.0f, 360.0f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(400.0f, 0.0f), ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Spec::kModalWindowPadding, Spec::kModalWindowPadding));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Spec::kModalCornerRadius);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, tokens.surfaces.popupBg.vec4);
        ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.focus.vec4);

        if (ImGui::BeginPopupModal("Save Preset Chain##Modal", &m_showSavePresetModal,
                                   ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar)) {
            ImGui::TextColored(tokens.text.accent.vec4, "SAVE PRESET CHAIN");
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::Text("Preset Name:");
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetKeyboardFocusHere();
            }
            const bool enterPressed = ImGui::InputTextWithHint(
                "##NewPresetNameInput", "e.g., Modern Rhythm Crunch...",
                m_presetNameBuffer, sizeof(m_presetNameBuffer),
                ImGuiInputTextFlags_EnterReturnsTrue
            );
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            if (CenteredButton("Save", ImVec2(110.0f, Metrics::buttonHeight())) || enterPressed) {
                if (m_presetNameBuffer[0] != '\0') {
                    m_scenes.savePresetChain(m_presetNameBuffer, m_graph);
                    m_userPresetsValid = false;
                    m_auditionedPreset = m_presetNameBuffer;
                    state::AppConfig cfg;
                    cfg.load();
                    cfg.lastLoadedPreset = m_presetNameBuffer;
                    cfg.save();
                    m_cachedConfig = cfg;
                    triggerHudToast(std::string("Preset '") + m_presetNameBuffer + "' saved!");
                    m_showSavePresetModal = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine(0, 8.0f);
            if (CenteredButton("Cancel", ImVec2(90.0f, Metrics::buttonHeight())) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                m_showSavePresetModal = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}

namespace {
float calculateParallelEnvelopeWidth(size_t numSlots0, size_t numSlots1) {
    const float cardW = ui::px(240.0f);
    const float slotWireW = ui::px(28.0f);
    const float insertCardW = ui::px(80.0f);
    const float envPadX = ui::px(14.0f);

    const float w0 = envPadX + (static_cast<float>(numSlots0) * (cardW + slotWireW)) + insertCardW + envPadX;
    const float w1 = envPadX + (static_cast<float>(numSlots1) * (cardW + slotWireW)) + insertCardW + envPadX;

    // Minimum width needed for header controls to never overlap:
    // Left: Pad(12) + Badge("BRANCH A/B" ~90-140) + Spacing(8) + SlotCount("(88 Slots)" ~55-80) + Spacing(10)
    // Buttons: Mute(24) + Spacing(4) + Solo(24) + Spacing(4) + Phase(24) + Spacing(8)
    // Sliders: 2 * minComfortableSliderW(72) + sliderGap(6) = 150
    // Right: Safety(16) + Minimise(24) + Spacing(4) + Delete(24) + RightMargin(14) = 82
    const float badgeW = ImGui::CalcTextSize("BRANCH A").x + 20.0f;
    const float slotTextW = ImGui::CalcTextSize("(99 Slots)").x;
    const float minSlidersW = (72.0f * 2.0f) + 6.0f;
    const float headerRequiredW = 12.0f + badgeW + 8.0f + slotTextW + 10.0f
                                + (24.0f * 3.0f + 8.0f) + 8.0f
                                + minSlidersW + 16.0f + 52.0f + 14.0f;

    const float minEnvelopeW = std::max(540.0f, headerRequiredW);
    return std::max({ w0, w1, minEnvelopeW });
}
} // namespace

void RackView::renderSignalRack() {
    m_graph.processReclamation();

    const auto& tokens = themeTokens();
    const float bottomBarReservedH = 54.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.panelBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.subtle.vec4);
    ImGui::BeginChild("RackScrollArea", ImVec2(0, -bottomBarReservedH), true, ImGuiWindowFlags_HorizontalScrollbar);

    const size_t numNodes = m_graph.numNodes();
    bool hasParallel = false;
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = m_graph.getNode(i);
        if (node && node->type() == audio::NodeType::ParallelSplitMerge) {
            hasParallel = true;
            break;
        }
    }

    const float cardW = ui::px(240.0f);
    const float wireW = ui::px(36.0f);
    const float insertCardW = ui::px(80.0f);
    const float outputCardW = ui::px(100.0f);
    const float inputCardW = ui::px(180.0f);

    // 1. Analytical Pre-computation of Total Chain Footprint
    float totalContentWidth = inputCardW; // 180px

    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = m_graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            totalContentWidth += (wireW + cardW);
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            const size_t n0 = (block && block->getBranch(0)) ? block->getBranch(0)->numSlots() : 0;
            const size_t n1 = (block && block->getBranch(1)) ? block->getBranch(1)->numSlots() : 0;
            const float envelopeW = calculateParallelEnvelopeWidth(n0, n1);

            // InWire(24) + Fork(36) + Envelope + Conv(36) + Comb(104) = Envelope + 200px
            totalContentWidth += (ui::px(24.0f) + ui::px(36.0f) + envelopeW + ui::px(36.0f) + ui::px(104.0f));
        }
    }

    // Insert serial card (+32px wire + 80px card = 112px)
    totalContentWidth += (wireW + insertCardW);

    // Output destination card (+32px wire + 100px card = 132px)
    totalContentWidth += (wireW + outputCardW);

    // 2. Viewport Dimension Acquisition & Dynamic Centering
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float viewportWidth = avail.x;
    const float viewportHeight = avail.y;

    const float safeW = (std::isnan(viewportWidth) || viewportWidth <= 0.0f) ? 0.0f : viewportWidth;
    const float safeH = (std::isnan(viewportHeight) || viewportHeight <= 0.0f) ? 0.0f : viewportHeight;

    // Dynamically calculate parallel block height based on branch collapse/expand states
    float maxParallelH = 0.0f;
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = m_graph.getNode(i);
        if (node && node->type() == audio::NodeType::ParallelSplitMerge) {
            const uint64_t k0 = (static_cast<uint64_t>(static_cast<uint32_t>(i)) << 32) | 0ULL;
            const uint64_t k1 = (static_cast<uint64_t>(static_cast<uint32_t>(i)) << 32) | 1ULL;
            bool m0 = m_branchMinimized[k0];
            bool m1 = m_branchMinimized[k1];
            const float envHeaderH = ui::px(36.0f);
            const float envPadY = ui::px(10.0f);
            const float branchCardH = ui::px(224.0f);
            const float envH = envHeaderH + (2.0f * envPadY) + branchCardH;
            const float h0 = m0 ? envHeaderH : envH;
            const float h1 = m1 ? envHeaderH : envH;
            const float bH = h0 + ui::px(20.0f) + h1;
            if (bH > maxParallelH) maxParallelH = bH;
        }
    }

    // Standard compact card height for rack elements
    const float serialCardH = ui::px(224.0f);

    const float totalContentHeight = hasParallel ? std::max(serialCardH, maxParallelH) : serialCardH;

    const float offsetX = std::max(20.0f, (safeW - totalContentWidth) * 0.5f);
    const float offsetY = std::max(16.0f, (safeH - totalContentHeight) * 0.5f);

    float centerY = 0.0f;
    float rackTotalH = 0.0f;
    if (safeH >= totalContentHeight) {
        centerY = safeH * 0.5f;
        rackTotalH = safeH;
    } else {
        centerY = offsetY + (totalContentHeight * 0.5f);
        rackTotalH = offsetY + totalContentHeight + 16.0f;
    }
    const float serialCardY = centerY - (serialCardH * 0.5f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    float scrollX = ImGui::GetScrollX();
    float scrollY = ImGui::GetScrollY();

    auto toScreen = [&](float lx, float ly) -> ImVec2 {
        return ImVec2(winPos.x + lx - scrollX, winPos.y + ly - scrollY);
    };

    float currentX = offsetX;
    float animTime = static_cast<float>(ImGui::GetTime());

    // 1. Input Node Card
    ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
    renderInputCard(serialCardY, serialCardH);
    currentX += inputCardW;

    float prevPeak = std::max(m_graph.inputMeter().peakLeft(), m_graph.inputMeter().peakRight());

    // 2. Render Graph Nodes
    for (size_t i = 0; i < numNodes; ++i) {
        auto* node = m_graph.getNode(i);
        if (!node) continue;

        if (node->type() == audio::NodeType::Plugin) {
            auto* slot = dynamic_cast<audio::PluginSlot*>(node);
            bool bypassed = slot ? slot->isBypassed() : false;

            drawCubicHermiteCable(dl, toScreen(currentX, centerY), toScreen(currentX + wireW, centerY),
                                 prevPeak, false, animTime);
            currentX += wireW;

            ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
            renderPluginSlot(slot, static_cast<int>(i), -1, -1, serialCardH);
            currentX += cardW;

            if (slot) {
                prevPeak = bypassed ? 0.0f : std::max(slot->meter().peakLeft(), slot->meter().peakRight());
            }
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            const float parInWireW = ui::px(24.0f);
            drawCubicHermiteCable(dl, toScreen(currentX, centerY), toScreen(currentX + parInWireW, centerY),
                                 prevPeak, false, animTime);
            currentX += parInWireW;

            ImGui::SetCursorPos(ImVec2(currentX, 0.0f));
            renderParallelBlock(block, static_cast<int>(i), centerY);
            currentX = ImGui::GetCursorPosX();

            prevPeak = 0.5f; // Active parallel mix signal
        }
    }

    // 3. Serial Rack Insertion Slot Card (+ Add Plugin)
    drawCubicHermiteCable(dl, toScreen(currentX, centerY), toScreen(currentX + wireW, centerY),
                         prevPeak, false, animTime);
    currentX += wireW;

    ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, static_cast<ImVec4>(tokens.surfaces.cardBg));
    ImGui::BeginChild("InsertSerialCard", ImVec2(insertCardW, serialCardH), true, ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2((insertCardW - ui::px(48.0f)) * 0.5f, (serialCardH - ui::px(48.0f)) * 0.5f));
    if (renderCenteredPlusButton("##AddPluginSerial", ImVec2(ui::px(48.0f), ui::px(48.0f)), ui::px(11.0f))) {
        m_pluginBrowserModal->open(-1, -1);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Insert a new plugin into the signal chain");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("RACK_CARD_PAYLOAD")) {
            const auto* pl = static_cast<const CardDragPayload*>(p->Data);
            if (pl && pl->sourceIndex >= 0) {
                moveSlot(pl->sourceIndex, pl->sourceBlockIndex, pl->sourceBranchIndex, -1, -1, -1);
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::EndGroup();
    currentX += insertCardW;

    // 4. Output Node Card
    drawCubicHermiteCable(dl, toScreen(currentX, centerY), toScreen(currentX + wireW, centerY),
                         prevPeak, false, animTime);
    currentX += wireW;

    ImGui::SetCursorPos(ImVec2(currentX, serialCardY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, static_cast<ImVec4>(tokens.surfaces.cardBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    ImGui::BeginChild("OutputDestNode", ImVec2(outputCardW, serialCardH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    renderBadgePill("OUTPUT", tokens.surfaces.branchABg, tokens.borders.branchABorder, tokens.text.badgeVst3, ui::px(18.0f));
    ImGui::TextDisabled("To ASIO");
    const float rackMeterW = ui::px(14.0f);
    const float outMeterH = std::max(ui::px(40.0f), ImGui::GetContentRegionAvail().y - ui::px(4.0f));
    bool clipL = false;
    renderMeter("FinalL", m_graph.outputMeter().peakLeft(), m_graph.outputMeter().holdLeft(),
                m_graph.outputMeter().isClipped(), rackMeterW, outMeterH, &clipL);
    if (clipL) m_graph.outputMeter().resetClip();
    ImGui::SameLine(0, ui::px(6.0f));
    bool clipR = false;
    renderMeter("FinalR", m_graph.outputMeter().peakRight(), m_graph.outputMeter().holdRight(),
                m_graph.outputMeter().isClipped(), rackMeterW, outMeterH, &clipR);
    if (clipR) m_graph.outputMeter().resetClip();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::EndGroup();
    currentX += outputCardW;

    // 3. Right / Bottom Extent Boundaries: Extend canvas ONLY when content exceeds viewport
    const float scrollPadX = ui::px(32.0f);
    const bool needScrollX = (currentX + scrollPadX > viewportWidth);
    const bool needScrollY = (rackTotalH > viewportHeight);

    if (needScrollX || needScrollY) {
        const float targetX = needScrollX ? (currentX + scrollPadX) : 0.0f;
        const float targetY = needScrollY ? rackTotalH : 0.0f;
        ImGui::SetCursorPos(ImVec2(targetX, targetY));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    ImGui::EndChild();
    ImGui::PopStyleColor(2);
}

void RackView::renderInputCard(float cardY, float cardH) {
    if (cardY >= 0.0f) {
        ImGui::SetCursorPosY(cardY);
    }
    const auto& tokens = themeTokens();
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, static_cast<ImVec4>(tokens.surfaces.cardBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    ImGui::BeginChild("InputGainNode", ImVec2(ui::px(180.0f), cardH), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    renderBadgePill("INPUT", tokens.surfaces.branchABg, tokens.borders.branchABorder, tokens.text.badgeVst3, ui::px(18.0f));

    // Input Channel Routing Selector (Mono/Stereo)
    auto inputConfig = m_graph.inputRouting();
    const char* routingLabels[] = {
        "Mono: In 1 (L+R)",
        "Mono: In 2 (L+R)",
        "Stereo: In 1+2"
    };

    int currentModeIdx = 0;
    if (inputConfig.mode == audio::InputRoutingMode::MonoLeft) currentModeIdx = 0;
    else if (inputConfig.mode == audio::InputRoutingMode::MonoRight) currentModeIdx = 1;
    else if (inputConfig.mode == audio::InputRoutingMode::Stereo) currentModeIdx = 2;

    ImGui::SetNextItemWidth(ui::px(164.0f));
    if (ImGui::Combo("##InRoute", &currentModeIdx, routingLabels, 3)) {
        if (currentModeIdx == 0) inputConfig.mode = audio::InputRoutingMode::MonoLeft;
        else if (currentModeIdx == 1) inputConfig.mode = audio::InputRoutingMode::MonoRight;
        else if (currentModeIdx == 2) inputConfig.mode = audio::InputRoutingMode::Stereo;
        m_graph.setInputRouting(inputConfig);

        state::AppConfig cfg;
        cfg.load();
        cfg.inputMode = inputConfig.mode;
        cfg.save();
    }

    float inGain = m_graph.inputGainDb();
    const float availH = ImGui::GetContentRegionAvail().y;
    const float meterH = std::max(ui::px(40.0f), availH - ui::px(4.0f));
    if (ResettableVSliderFloat("##InGain", ImVec2(ui::px(24.0f), meterH), &inGain, -24.0f, +24.0f, 0.0f, "")) {
        m_graph.setInputGainDb(inGain);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Input Gain (Double-click to reset 0 dB)");
    const float rackMeterW = ui::px(14.0f);
    ImGui::SameLine(0, ui::px(8.0f));
    bool clipInL = false;
    renderMeter("InL", m_graph.inputMeter().peakLeft(), m_graph.inputMeter().holdLeft(),
                m_graph.inputMeter().isClipped(), rackMeterW, meterH, &clipInL);
    if (clipInL) m_graph.inputMeter().resetClip();
    ImGui::SameLine(0, ui::px(6.0f));
    bool clipInR = false;
    renderMeter("InR", m_graph.inputMeter().peakRight(), m_graph.inputMeter().holdRight(),
                m_graph.inputMeter().isClipped(), rackMeterW, meterH, &clipInR);
    if (clipInR) m_graph.inputMeter().resetClip();

    ImGui::SameLine(0, ui::px(8.0f));
    ImGui::BeginGroup();
    ImGui::Text("%.0fdB", static_cast<double>(inGain));
    ImGui::TextDisabled("Gain");
    ImGui::EndGroup();

    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::EndGroup();
}

static inline std::string formatMacroLabel(const std::string& rawLabel, int macroIndex) {
    if (rawLabel.empty()) {
        return (macroIndex == 0 ? "GAIN" : (macroIndex == 1 ? "TONE" : (macroIndex == 2 ? "VOL" : "MIX")));
    }
    std::string s = rawLabel;
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower == "volume" || lower == "master volume") return "VOL";
    if (lower == "input gain" || lower == "preamp") return "GAIN";
    if (lower == "dry/wet" || lower == "dry wet" || lower == "mix level") return "MIX";
    if (lower == "threshold") return "THRESH";
    if (lower == "frequency") return "FREQ";
    if (lower == "resonance") return "RESON";
    if (lower == "overdrive") return "DRIVE";
    if (lower == "feedback") return "F.BACK";
    if (lower == "presence") return "PRES";

    if (s.size() <= 6) {
        std::string upper = s;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return upper;
    }

    return s.substr(0, 5) + "...";
}

void RackView::renderPluginSlot(audio::PluginSlot* slot, int slotIndex, int blockIndex, int branchIndex, float cardHeight) {
    if (!slot) return;

    initMacrosForSlot(slot);

    ImGui::BeginGroup();
    const auto& tokens = themeTokens();
    auto* pluginInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    if (pluginInst) {
        pluginInst->handleMainThreadCallbacks();
    }
    const bool isFaulted = pluginInst && pluginInst->isFaulted();
    const bool isMissing = slot->innerNode() && slot->innerNode()->isMissing();
    const bool isLoading = slot->innerNode() && slot->innerNode()->isLoading();
    const bool bypassed = slot->isBypassed();

    ImVec4 cardBg = (isFaulted || isMissing) ? static_cast<ImVec4>(tokens.surfaces.cardBgFaulted) :
                    (bypassed ? static_cast<ImVec4>(tokens.surfaces.cardBgBypassed) : static_cast<ImVec4>(tokens.surfaces.cardBg));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, cardBg);
    char childId[64];
    std::snprintf(childId, sizeof(childId), "SlotCard_%d_%d_%d", slotIndex, blockIndex, branchIndex);

    const float cardWidth = ui::px(240.0f);
    const ImVec2 cardScreenMin = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild(childId, ImVec2(cardWidth, cardHeight), true, ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    bool hasGui = pluginInst && pluginInst->hasCustomGui();
    bool isWindowOpen = false;
    if (hasGui) {
        isWindowOpen = plugins::PluginWindowManager::instance().isWindowOpen(pluginInst);
    } else if (m_dspTweakSlot == slot) {
        isWindowOpen = true;
    }

    // =========================================================================
    // SECTION 1: HEADER BAR (H = 24px, Y = 8px..32px)
    // =========================================================================
    {
        ImGui::SetCursorPos(ImVec2(ui::px(8.0f), ui::px(8.0f)));

        // Format pill badge
        const char* badgeStr = "DSP";
        ImU32 badgeTextCol = tokens.text.badgeInternal;
        if (isLoading) {
            badgeStr = "LOADING";
            badgeTextCol = tokens.signal.active.u32;
        } else if (isMissing) {
            badgeStr = "MISSING";
            badgeTextCol = tokens.signal.faulted.u32;
        } else if (pluginInst) {
            if (dynamic_cast<plugins::Vst3PluginInstance*>(pluginInst)) {
                badgeStr = "VST3";
                badgeTextCol = tokens.text.badgeVst3;
            } else if (dynamic_cast<plugins::ClapPluginInstance*>(pluginInst)) {
                badgeStr = "CLAP";
                badgeTextCol = tokens.text.badgeClap;
            }
        }
        renderBadgePill(badgeStr,
                        isMissing ? tokens.surfaces.cardBgFaulted : tokens.surfaces.frameBg,
                        isLoading ? static_cast<ImU32>(tokens.borders.focus) : (isMissing ? static_cast<ImU32>(tokens.borders.cardFaulted) : static_cast<ImU32>(tokens.borders.focus)),
                        badgeTextCol, ui::px(18.0f));
        ImGui::SameLine(0, ui::px(6.0f));

        // Flex title with scrolling marquee if overflowing
        std::string rawName = slot->name();
        const float rightEdge = cardWidth - ui::px(8.0f) - ui::px(38.0f) - (branchIndex == -1 ? ui::px(46.0f) : ui::px(24.0f));
        const float availTitleW = std::max(ui::px(60.0f), rightEdge - ImGui::GetCursorPosX() - ui::px(6.0f));
        const float rawTextW = ImGui::CalcTextSize(rawName.c_str()).x;
        const float overflow = rawTextW - availTitleW;

        const float fontH = ImGui::GetFontSize();
        ImGui::SetCursorPosY(ui::px(8.0f));
        const ImVec4 titleColor = isLoading ? tokens.signal.active.vec4 :
                                  (isMissing ? tokens.signal.faulted.vec4 :
                                  (isFaulted ? tokens.signal.faulted.vec4 :
                                  (bypassed ? tokens.signal.bypassed.vec4 : tokens.text.primary.vec4)));

        ImVec2 titlePos = ImGui::GetCursorScreenPos();
        char titleBtnId[64];
        std::snprintf(titleBtnId, sizeof(titleBtnId), "##SlotTitle_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        ImGui::InvisibleButton(titleBtnId, ImVec2(availTitleW, ui::px(18.0f)));
        const bool isTitleHovered = ImGui::IsItemHovered();
        ImVec2 clipMin = titlePos;
        ImVec2 clipMax = ImVec2(titlePos.x + availTitleW, titlePos.y + ui::px(18.0f));
        dl->PushClipRect(clipMin, clipMax, true);
        float textY = titlePos.y + (ui::px(18.0f) - fontH) * 0.5f;

        if (overflow > 0.0f && isTitleHovered) {
            // Smooth ping-pong marquee animation on hover to reveal full name
            const float scrollSpeed = 36.0f; // px / sec
            const float travelTime = overflow / scrollSpeed;
            const float pauseTime = 0.8f;
            const float cyclePeriod = travelTime * 2.0f + pauseTime * 2.0f;
            float t = std::fmod(static_cast<float>(ImGui::GetTime()), cyclePeriod);
            float scrollOffset = 0.0f;
            if (t >= pauseTime && t < pauseTime + travelTime) {
                scrollOffset = ((t - pauseTime) / travelTime) * overflow;
            } else if (t >= pauseTime + travelTime && t < pauseTime * 2.0f + travelTime) {
                scrollOffset = overflow;
            } else if (t >= pauseTime * 2.0f + travelTime) {
                float returnProgress = (t - (pauseTime * 2.0f + travelTime)) / travelTime;
                scrollOffset = (1.0f - returnProgress) * overflow;
            }
            dl->AddText(ImVec2(titlePos.x - scrollOffset, textY), ImGui::GetColorU32(titleColor), rawName.c_str());
        } else if (overflow > 0.0f) {
            // Static left-aligned title with clean right-side ellipsis so the start of the plugin name is never clipped
            std::string truncated = rawName;
            const float ellipsisW = ImGui::CalcTextSize("..").x;
            while (truncated.size() > 3 && ImGui::CalcTextSize(truncated.c_str()).x + ellipsisW > availTitleW) {
                truncated.pop_back();
            }
            truncated += "..";
            dl->AddText(ImVec2(titlePos.x, textY), ImGui::GetColorU32(titleColor), truncated.c_str());
        } else {
            dl->AddText(ImVec2(titlePos.x, textY), ImGui::GetColorU32(titleColor), rawName.c_str());
        }
        dl->PopClipRect();

        // Header drag-and-drop source
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            CardDragPayload payload{slotIndex, blockIndex, branchIndex};
            ImGui::SetDragDropPayload("RACK_CARD_PAYLOAD", &payload, sizeof(payload));
            const char* loc = (branchIndex == -1) ? "Rack" : (branchIndex == 0 ? "Branch A" : "Branch B");
            ImGui::Text("Moving %s (%s, Slot %d)", slot->name().c_str(), loc, slotIndex + 1);
            ImGui::EndDragDropSource();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s (%s)\nDrag to reorder slot", rawName.c_str(), pluginInst ? pluginInst->vendor().c_str() : "Praccy Audio");
        }

        // Action buttons & Sliding Pill Toggle on far right
        ImGui::SameLine(rightEdge);
        ImGui::SetCursorPosY(ui::px(8.0f));

        if (branchIndex == -1) {
            char spId[32];
            std::snprintf(spId, sizeof(spId), "##Sp_%d", slotIndex);
            if (renderCenteredSplitButton(spId, ImVec2(ui::px(18.0f), ui::px(18.0f)))) {
                m_graph.splitSerialNodeIntoParallel(slotIndex);
                syncMacrosFromGraph();
                m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Split Parallel"),
                                           "Split Parallel",
                                           state::UndoActionType::StructuralMutation);
                ImGui::EndChild();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                ImGui::EndGroup();
                return;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Split into Parallel Branches");
            ImGui::SameLine(0, ui::px(4.0f));
        }

        char delId[32];
        std::snprintf(delId, sizeof(delId), "##Del_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        if (renderCenteredDeleteButton(delId, ImVec2(ui::px(18.0f), ui::px(18.0f)))) {
            plugins::AsyncPluginLoader::instance().cancelSlot(slot);
            if (hasGui) plugins::PluginWindowManager::instance().closePluginWindow(pluginInst);
            if (m_dspTweakSlot == slot) m_dspTweakSlot = nullptr;
            if (m_expandedSlot == slot) m_expandedSlot = nullptr;
            if (m_pendingGuiEditSlot == slot) m_pendingGuiEditSlot = nullptr;
            if (m_focusedSlot == slot) {
                m_focusedSlot = nullptr;
                m_focusedSlotIndex = -1;
                m_focusedBlockIndex = -1;
                m_focusedBranchIndex = -1;
            }
            m_guiParamBaseline.erase(slot);
            m_slotMacros.erase(slot);
            if (branchIndex == -1) {
                m_graph.removeSerialNode(slotIndex);
            } else {
                auto* blk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(blockIndex));
                if (blk) {
                    auto* br = blk->getBranch(branchIndex);
                    if (br) br->removeSlot(slotIndex);
                }
            }
            m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Delete Slot"),
                                       "Delete Slot",
                                       state::UndoActionType::StructuralMutation);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::EndGroup();
            return;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete Slot");
        ImGui::SameLine(0, ui::px(5.0f));

        // Tactile Sliding Pill Toggle Switch (Active/Bypass)
        bool activeState = !bypassed;
        char pillId[32];
        std::snprintf(pillId, sizeof(pillId), "##Pill_%d_%d_%d", slotIndex, blockIndex, branchIndex);

        const int effSlot = (blockIndex >= 0 ? blockIndex : slotIndex);
        const int effBranchSlot = (blockIndex >= 0 ? slotIndex : -1);
        const bool isAwaitingBypass = m_keyMapMode && m_awaitingKeyTarget.active &&
                                      m_awaitingKeyTarget.slotIndex == effSlot &&
                                      m_awaitingKeyTarget.branchIndex == branchIndex &&
                                      m_awaitingKeyTarget.branchSlotIndex == effBranchSlot &&
                                      m_awaitingKeyTarget.targetType == static_cast<uint32_t>(midi::BindingTargetType::SlotBypass);

        if (renderSlidingPillToggle(pillId, &activeState, ImVec2(ui::px(38.0f), ui::px(18.0f)))) {
            if (m_keyMapMode) {
                m_awaitingKeyTarget.active = true;
                m_awaitingKeyTarget.slotIndex = effSlot;
                m_awaitingKeyTarget.blockIndex = blockIndex;
                m_awaitingKeyTarget.branchIndex = branchIndex;
                m_awaitingKeyTarget.branchSlotIndex = effBranchSlot;
                m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::SlotBypass);
                m_awaitingKeyTarget.paramId = 0;
                m_awaitingKeyTarget.targetLabel = slot->name() + " Bypass";
                triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
            } else if (m_midiMapMode) {
                m_midi.startLearning(midi::BindingTargetType::SlotBypass, effSlot, branchIndex, -1, effBranchSlot);
                triggerHudToast("MIDI LEARN: " + slot->name() + " BYPASS");
            } else {
                slot->setBypassed(!activeState);
                m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Toggle Bypass"),
                                           "Bypass: " + slot->name(),
                                           state::UndoActionType::ParameterTweak,
                                           slot->name() + ":Bypass",
                                           true);
                m_undoManager.commitActiveTransaction();
            }
        } else if (m_keyMapMode && ImGui::IsItemClicked()) {
            m_awaitingKeyTarget.active = true;
            m_awaitingKeyTarget.slotIndex = effSlot;
            m_awaitingKeyTarget.blockIndex = blockIndex;
            m_awaitingKeyTarget.branchIndex = branchIndex;
            m_awaitingKeyTarget.branchSlotIndex = effBranchSlot;
            m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::SlotBypass);
            m_awaitingKeyTarget.paramId = 0;
            m_awaitingKeyTarget.targetLabel = slot->name() + " Bypass";
            triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
        } else if (m_midiMapMode && ImGui::IsItemClicked()) {
            m_midi.startLearning(midi::BindingTargetType::SlotBypass, effSlot, branchIndex, -1, effBranchSlot);
            triggerHudToast("MIDI LEARN: " + slot->name() + " BYPASS");
        }

        if (isAwaitingBypass) {
            float t = static_cast<float>(ImGui::GetTime());
            float pulse = 0.5f + 0.5f * std::sin(8.0f * t);
            ImU32 highlightCol = tokens.borders.mapModeViolet.withAlpha(0.6f + 0.4f * pulse);
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            dl->AddRect(ImVec2(rMin.x - 2.0f, rMin.y - 2.0f), ImVec2(rMax.x + 2.0f, rMax.y + 2.0f), highlightCol, 4.0f, 0, 2.0f);
        }
    }

    // =========================================================================
    // SECTION 2: BODY SECTION (Faceplate preview badge)
    // =========================================================================
    const float extraH = std::max(0.0f, cardHeight - ui::px(224.0f));
    const float previewW = ui::px(224.0f);
    const float previewH = ui::px(56.0f) + extraH * 0.38f;
    const float knobY = ui::px(34.0f) + previewH + ui::px(4.0f) + extraH * 0.12f;
    const float knobR = (cardHeight > ui::px(260.0f)) ? ui::px(12.0f) : ui::px(11.0f);
    const float trimY = (cardHeight > ui::px(224.0f)) ? (cardHeight - ui::px(36.0f)) : ui::px(182.0f);
    const float mixY = (cardHeight > ui::px(224.0f)) ? (trimY - ui::px(30.0f)) : ui::px(152.0f);

    {
        ImGui::SetCursorPos(ImVec2(ui::px(8.0f), ui::px(34.0f)));
        const ImVec2 previewPos = ImGui::GetCursorScreenPos();

        // Multi-layer Frame Glow Border
        ImU32 frameBg = isFaulted ? static_cast<ImU32>(tokens.surfaces.cardBgFaulted) :
                        (bypassed ? static_cast<ImU32>(tokens.surfaces.cardBgBypassed) : static_cast<ImU32>(tokens.surfaces.frameBg));
        dl->AddRectFilled(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), frameBg, 5.0f);

        // Ambient outer glow layer
        if (isFaulted) {
            dl->AddRect(ImVec2(previewPos.x - 2.0f, previewPos.y - 2.0f),
                        ImVec2(previewPos.x + previewW + 2.0f, previewPos.y + previewH + 2.0f),
                        tokens.signal.faulted.withAlpha(0.25f), 7.0f, 0, 2.0f);
        } else if (!bypassed) {
            dl->AddRect(ImVec2(previewPos.x - 1.5f, previewPos.y - 1.5f),
                        ImVec2(previewPos.x + previewW + 1.5f, previewPos.y + previewH + 1.5f),
                        tokens.signal.active.withAlpha(0.22f), 6.5f, 0, 2.0f);
        }

        // Invisible button to capture clicks over the entire preview area (only when active)
        bool previewClicked = false;
        bool isPreviewHovered = false;
        if (!isFaulted && !isMissing) {
            ImGui::SetCursorScreenPos(previewPos);
            char previewBtnId[64];
            std::snprintf(previewBtnId, sizeof(previewBtnId), "##GuiPreview_%d_%d_%d", slotIndex, blockIndex, branchIndex);
            previewClicked = ImGui::InvisibleButton(previewBtnId, ImVec2(previewW, previewH));
            isPreviewHovered = ImGui::IsItemHovered();

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                CardDragPayload payload{slotIndex, blockIndex, branchIndex};
                ImGui::SetDragDropPayload("RACK_CARD_PAYLOAD", &payload, sizeof(payload));
                const char* loc = (branchIndex == -1) ? "Rack" : (branchIndex == 0 ? "Branch A" : "Branch B");
                ImGui::Text("Moving %s (%s, Slot %d)", slot->name().c_str(), loc, slotIndex + 1);
                ImGui::EndDragDropSource();
            }

            if (isPreviewHovered) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
        }

        // Preview rendering:
        if (isLoading) {
            const float t = static_cast<float>(ImGui::GetTime());
            const float pulse = 0.55f + 0.45f * std::sin(5.0f * t);

            // Animated subtle loading arc spinner
            const float centerX = previewPos.x + previewW * 0.5f;
            const float centerY = previewPos.y + previewH * 0.35f;
            const float radius = ui::px(10.0f);

            const int numSegments = 16;
            const float startAngle = t * 6.0f;
            const float arcLength = 4.71239f; // 270 degrees
            dl->PathClear();
            for (int i = 0; i <= numSegments; ++i) {
                float a = startAngle + (static_cast<float>(i) / static_cast<float>(numSegments)) * arcLength;
                dl->PathLineTo(ImVec2(centerX + std::cos(a) * radius, centerY + std::sin(a) * radius));
            }
            dl->PathStroke(tokens.borders.focus.withAlpha(pulse), 0, 2.0f);

            const char* loadTitle = "LOADING PLUGIN...";
            ImVec2 tSz = ImGui::CalcTextSize(loadTitle);
            dl->AddText(ImVec2(previewPos.x + (previewW - tSz.x) * 0.5f, previewPos.y + previewH - ui::px(24.0f)),
                        tokens.signal.active, loadTitle);

            const char* loadSub = "Dry signal pass-through active";
            ImVec2 sSz = ImGui::CalcTextSize(loadSub);
            dl->AddText(ImVec2(previewPos.x + (previewW - sSz.x) * 0.5f, previewPos.y + previewH - ui::px(11.0f)),
                        tokens.text.secondary, loadSub);
        } else if (isMissing) {
            const char* missTitle = "MISSING PLUGIN";
            ImVec2 tSz = ImGui::CalcTextSize(missTitle);
            dl->AddText(ImVec2(previewPos.x + (previewW - tSz.x) * 0.5f, previewPos.y + 4.0f),
                        tokens.signal.faulted, missTitle);

            const char* missSub = "Dry signal pass-through active";
            ImVec2 sSz = ImGui::CalcTextSize(missSub);
            dl->AddText(ImVec2(previewPos.x + (previewW - sSz.x) * 0.5f, previewPos.y + 18.0f),
                        tokens.text.secondary, missSub);

            ImGui::SetCursorScreenPos(ImVec2(previewPos.x + (previewW - 140.0f) * 0.5f, previewPos.y + previewH - 23.0f));
            char replaceBtnId[48];
            std::snprintf(replaceBtnId, sizeof(replaceBtnId), "FIND REPLACEMENT##%d_%d_%d", slotIndex, blockIndex, branchIndex);
            if (ImGui::Button(replaceBtnId, ImVec2(140.0f, 18.0f))) {
                m_pluginBrowserModal->open(blockIndex, branchIndex);
            }
        } else if (isFaulted) {
            const char* crashTitle = "PLUGIN CRASH ISOLATED";
            ImVec2 tSz = ImGui::CalcTextSize(crashTitle);
            dl->AddText(ImVec2(previewPos.x + (previewW - tSz.x) * 0.5f, previewPos.y + 4.0f),
                        tokens.signal.faulted, crashTitle);

            const char* crashSub = "Dry signal pass-through active";
            ImVec2 sSz = ImGui::CalcTextSize(crashSub);
            dl->AddText(ImVec2(previewPos.x + (previewW - sSz.x) * 0.5f, previewPos.y + 18.0f),
                        tokens.text.secondary, crashSub);

            ImGui::SetCursorScreenPos(ImVec2(previewPos.x + (previewW - 130.0f) * 0.5f, previewPos.y + previewH - 23.0f));
            char reloadBtnId[48];
            std::snprintf(reloadBtnId, sizeof(reloadBtnId), "RELOAD PLUGIN##%d_%d_%d", slotIndex, blockIndex, branchIndex);
            if (ImGui::Button(reloadBtnId, ImVec2(130.0f, 18.0f))) {
                m_graph.executeWithAudioFence([&]() {
                    pluginInst->resetFault();
                    slot->reset();
                });
            }
        } else {
            bool drawnThumbnail = false;
            if (m_config.enableLiveThumbnails && m_thumbnailMgr && pluginInst) {
                Thumbnail* thumb = m_thumbnailMgr->getThumbnail(slot->name());
                if (!thumb && !pluginInst->path().empty()) {
                    thumb = m_thumbnailMgr->getThumbnail(pluginInst->path());
                }
                if (thumb && thumb->srv) {
                    dl->AddImage(reinterpret_cast<ImTextureID>(thumb->srv), previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH));
                    drawnThumbnail = true;
                }
            }
            if (!drawnThumbnail) {
                // Stylized vector hardware faceplate badge
                std::string vendor = pluginInst ? pluginInst->vendor() : "Praccy Audio";
                DeviceCategory cat = detectDeviceCategory(slot->name(), vendor);
                DrawDeviceFaceplateBadge(dl, previewPos, ImVec2(previewW, previewH), cat, slot->name(), vendor, bypassed, isPreviewHovered, isWindowOpen);
            }
        }

        // Crisp inner border stroke
        ImU32 frameBorder = isLoading ? static_cast<ImU32>(tokens.borders.focus.withAlpha(0.6f + 0.4f * std::sin(6.0f * static_cast<float>(ImGui::GetTime())))) :
                            ((isFaulted || isMissing) ? static_cast<ImU32>(tokens.borders.cardFaulted) :
                            (isWindowOpen ? static_cast<ImU32>(tokens.borders.cardGlowOpen) :
                            (isPreviewHovered ? static_cast<ImU32>(tokens.borders.cardHovered) :
                            (bypassed ? static_cast<ImU32>(tokens.borders.cardBypassed) : static_cast<ImU32>(tokens.borders.card)))));
        dl->AddRect(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH), frameBorder, 5.0f, 0, 1.5f);

        if (previewClicked && !isFaulted && !isMissing) {
            if (isLoading) {
                triggerHudToast("LOADING IN BACKGROUND... WILL ACTIVATE SHORTLY");
            } else {
                m_focusedSlot = slot;
                m_focusedSlotIndex = slotIndex;
                m_focusedBlockIndex = blockIndex;
                m_focusedBranchIndex = branchIndex;
                if (hasGui) {
                    refreshGuiParamBaseline(slot);
                    plugins::PluginWindowManager::instance().openPluginWindow(pluginInst);
                    HWND hWin = plugins::PluginWindowManager::instance().getWindow(pluginInst);
                    if (hWin && m_config.enableLiveThumbnails && m_thumbnailMgr) {
                        m_thumbnailMgr->captureWindowAsync(slot->name(), hWin, 30);
                    }
                } else {
                    m_dspTweakSlot = (m_dspTweakSlot == slot) ? nullptr : slot;
                }
                m_expandedSlot = slot;
                m_expandPulseTimer = 1.0f;
            }
        }
    }

    // =========================================================================
    // SECTION 3: 4 FRONT-PANEL MACRO MINI-KNOBS
    // =========================================================================
    {
        auto& macros = slot->macros();
        const float colW = ui::px(56.0f);

        for (int m = 0; m < 4; ++m) {
            auto& macro = macros[m];
            float colX = ui::px(8.0f) + static_cast<float>(m) * colW;

            char knobId[64];
            std::snprintf(knobId, sizeof(knobId), "##MacroKnob_%d_%d_%d_%d", slotIndex, blockIndex, branchIndex, m);
            const ImGuiID kId = ImGui::GetID(knobId);
            const bool isKnobActive = (ImGui::GetActiveID() == kId);

            // Sync current value from real source only when not actively manipulated by user
            if (!isKnobActive) {
                if (macro.bound && pluginInst) {
                    macro.currentValue = pluginInst->getParameterValue(macro.paramId);
                } else if (macro.isSlotTrim) {
                    macro.currentValue = slot->outputGainDb();
                } else if (macro.isSlotMix) {
                    macro.currentValue = slot->dryWet() * 100.0f;
                }
            }

            ImGui::SetCursorPos(ImVec2(colX + (colW - (knobR * 2.0f + ui::px(8.0f))) * 0.5f, knobY));

            HardwareKnobOptions opt;
            opt.radius = knobR;
            opt.arcThickness = 2.0f;
            opt.showValue = false;
            opt.showLabel = false;
            opt.unit = macro.isSlotTrim ? "dB" : (macro.isSlotMix ? "%" : "");

            bool changed = DrawHardwareKnob(knobId, macro.label.c_str(), &macro.currentValue,
                                            macro.minValue, macro.maxValue, macro.defaultValue,
                                            "%.1f", opt);
            const bool knobDeactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
            const bool knobDiscreteChange = changed && !ImGui::IsItemActive();

            const int effSlot = (blockIndex >= 0 ? blockIndex : slotIndex);
            const int effBranchSlot = (blockIndex >= 0 ? slotIndex : -1);
            const bool isAwaitingMacro = m_keyMapMode && m_awaitingKeyTarget.active &&
                                         m_awaitingKeyTarget.slotIndex == effSlot &&
                                         m_awaitingKeyTarget.branchIndex == branchIndex &&
                                         m_awaitingKeyTarget.branchSlotIndex == effBranchSlot &&
                                         m_awaitingKeyTarget.macroIndex == m;

            if (m_keyMapMode && ImGui::IsItemClicked()) {
                m_awaitingKeyTarget.active = true;
                m_awaitingKeyTarget.slotIndex = effSlot;
                m_awaitingKeyTarget.blockIndex = blockIndex;
                m_awaitingKeyTarget.branchIndex = branchIndex;
                m_awaitingKeyTarget.branchSlotIndex = effBranchSlot;
                m_awaitingKeyTarget.macroIndex = m;
                if (macro.isSlotTrim) {
                    m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::SlotOutputGain);
                    m_awaitingKeyTarget.paramId = 0;
                } else if (macro.isSlotMix) {
                    m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::SlotDryWet);
                    m_awaitingKeyTarget.paramId = 0;
                } else if (macro.bound) {
                    m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::PluginParameter);
                    m_awaitingKeyTarget.paramId = macro.paramId;
                } else {
                    m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::PluginParameter);
                    m_awaitingKeyTarget.paramId = static_cast<uint32_t>(m);
                }
                m_awaitingKeyTarget.targetLabel = slot->name() + " " + macro.label;
                triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
            } else if (m_midiMapMode && ImGui::IsItemClicked()) {
                if (macro.isSlotTrim) {
                    m_midi.startLearning(midi::BindingTargetType::SlotOutputGain, effSlot, branchIndex, -1, effBranchSlot, 0);
                } else if (macro.isSlotMix) {
                    m_midi.startLearning(midi::BindingTargetType::SlotDryWet, effSlot, branchIndex, -1, effBranchSlot, 0);
                } else {
                    m_midi.startLearning(midi::BindingTargetType::PluginParameter, effSlot, branchIndex, -1, effBranchSlot, macro.paramId);
                }
                triggerHudToast("MIDI LEARN: " + slot->name() + " " + macro.label);
            } else if (!m_keyMapMode && changed) {
                if (macro.bound) {
                    slot->setParameter(macro.paramId, macro.currentValue);
                    if (pluginInst) {
                        pluginInst->setParameterValue(macro.paramId, macro.currentValue);
                        pluginInst->clearGuiParamEdit();
                    }
                } else if (macro.isSlotTrim) {
                    slot->setOutputGainDb(macro.currentValue);
                } else if (macro.isSlotMix) {
                    slot->setDryWet(macro.currentValue / 100.0f);
                }
                slot->macros()[static_cast<size_t>(m)].currentValue = macro.currentValue;
                m_slotMacros[slot][static_cast<size_t>(m)].currentValue = macro.currentValue;
                refreshGuiParamBaseline(slot);
            }

            if (isAwaitingMacro) {
                float t = static_cast<float>(ImGui::GetTime());
                float pulse = 0.5f + 0.5f * std::sin(8.0f * t);
                ImU32 highlightCol = tokens.borders.mapModeViolet.withAlpha(0.6f + 0.4f * pulse);
                ImVec2 rMin = ImGui::GetItemRectMin();
                ImVec2 rMax = ImGui::GetItemRectMax();
                dl->AddRect(ImVec2(rMin.x - 2.0f, rMin.y - 2.0f), ImVec2(rMax.x + 2.0f, rMax.y + 2.0f), highlightCol, 4.0f, 0, 2.0f);
            }

            if (!m_keyMapMode && !m_midiMapMode && (knobDeactivatedAfterEdit || knobDiscreteChange)) {
                refreshGuiParamBaseline(slot);
                const std::string targetKey = slot->name() + ":" + macro.label;
                const bool skipBinary = m_undoManager.shouldCoalesce(state::UndoActionType::ParameterTweak, targetKey);
                m_undoManager.pushSnapshot(
                    state::SceneManager::captureGraphSnapshot(m_graph, "Tweak " + macro.label, !skipBinary),
                    "Macro: " + macro.label,
                    state::UndoActionType::ParameterTweak,
                    targetKey
                );
                if (knobDeactivatedAfterEdit) {
                    m_undoManager.commitActiveTransaction();
                }
            }

            char macroPopupId[64];
            std::snprintf(macroPopupId, sizeof(macroPopupId), "MacroCtx_%d_%d_%d_%d", slotIndex, blockIndex, branchIndex, m);
            if (ImGui::BeginPopupContextItem(macroPopupId)) {
                ImGui::TextColored(tokens.text.accent.vec4, "REASSIGN MACRO %d", m + 1);
                ImGui::Separator();

                bool reassigned = false;
                if (pluginInst) {
                    size_t pCount = pluginInst->numParameters();
                    for (size_t p = 0; p < pCount; ++p) {
                        auto desc = pluginInst->getParameterDesc(p);
                        bool isSel = macro.bound && (macro.paramId == desc.id);
                        if (ImGui::Selectable(desc.name.c_str(), isSel)) {
                            macro.bound = true;
                            macro.isSlotMix = false;
                            macro.isSlotTrim = false;
                            macro.paramId = desc.id;
                            macro.label = desc.name;
                            macro.minValue = desc.minValue;
                            macro.maxValue = desc.maxValue;
                            macro.defaultValue = desc.defaultValue;
                            macro.currentValue = pluginInst->getParameterValue(desc.id);
                            reassigned = true;
                        }
                    }
                    ImGui::Separator();
                }

                if (ImGui::Selectable("Slot Dry/Wet Mix", macro.isSlotMix)) {
                    macro.bound = false;
                    macro.isSlotMix = true;
                    macro.isSlotTrim = false;
                    macro.label = "MIX";
                    macro.minValue = 0.0f;
                    macro.maxValue = 100.0f;
                    macro.defaultValue = 100.0f;
                    macro.currentValue = slot->dryWet() * 100.0f;
                    reassigned = true;
                }
                if (ImGui::Selectable("Slot Output Trim", macro.isSlotTrim)) {
                    macro.bound = false;
                    macro.isSlotMix = false;
                    macro.isSlotTrim = true;
                    macro.label = "TRIM";
                    macro.minValue = -24.0f;
                    macro.maxValue = 12.0f;
                    macro.defaultValue = 0.0f;
                    macro.currentValue = slot->outputGainDb();
                    reassigned = true;
                }

                if (reassigned) {
                    m_slotMacros[slot][m] = macro;
                    m_undoManager.pushSnapshot(
                        state::SceneManager::captureGraphSnapshot(m_graph, "Reassign Macro " + macro.label),
                        "Reassign Macro: " + macro.label,
                        state::UndoActionType::ParameterTweak,
                        slot->name() + ":MacroAssign",
                        true
                    );
                    m_undoManager.commitActiveTransaction();
                }

                ImGui::EndPopup();
            }

            // Two-tier typography:
            // Tier 1: Monospace value readout in g_fontMono
            char valBuf[32];
            const char* unitStr = macro.isSlotTrim ? "dB" : (macro.isSlotMix ? "%" : "");
            if (unitStr[0] != '\0') {
                char nBuf[24];
                std::snprintf(nBuf, sizeof(nBuf), "%.1f", macro.currentValue);
                std::snprintf(valBuf, sizeof(valBuf), "%s%s", nBuf, unitStr);
            } else {
                std::snprintf(valBuf, sizeof(valBuf), "%.1f", macro.currentValue);
            }
            const float valY = knobY + knobR * 2.0f + ui::px(6.0f);
            const float lblY = valY + ui::px(12.0f);

            if (g_fontMono) ImGui::PushFont(g_fontMono);
            ImVec2 vSz = ImGui::CalcTextSize(valBuf);
            ImGui::SetCursorPos(ImVec2(colX + (colW - vSz.x) * 0.5f, valY));
            ImGui::TextColored(tokens.text.primary.vec4, "%s", valBuf);
            if (g_fontMono) ImGui::PopFont();

            // Tier 2: Pro-audio parameter label centered beneath value (disambiguated if duplicate prefix)
            std::string dLabel = formatMacroLabel(macro.label, m);
            for (int prev = 0; prev < m; ++prev) {
                if (formatMacroLabel(macros[prev].label, prev) == dLabel && dLabel.size() >= 4) {
                    char mSuffix[4] = {' ', static_cast<char>('1' + m), '\0'};
                    dLabel = dLabel.substr(0, 4) + mSuffix;
                    break;
                }
            }
            ImVec2 lSz = ImGui::CalcTextSize(dLabel.c_str());
            ImGui::SetCursorPos(ImVec2(colX + (colW - lSz.x) * 0.5f, lblY));
            ImGui::TextColored(tokens.text.secondary.vec4, "%s", dLabel.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s (Macro %d)", macro.label.c_str(), m + 1);
            }
        }
    }

    // =========================================================================
    // SECTION 4: TACTILE MIX SLIDER & MINI METER
    // =========================================================================
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.5f));
        ImGui::SetCursorPos(ImVec2(ui::px(8.0f), mixY));

        float mix = slot->dryWet() * 100.0f;
        char mixLabel[32];
        std::snprintf(mixLabel, sizeof(mixLabel), "##Mix_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        ImGui::SetNextItemWidth(ui::px(190.0f));
        const bool mixChanged = ResettableSliderFloat(mixLabel, &mix, 0.0f, 100.0f, 100.0f, "Mix: %.0f%%");
        const bool mixDeactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
        const bool mixDiscreteChange = mixChanged && !ImGui::IsItemActive();
        const int effSlot = (blockIndex >= 0 ? blockIndex : slotIndex);
        const int effBranchSlot = (blockIndex >= 0 ? slotIndex : -1);
        const bool isAwaitingMix = m_keyMapMode && m_awaitingKeyTarget.active &&
                                   m_awaitingKeyTarget.slotIndex == effSlot &&
                                   m_awaitingKeyTarget.branchIndex == branchIndex &&
                                   m_awaitingKeyTarget.branchSlotIndex == effBranchSlot &&
                                   m_awaitingKeyTarget.targetType == static_cast<uint32_t>(midi::BindingTargetType::SlotDryWet);

        if (m_keyMapMode && (mixChanged || ImGui::IsItemClicked())) {
            m_awaitingKeyTarget.active = true;
            m_awaitingKeyTarget.slotIndex = effSlot;
            m_awaitingKeyTarget.blockIndex = blockIndex;
            m_awaitingKeyTarget.branchIndex = branchIndex;
            m_awaitingKeyTarget.branchSlotIndex = effBranchSlot;
            m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::SlotDryWet);
            m_awaitingKeyTarget.paramId = 0;
            m_awaitingKeyTarget.targetLabel = slot->name() + " Mix";
            triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
        } else if (mixChanged) {
            if (m_midiMapMode) {
                m_midi.startLearning(midi::BindingTargetType::SlotDryWet, effSlot, branchIndex, -1, effBranchSlot);
                triggerHudToast("MIDI LEARN: " + slot->name() + " MIX");
            } else {
                slot->setDryWet(mix / 100.0f);
                for (size_t m = 0; m < 4; ++m) {
                    if (slot->macros()[m].isSlotMix) {
                        slot->macros()[m].currentValue = mix;
                        m_slotMacros[slot][m].currentValue = mix;
                    }
                }
            }
        } else if (m_midiMapMode && ImGui::IsItemClicked()) {
            m_midi.startLearning(midi::BindingTargetType::SlotDryWet, effSlot, branchIndex, -1, effBranchSlot);
            triggerHudToast("MIDI LEARN: " + slot->name() + " MIX");
        }

        if (isAwaitingMix) {
            float t = static_cast<float>(ImGui::GetTime());
            float pulse = 0.5f + 0.5f * std::sin(8.0f * t);
            ImU32 highlightCol = tokens.borders.mapModeViolet.withAlpha(0.6f + 0.4f * pulse);
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            dl->AddRect(ImVec2(rMin.x - 2.0f, rMin.y - 2.0f), ImVec2(rMax.x + 2.0f, rMax.y + 2.0f), highlightCol, 4.0f, 0, 2.0f);
        }

        if (!m_keyMapMode && !m_midiMapMode && (mixDeactivatedAfterEdit || mixDiscreteChange)) {
            m_undoManager.pushSnapshot(
                state::SceneManager::captureGraphSnapshot(m_graph, "Mix " + slot->name()),
                "Mix: " + slot->name(),
                state::UndoActionType::ParameterTweak,
                slot->name() + ":Mix"
            );
            if (mixDeactivatedAfterEdit) {
                m_undoManager.commitActiveTransaction();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Dry/Wet Mix (Double-click to reset 100%%)");

        ImGui::SameLine(0, ui::px(6.0f));
        const float slotMeterW = ui::px(8.0f);
        const float slotMeterH = ui::px(24.0f);
        bool slotClipped = false;
        renderMeter("SlotMtr", slot->meter().peakLeft(), slot->meter().holdLeft(),
                    slot->meter().isClipped(), slotMeterW, slotMeterH, &slotClipped);
        if (slotClipped) {
            slot->meter().resetClip();
        }
    }

    // =========================================================================
    // SECTION 5: TACTILE OUTPUT TRIM SLIDER
    // =========================================================================
    {
        ImGui::SetCursorPos(ImVec2(ui::px(8.0f), trimY));
        float outTrim = slot->outputGainDb();
        char trimLabel[32];
        std::snprintf(trimLabel, sizeof(trimLabel), "##Trim_%d_%d_%d", slotIndex, blockIndex, branchIndex);
        ImGui::SetNextItemWidth(ui::px(206.0f));
        const bool trimChanged = ResettableSliderFloat(trimLabel, &outTrim, -24.0f, +12.0f, 0.0f, "Trim: %+.1f dB");
        const bool trimDeactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
        const bool trimDiscreteChange = trimChanged && !ImGui::IsItemActive();

        const int effSlot = (blockIndex >= 0 ? blockIndex : slotIndex);
        const int effBranchSlot = (blockIndex >= 0 ? slotIndex : -1);
        const bool isAwaitingTrim = m_keyMapMode && m_awaitingKeyTarget.active &&
                                    m_awaitingKeyTarget.slotIndex == effSlot &&
                                    m_awaitingKeyTarget.branchIndex == branchIndex &&
                                    m_awaitingKeyTarget.branchSlotIndex == effBranchSlot &&
                                    m_awaitingKeyTarget.targetType == static_cast<uint32_t>(midi::BindingTargetType::SlotOutputGain);

        if (m_keyMapMode && (trimChanged || ImGui::IsItemClicked())) {
            m_awaitingKeyTarget.active = true;
            m_awaitingKeyTarget.slotIndex = effSlot;
            m_awaitingKeyTarget.blockIndex = blockIndex;
            m_awaitingKeyTarget.branchIndex = branchIndex;
            m_awaitingKeyTarget.branchSlotIndex = effBranchSlot;
            m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::SlotOutputGain);
            m_awaitingKeyTarget.paramId = 0;
            m_awaitingKeyTarget.targetLabel = slot->name() + " Trim";
            triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
        } else if (trimChanged) {
            if (m_midiMapMode) {
                m_midi.startLearning(midi::BindingTargetType::SlotOutputGain, effSlot, branchIndex, -1, effBranchSlot);
                triggerHudToast("MIDI LEARN: " + slot->name() + " TRIM");
            } else {
                slot->setOutputGainDb(outTrim);
                for (size_t m = 0; m < 4; ++m) {
                    if (slot->macros()[m].isSlotTrim) {
                        slot->macros()[m].currentValue = outTrim;
                        m_slotMacros[slot][m].currentValue = outTrim;
                    }
                }
            }
        } else if (m_midiMapMode && ImGui::IsItemClicked()) {
            m_midi.startLearning(midi::BindingTargetType::SlotOutputGain, effSlot, branchIndex, -1, effBranchSlot);
            triggerHudToast("MIDI LEARN: " + slot->name() + " TRIM");
        }

        if (isAwaitingTrim) {
            float t = static_cast<float>(ImGui::GetTime());
            float pulse = 0.5f + 0.5f * std::sin(8.0f * t);
            ImU32 highlightCol = tokens.borders.mapModeViolet.withAlpha(0.6f + 0.4f * pulse);
            ImVec2 rMin = ImGui::GetItemRectMin();
            ImVec2 rMax = ImGui::GetItemRectMax();
            dl->AddRect(ImVec2(rMin.x - 2.0f, rMin.y - 2.0f), ImVec2(rMax.x + 2.0f, rMax.y + 2.0f), highlightCol, 4.0f, 0, 2.0f);
        }

        if (!m_keyMapMode && !m_midiMapMode && (trimDeactivatedAfterEdit || trimDiscreteChange)) {
            m_undoManager.pushSnapshot(
                state::SceneManager::captureGraphSnapshot(m_graph, "Trim " + slot->name()),
                "Trim: " + slot->name(),
                state::UndoActionType::ParameterTweak,
                slot->name() + ":Trim"
            );
            if (trimDeactivatedAfterEdit) {
                m_undoManager.commitActiveTransaction();
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output Trim (Double-click to reset 0.0 dB)");
        ImGui::PopStyleVar(); // FramePadding
    }

    // Context Menu for MIDI Learn & Options
    char popupId[64];
    std::snprintf(popupId, sizeof(popupId), "SlotCtx_%d_%d_%d", slotIndex, blockIndex, branchIndex);
    if (ImGui::BeginPopupContextItem(popupId)) {
        if (hasGui) {
            if (ImGui::MenuItem("Open Plugin GUI Window")) {
                refreshGuiParamBaseline(slot);
                plugins::PluginWindowManager::instance().openPluginWindow(pluginInst);
                HWND hWin = plugins::PluginWindowManager::instance().getWindow(pluginInst);
                if (hWin && m_config.enableLiveThumbnails && m_thumbnailMgr) {
                    m_thumbnailMgr->captureWindowAsync(slot->name(), hWin, 30);
                }
            }
            HWND hWin = plugins::PluginWindowManager::instance().getWindow(pluginInst);
            if (hWin && ImGui::MenuItem("Capture GUI Preview Screenshot")) {
                if (m_config.enableLiveThumbnails && m_thumbnailMgr) {
                    m_thumbnailMgr->captureWindowAsync(slot->name(), hWin, 10);
                }
            }
            ImGui::Separator();
        } else if (pluginInst) {
            if (ImGui::MenuItem("Edit DSP Parameters")) {
                m_dspTweakSlot = slot;
            }
            ImGui::Separator();
        }
        const int effSlot = (blockIndex >= 0 ? blockIndex : slotIndex);
        const int effBranchSlot = (blockIndex >= 0 ? slotIndex : -1);
        if (ImGui::MenuItem("MIDI Learn: Bypass Toggle")) {
            m_midi.startLearning(midi::BindingTargetType::SlotBypass, effSlot, branchIndex, -1, effBranchSlot);
        }
        if (ImGui::MenuItem("MIDI Learn: Dry/Wet Mix")) {
            m_midi.startLearning(midi::BindingTargetType::SlotDryWet, effSlot, branchIndex, -1, effBranchSlot);
        }
        if (ImGui::MenuItem("MIDI Learn: Output Trim")) {
            m_midi.startLearning(midi::BindingTargetType::SlotOutputGain, effSlot, branchIndex, -1, effBranchSlot);
        }
        if (ImGui::MenuItem("Delete Slot")) {
            if (hasGui) plugins::PluginWindowManager::instance().closePluginWindow(pluginInst);
            if (m_dspTweakSlot == slot) m_dspTweakSlot = nullptr;
            if (m_expandedSlot == slot) m_expandedSlot = nullptr;
            if (m_pendingGuiEditSlot == slot) m_pendingGuiEditSlot = nullptr;
            if (m_focusedSlot == slot) {
                m_focusedSlot = nullptr;
                m_focusedSlotIndex = -1;
                m_focusedBlockIndex = -1;
                m_focusedBranchIndex = -1;
            }
            m_guiParamBaseline.erase(slot);
            m_slotMacros.erase(slot);
            if (branchIndex == -1) {
                m_graph.removeSerialNode(slotIndex);
            } else {
                auto* blk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(blockIndex));
                if (blk) {
                    auto* br = blk->getBranch(branchIndex);
                    if (br) br->removeSlot(slotIndex);
                }
            }
            m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Delete Slot"),
                                       "Delete Slot",
                                       state::UndoActionType::StructuralMutation);
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(); // WindowPadding
    ImGui::PopStyleColor(); // ChildBg

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        m_focusedSlot = slot;
        m_focusedSlotIndex = slotIndex;
        m_focusedBlockIndex = blockIndex;
        m_focusedBranchIndex = branchIndex;
    }

    if (m_focusedSlot == slot) {
        ImDrawList* fgDl = ImGui::GetWindowDrawList();
        fgDl->AddRect(cardScreenMin, ImVec2(cardScreenMin.x + cardWidth, cardScreenMin.y + cardHeight),
                      tokens.borders.focus.u32, 6.0f, 0, 2.0f);
    }

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("RACK_CARD_PAYLOAD")) {
            const auto* pl = static_cast<const CardDragPayload*>(p->Data);
            if (pl && pl->sourceIndex >= 0) {
                moveSlot(pl->sourceIndex, pl->sourceBlockIndex, pl->sourceBranchIndex,
                         slotIndex, blockIndex, branchIndex);
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::EndGroup();
}

void RackView::renderParallelBlock(audio::ParallelSplitMergeBlock* block, int blockIndex, float centerY) {
    if (!block) return;

    ImGui::PushID(blockIndex);
    const auto& tokens = themeTokens();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 winPos = ImGui::GetWindowPos();
    float scrollX = ImGui::GetScrollX();
    float scrollY = ImGui::GetScrollY();

    auto toScreen = [&](float lx, float ly) -> ImVec2 {
        return ImVec2(winPos.x + lx - scrollX, winPos.y + ly - scrollY);
    };

    const float cardW = ui::px(240.0f);
    const float cardH = ui::px(224.0f);
    const float slotWireW = ui::px(28.0f);
    const float insertCardW = ui::px(80.0f);
    const float envHeaderH = ui::px(36.0f);
    const float envPadX = ui::px(14.0f);
    const float envPadY = ui::px(10.0f);
    const float envH = envHeaderH + (2.0f * envPadY) + cardH; // 280.0f

    auto* br0 = block->getBranch(0);
    auto* br1 = block->getBranch(1);
    const size_t numSlots0 = br0 ? br0->numSlots() : 0;
    const size_t numSlots1 = br1 ? br1->numSlots() : 0;

    // Calculate dynamic envelope width based on number of slots in each branch
    const float envelopeW = calculateParallelEnvelopeWidth(numSlots0, numSlots1);

    const uint64_t key0 = (static_cast<uint64_t>(static_cast<uint32_t>(blockIndex)) << 32) | 0ULL;
    const uint64_t key1 = (static_cast<uint64_t>(static_cast<uint32_t>(blockIndex)) << 32) | 1ULL;
    bool min0 = m_branchMinimized[key0];
    bool min1 = m_branchMinimized[key1];

    const float chassisH0 = min0 ? envHeaderH : envH;
    const float chassisH1 = min1 ? envHeaderH : envH;
    const float branchGap = ui::px(20.0f);
    const float totalBlockH = chassisH0 + branchGap + chassisH1;

    // Symmetrically center the parallel envelopes around centerY
    const float envY0 = centerY - (totalBlockH * 0.5f);
    const float envY1 = envY0 + chassisH0 + branchGap;

    const float card0Top = envY0 + envHeaderH + envPadY;
    const float card1Top = envY1 + envHeaderH + envPadY;

    const float card0MidY = card0Top + (cardH * 0.5f);
    const float card1MidY = card1Top + (cardH * 0.5f);

    const float startX = ImGui::GetCursorPosX();
    const float forkLeadW = ui::px(36.0f);
    const float forkStartX = startX;
    const float envX = forkStartX + forkLeadW;

    const float port0Y = min0 ? (envY0 + envHeaderH * 0.5f) : card0MidY;
    const float port1Y = min1 ? (envY1 + envHeaderH * 0.5f) : card1MidY;

    int pendingDissolveKeepBranch = -99;

    // 1. Split Fork Junction & Wires entering Envelope Left Ports
    dl->AddCircleFilled(toScreen(forkStartX, centerY), 4.5f, tokens.cables.core);
    drawRoutingWire(dl, toScreen(forkStartX, centerY), toScreen(envX, port0Y), false);
    drawRoutingWire(dl, toScreen(forkStartX, centerY), toScreen(envX, port1Y), false);

    // Left Input Ports on Envelopes
    dl->AddCircleFilled(toScreen(envX, port0Y), 6.0f, tokens.cables.socketRing);
    dl->AddCircleFilled(toScreen(envX, port0Y), 3.5f, tokens.signal.branchA);

    dl->AddCircleFilled(toScreen(envX, port1Y), 6.0f, tokens.cables.socketRing);
    dl->AddCircleFilled(toScreen(envX, port1Y), 3.5f, tokens.signal.branchB);

    // =========================================================================
    // 2. BRANCH A ENVELOPE (Top Chassis)
    // =========================================================================
    {
        // Envelope Background & Border (Cool Cyan / Navy theme)
        dl->AddRectFilled(toScreen(envX, envY0), toScreen(envX + envelopeW, envY0 + chassisH0), tokens.surfaces.branchABg, 8.0f);
        dl->AddRect(toScreen(envX, envY0), toScreen(envX + envelopeW, envY0 + chassisH0), tokens.borders.branchABorder, 8.0f, 0, 1.5f);

        // Header Background Bar (Rounded top corners, or fully rounded if minimized)
        ImDrawFlags roundFlags0 = min0 ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop;
        dl->AddRectFilled(toScreen(envX, envY0), toScreen(envX + envelopeW, envY0 + envHeaderH), tokens.surfaces.branchAHeader, 8.0f, roundFlags0);
        if (!min0) {
            dl->AddLine(toScreen(envX, envY0 + envHeaderH), toScreen(envX + envelopeW, envY0 + envHeaderH), tokens.borders.branchABorder, 1.0f);
        }

        // Header Controls
        if (br0) {
            const float headerY0 = envY0 + ui::px(6.0f);
            const float rightBtnsW = ui::px(24.0f) + ui::px(4.0f) + ui::px(24.0f); // Minimise (24) + spacing (4) + Delete (24)
            const float rightMargin = ui::px(14.0f);
            const float btnAreaX0 = envX + envelopeW - rightBtnsW - rightMargin;
            const float headerSafetyMargin = ui::px(16.0f);

            ImGui::SetCursorPos(ImVec2(envX + ui::px(12.0f), headerY0));
            ImGui::BeginGroup();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(6.0f), ui::px(3.5f)));

            // Branch A Badge Pill
            renderBadgePill("BRANCH A", tokens.surfaces.branchABg, tokens.borders.branchABorder, tokens.signal.branchA, ui::px(24.0f));
            if (ImGui::IsItemClicked()) {
                m_focusedSlot = nullptr;
                m_focusedSlotIndex = blockIndex;
                m_focusedBlockIndex = blockIndex;
                m_focusedBranchIndex = 0;
            }
            ImGui::SameLine(0, ui::px(8.0f));

            char countBuf[32];
            std::snprintf(countBuf, sizeof(countBuf), "(%zu %s)", numSlots0, numSlots0 == 1 ? "Slot" : "Slots");
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", countBuf);
            ImGui::SameLine(0, ui::px(10.0f));

            // Mute Button
            bool m0 = br0->isMuted();
            const bool isAwaitingMute0 = m_keyMapMode && m_awaitingKeyTarget.active &&
                                         m_awaitingKeyTarget.slotIndex == blockIndex &&
                                         m_awaitingKeyTarget.branchIndex == 0 &&
                                         m_awaitingKeyTarget.targetType == static_cast<uint32_t>(midi::BindingTargetType::BranchMute);
            {
                ScopedStyleColor muteGuard(ImGuiCol_Button, tokens.signal.faulted.vec4, m0);
                if (CenteredButton("M##br0", ImVec2(ui::px(24.0f), ui::px(24.0f)))) {
                    if (m_keyMapMode) {
                        m_awaitingKeyTarget.active = true;
                        m_awaitingKeyTarget.slotIndex = blockIndex;
                        m_awaitingKeyTarget.blockIndex = blockIndex;
                        m_awaitingKeyTarget.branchIndex = 0;
                        m_awaitingKeyTarget.branchSlotIndex = -1;
                        m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::BranchMute);
                        m_awaitingKeyTarget.paramId = 0;
                        m_awaitingKeyTarget.targetLabel = "Branch A Mute";
                        triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
                    } else {
                        br0->setMuted(!m0);
                        m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Branch A Mute"),
                                                   "Branch A Mute", state::UndoActionType::ParameterTweak, "Br0:Mute", true);
                        m_undoManager.commitActiveTransaction();
                    }
                } else if (m_keyMapMode && ImGui::IsItemClicked()) {
                    m_awaitingKeyTarget.active = true;
                    m_awaitingKeyTarget.slotIndex = blockIndex;
                    m_awaitingKeyTarget.blockIndex = blockIndex;
                    m_awaitingKeyTarget.branchIndex = 0;
                    m_awaitingKeyTarget.branchSlotIndex = -1;
                    m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::BranchMute);
                    m_awaitingKeyTarget.paramId = 0;
                    m_awaitingKeyTarget.targetLabel = "Branch A Mute";
                    triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
                }
            }
            if (isAwaitingMute0) {
                float t = static_cast<float>(ImGui::GetTime());
                float pulse = 0.5f + 0.5f * std::sin(8.0f * t);
                ImU32 highlightCol = tokens.borders.mapModeViolet.withAlpha(0.6f + 0.4f * pulse);
                ImVec2 rMin = ImGui::GetItemRectMin();
                ImVec2 rMax = ImGui::GetItemRectMax();
                dl->AddRect(ImVec2(rMin.x - 2.0f, rMin.y - 2.0f), ImVec2(rMax.x + 2.0f, rMax.y + 2.0f), highlightCol, 4.0f, 0, 2.0f);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(m0 ? "Unmute Branch A" : "Mute Branch A");

            ImGui::SameLine(0, ui::px(4.0f));

            // Solo Button
            bool s0 = br0->isSolo();
            const bool isAwaitingSolo0 = m_keyMapMode && m_awaitingKeyTarget.active &&
                                         m_awaitingKeyTarget.slotIndex == blockIndex &&
                                         m_awaitingKeyTarget.branchIndex == 0 &&
                                         m_awaitingKeyTarget.targetType == static_cast<uint32_t>(midi::BindingTargetType::BranchSolo);
            {
                ScopedStyleColor soloGuard(ImGuiCol_Button, tokens.signal.active.vec4, s0);
                if (CenteredButton("S##br0", ImVec2(ui::px(24.0f), ui::px(24.0f)))) {
                    if (m_keyMapMode) {
                        m_awaitingKeyTarget.active = true;
                        m_awaitingKeyTarget.slotIndex = blockIndex;
                        m_awaitingKeyTarget.blockIndex = blockIndex;
                        m_awaitingKeyTarget.branchIndex = 0;
                        m_awaitingKeyTarget.branchSlotIndex = -1;
                        m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::BranchSolo);
                        m_awaitingKeyTarget.paramId = 0;
                        m_awaitingKeyTarget.targetLabel = "Branch A Solo";
                        triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
                    } else {
                        br0->setSolo(!s0);
                        m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Branch A Solo"),
                                                   "Branch A Solo", state::UndoActionType::ParameterTweak, "Br0:Solo", true);
                        m_undoManager.commitActiveTransaction();
                    }
                } else if (m_keyMapMode && ImGui::IsItemClicked()) {
                    m_awaitingKeyTarget.active = true;
                    m_awaitingKeyTarget.slotIndex = blockIndex;
                    m_awaitingKeyTarget.blockIndex = blockIndex;
                    m_awaitingKeyTarget.branchIndex = 0;
                    m_awaitingKeyTarget.branchSlotIndex = -1;
                    m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::BranchSolo);
                    m_awaitingKeyTarget.paramId = 0;
                    m_awaitingKeyTarget.targetLabel = "Branch A Solo";
                    triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
                }
            }
            if (isAwaitingSolo0) {
                float t = static_cast<float>(ImGui::GetTime());
                float pulse = 0.5f + 0.5f * std::sin(8.0f * t);
                ImU32 highlightCol = tokens.borders.mapModeViolet.withAlpha(0.6f + 0.4f * pulse);
                ImVec2 rMin = ImGui::GetItemRectMin();
                ImVec2 rMax = ImGui::GetItemRectMax();
                dl->AddRect(ImVec2(rMin.x - 2.0f, rMin.y - 2.0f), ImVec2(rMax.x + 2.0f, rMax.y + 2.0f), highlightCol, 4.0f, 0, 2.0f);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(s0 ? "Unsolo Branch A" : "Solo Branch A");

            ImGui::SameLine(0, ui::px(4.0f));

            // Phase Invert Button
            bool p0 = br0->isPhaseInvert();
            {
                ScopedStyleColor phaseGuard(ImGuiCol_Button, tokens.text.accent.vec4, p0);
                if (CenteredButton("\xC3\x98##br0", ImVec2(ui::px(24.0f), ui::px(24.0f)))) {
                    br0->setPhaseInvert(!p0);
                    m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Branch A Phase"),
                                               "Branch A Phase", state::UndoActionType::ParameterTweak, "Br0:Phase", true);
                    m_undoManager.commitActiveTransaction();
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Phase Invert (180° Polarity Flip)");

            ImGui::SameLine(0, ui::px(8.0f));

            // Dynamic slider sizing based on available envelope header width
            const float sliderStartX0 = ImGui::GetCursorPosX();
            const float availForSliders0 = (btnAreaX0 - headerSafetyMargin) - sliderStartX0;
            const float sliderGap = ui::px(6.0f);
            const float maxSliderW = ui::px(96.0f);
            const float sliderW0 = std::clamp((availForSliders0 - sliderGap) * 0.5f, ui::px(48.0f), maxSliderW);

            // Pan Slider (Double-click reset to 0.0)
            float pan0 = br0->pan();
            ImGui::SetNextItemWidth(sliderW0);
            const bool pan0Changed = ResettableSliderFloat("##Pan0", &pan0, -1.0f, 1.0f, 0.0f, "Pan: %+.2f");
            if (pan0Changed) { br0->setPan(pan0); }
            if (ImGui::IsItemDeactivatedAfterEdit() || (pan0Changed && !ImGui::IsItemActive())) {
                m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Branch A Pan"),
                                           "Branch A Pan", state::UndoActionType::ParameterTweak, "Br0:Pan");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_undoManager.commitActiveTransaction();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Branch A Pan (Double-click to center)");

            ImGui::SameLine(0, sliderGap);

            // Gain Slider (Double-click reset to 0.0dB)
            float gain0 = br0->gainDb();
            ImGui::SetNextItemWidth(sliderW0);
            const bool gain0Changed = ResettableSliderFloat("##Gain0", &gain0, -36.0f, +12.0f, 0.0f, "%+.1f dB");
            if (gain0Changed) { br0->setGainDb(gain0); }
            if (ImGui::IsItemDeactivatedAfterEdit() || (gain0Changed && !ImGui::IsItemActive())) {
                m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Branch A Gain"),
                                           "Branch A Gain", state::UndoActionType::ParameterTweak, "Br0:Gain");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_undoManager.commitActiveTransaction();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Branch A Gain (Double-click to reset 0.0 dB)");

            ImGui::PopStyleVar();
            ImGui::EndGroup();

            // Top-right controls: Minimise & Delete Branch A
            ImGui::SetCursorPos(ImVec2(btnAreaX0, headerY0));

            if (renderCenteredMinimiseButton("##minBr0", ImVec2(ui::px(24.0f), ui::px(24.0f)), min0)) {
                m_branchMinimized[key0] = !min0;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(min0 ? "Maximise Branch A" : "Minimise Branch A");

            ImGui::SameLine(0, ui::px(4.0f));
            if (renderCenteredDeleteButton("##delBr0", ImVec2(ui::px(24.0f), ui::px(24.0f)))) {
                pendingDissolveKeepBranch = 1;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete Branch A (Branch B will be kept in rack)");

            // If not minimized, render slots and internal wiring
            if (!min0) {
                float curX0 = envX + envPadX;

                // Wire from envelope entry port to first slot/insert card
                drawRoutingWire(dl, toScreen(envX, card0MidY), toScreen(curX0, card0MidY), false);

                for (size_t s = 0; s < numSlots0; ++s) {
                    auto* bSlot = br0->getSlot(s);
                    if (!bSlot) continue;

                    ImGui::SetCursorPos(ImVec2(curX0, card0Top));
                    renderPluginSlot(bSlot, static_cast<int>(s), blockIndex, 0);
                    curX0 += cardW;

                    drawRoutingWire(dl, toScreen(curX0, card0MidY), toScreen(curX0 + slotWireW, card0MidY), false);
                    curX0 += slotWireW;
                }

                // Branch A Inline Insert Button Card
                ImGui::SetCursorPos(ImVec2(curX0, card0Top));
                ImGui::BeginGroup();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.cardBg.vec4);
                char insertBr0Card[32];
                std::snprintf(insertBr0Card, sizeof(insertBr0Card), "InsertBr0Card_%d", blockIndex);
                ImGui::BeginChild(insertBr0Card, ImVec2(insertCardW, cardH), true, ImGuiWindowFlags_NoScrollbar);
                ImGui::SetCursorPos(ImVec2((insertCardW - ui::px(48.0f)) * 0.5f, (cardH - ui::px(48.0f)) * 0.5f));
                if (renderCenteredPlusButton("##AddPluginBr0", ImVec2(ui::px(48.0f), ui::px(48.0f)), ui::px(11.0f))) {
                    m_pluginBrowserModal->open(blockIndex, 0);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Insert plugin into Branch A");
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("RACK_CARD_PAYLOAD")) {
                        const auto* pl = static_cast<const CardDragPayload*>(p->Data);
                        if (pl && pl->sourceIndex >= 0) {
                            moveSlot(pl->sourceIndex, pl->sourceBlockIndex, pl->sourceBranchIndex,
                                     -1, blockIndex, 0);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::EndGroup();
                curX0 += insertCardW;

                // Wire from insert card to envelope exit port
                if (curX0 < envX + envelopeW) {
                    drawRoutingWire(dl, toScreen(curX0, card0MidY), toScreen(envX + envelopeW, card0MidY), false);
                }
            }
        }
    }

    // =========================================================================
    // 3. BRANCH B ENVELOPE (Bottom Chassis)
    // =========================================================================
    {
        // Envelope Background & Border (Warm Amber theme)
        dl->AddRectFilled(toScreen(envX, envY1), toScreen(envX + envelopeW, envY1 + chassisH1), tokens.surfaces.branchBBg, 8.0f);
        dl->AddRect(toScreen(envX, envY1), toScreen(envX + envelopeW, envY1 + chassisH1), tokens.borders.branchBBorder, 8.0f, 0, 1.5f);

        // Header Background Bar (Rounded top corners, or fully rounded if minimized)
        ImDrawFlags roundFlags1 = min1 ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersTop;
        dl->AddRectFilled(toScreen(envX, envY1), toScreen(envX + envelopeW, envY1 + envHeaderH), tokens.surfaces.branchBHeader, 8.0f, roundFlags1);
        if (!min1) {
            dl->AddLine(toScreen(envX, envY1 + envHeaderH), toScreen(envX + envelopeW, envY1 + envHeaderH), tokens.borders.branchBBorder, 1.0f);
        }

        // Header Controls
        if (br1) {
            const float headerY1 = envY1 + ui::px(6.0f);
            const float rightBtnsW = ui::px(24.0f) + ui::px(4.0f) + ui::px(24.0f); // Minimise (24) + spacing (4) + Delete (24)
            const float rightMargin = ui::px(14.0f);
            const float btnAreaX1 = envX + envelopeW - rightBtnsW - rightMargin;
            const float headerSafetyMargin = ui::px(16.0f);

            ImGui::SetCursorPos(ImVec2(envX + ui::px(12.0f), headerY1));
            ImGui::BeginGroup();
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(6.0f), ui::px(3.5f)));

            // Branch B Badge Pill
            renderBadgePill("BRANCH B", tokens.surfaces.branchBBg, tokens.borders.branchBBorder, tokens.signal.branchB, ui::px(24.0f));
            if (ImGui::IsItemClicked()) {
                m_focusedSlot = nullptr;
                m_focusedSlotIndex = blockIndex;
                m_focusedBlockIndex = blockIndex;
                m_focusedBranchIndex = 1;
            }
            ImGui::SameLine(0, ui::px(8.0f));

            char countBuf[32];
            std::snprintf(countBuf, sizeof(countBuf), "(%zu %s)", numSlots1, numSlots1 == 1 ? "Slot" : "Slots");
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", countBuf);
            ImGui::SameLine(0, ui::px(10.0f));

            // Mute Button
            bool m1 = br1->isMuted();
            const bool isAwaitingMute1 = m_keyMapMode && m_awaitingKeyTarget.active &&
                                         m_awaitingKeyTarget.slotIndex == blockIndex &&
                                         m_awaitingKeyTarget.branchIndex == 1 &&
                                         m_awaitingKeyTarget.targetType == static_cast<uint32_t>(midi::BindingTargetType::BranchMute);
            {
                ScopedStyleColor muteGuard(ImGuiCol_Button, tokens.signal.faulted.vec4, m1);
                if (CenteredButton("M##br1", ImVec2(ui::px(24.0f), ui::px(24.0f)))) {
                    if (m_keyMapMode) {
                        m_awaitingKeyTarget.active = true;
                        m_awaitingKeyTarget.slotIndex = blockIndex;
                        m_awaitingKeyTarget.blockIndex = blockIndex;
                        m_awaitingKeyTarget.branchIndex = 1;
                        m_awaitingKeyTarget.branchSlotIndex = -1;
                        m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::BranchMute);
                        m_awaitingKeyTarget.paramId = 0;
                        m_awaitingKeyTarget.targetLabel = "Branch B Mute";
                        triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
                    } else {
                        br1->setMuted(!m1);
                        m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Branch B Mute"),
                                                   "Branch B Mute", state::UndoActionType::ParameterTweak, "Br1:Mute", true);
                        m_undoManager.commitActiveTransaction();
                    }
                } else if (m_keyMapMode && ImGui::IsItemClicked()) {
                    m_awaitingKeyTarget.active = true;
                    m_awaitingKeyTarget.slotIndex = blockIndex;
                    m_awaitingKeyTarget.blockIndex = blockIndex;
                    m_awaitingKeyTarget.branchIndex = 1;
                    m_awaitingKeyTarget.branchSlotIndex = -1;
                    m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::BranchMute);
                    m_awaitingKeyTarget.paramId = 0;
                    m_awaitingKeyTarget.targetLabel = "Branch B Mute";
                    triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
                }
            }
            if (isAwaitingMute1) {
                float t = static_cast<float>(ImGui::GetTime());
                float pulse = 0.5f + 0.5f * std::sin(8.0f * t);
                ImU32 highlightCol = tokens.borders.mapModeViolet.withAlpha(0.6f + 0.4f * pulse);
                ImVec2 rMin = ImGui::GetItemRectMin();
                ImVec2 rMax = ImGui::GetItemRectMax();
                dl->AddRect(ImVec2(rMin.x - 2.0f, rMin.y - 2.0f), ImVec2(rMax.x + 2.0f, rMax.y + 2.0f), highlightCol, 4.0f, 0, 2.0f);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(m1 ? "Unmute Branch B" : "Mute Branch B");

            ImGui::SameLine(0, ui::px(4.0f));

            // Solo Button
            bool s1 = br1->isSolo();
            const bool isAwaitingSolo1 = m_keyMapMode && m_awaitingKeyTarget.active &&
                                         m_awaitingKeyTarget.slotIndex == blockIndex &&
                                         m_awaitingKeyTarget.branchIndex == 1 &&
                                         m_awaitingKeyTarget.targetType == static_cast<uint32_t>(midi::BindingTargetType::BranchSolo);
            {
                ScopedStyleColor soloGuard(ImGuiCol_Button, tokens.signal.active.vec4, s1);
                if (CenteredButton("S##br1", ImVec2(ui::px(24.0f), ui::px(24.0f)))) {
                    if (m_keyMapMode) {
                        m_awaitingKeyTarget.active = true;
                        m_awaitingKeyTarget.slotIndex = blockIndex;
                        m_awaitingKeyTarget.blockIndex = blockIndex;
                        m_awaitingKeyTarget.branchIndex = 1;
                        m_awaitingKeyTarget.branchSlotIndex = -1;
                        m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::BranchSolo);
                        m_awaitingKeyTarget.paramId = 0;
                        m_awaitingKeyTarget.targetLabel = "Branch B Solo";
                        triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
                    } else {
                        br1->setSolo(!s1);
                        m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Branch B Solo"),
                                                   "Branch B Solo", state::UndoActionType::ParameterTweak, "Br1:Solo", true);
                        m_undoManager.commitActiveTransaction();
                    }
                } else if (m_keyMapMode && ImGui::IsItemClicked()) {
                    m_awaitingKeyTarget.active = true;
                    m_awaitingKeyTarget.slotIndex = blockIndex;
                    m_awaitingKeyTarget.blockIndex = blockIndex;
                    m_awaitingKeyTarget.branchIndex = 1;
                    m_awaitingKeyTarget.branchSlotIndex = -1;
                    m_awaitingKeyTarget.targetType = static_cast<uint32_t>(midi::BindingTargetType::BranchSolo);
                    m_awaitingKeyTarget.paramId = 0;
                    m_awaitingKeyTarget.targetLabel = "Branch B Solo";
                    triggerHudToast("PRESS KEY FOR: " + m_awaitingKeyTarget.targetLabel);
                }
            }
            if (isAwaitingSolo1) {
                float t = static_cast<float>(ImGui::GetTime());
                float pulse = 0.5f + 0.5f * std::sin(8.0f * t);
                ImU32 highlightCol = tokens.borders.mapModeViolet.withAlpha(0.6f + 0.4f * pulse);
                ImVec2 rMin = ImGui::GetItemRectMin();
                ImVec2 rMax = ImGui::GetItemRectMax();
                dl->AddRect(ImVec2(rMin.x - 2.0f, rMin.y - 2.0f), ImVec2(rMax.x + 2.0f, rMax.y + 2.0f), highlightCol, 4.0f, 0, 2.0f);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(s1 ? "Unsolo Branch B" : "Solo Branch B");

            ImGui::SameLine(0, ui::px(4.0f));

            // Phase Invert Button
            bool p1 = br1->isPhaseInvert();
            {
                ScopedStyleColor phaseGuard(ImGuiCol_Button, tokens.text.accent.vec4, p1);
                if (CenteredButton("\xC3\x98##br1", ImVec2(ui::px(24.0f), ui::px(24.0f)))) {
                    br1->setPhaseInvert(!p1);
                    m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Branch B Phase"),
                                               "Branch B Phase", state::UndoActionType::ParameterTweak, "Br1:Phase", true);
                    m_undoManager.commitActiveTransaction();
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Phase Invert (180° Polarity Flip)");

            ImGui::SameLine(0, ui::px(8.0f));

            // Dynamic slider sizing based on available envelope header width
            const float sliderStartX1 = ImGui::GetCursorPosX();
            const float availForSliders1 = (btnAreaX1 - headerSafetyMargin) - sliderStartX1;
            const float sliderGap = ui::px(6.0f);
            const float maxSliderW = ui::px(96.0f);
            const float sliderW1 = std::clamp((availForSliders1 - sliderGap) * 0.5f, ui::px(48.0f), maxSliderW);

            // Pan Slider (Double-click reset to 0.0)
            float pan1 = br1->pan();
            ImGui::SetNextItemWidth(sliderW1);
            const bool pan1Changed = ResettableSliderFloat("##Pan1", &pan1, -1.0f, 1.0f, 0.0f, "Pan: %+.2f");
            if (pan1Changed) { br1->setPan(pan1); }
            if (ImGui::IsItemDeactivatedAfterEdit() || (pan1Changed && !ImGui::IsItemActive())) {
                m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Branch B Pan"),
                                           "Branch B Pan", state::UndoActionType::ParameterTweak, "Br1:Pan");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_undoManager.commitActiveTransaction();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Branch B Pan (Double-click to center)");

            ImGui::SameLine(0, sliderGap);

            // Gain Slider (Double-click reset to 0.0dB)
            float gain1 = br1->gainDb();
            ImGui::SetNextItemWidth(sliderW1);
            const bool gain1Changed = ResettableSliderFloat("##Gain1", &gain1, -36.0f, +12.0f, 0.0f, "%+.1f dB");
            if (gain1Changed) { br1->setGainDb(gain1); }
            if (ImGui::IsItemDeactivatedAfterEdit() || (gain1Changed && !ImGui::IsItemActive())) {
                m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Branch B Gain"),
                                           "Branch B Gain", state::UndoActionType::ParameterTweak, "Br1:Gain");
                if (ImGui::IsItemDeactivatedAfterEdit()) m_undoManager.commitActiveTransaction();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Branch B Gain (Double-click to reset 0.0 dB)");

            ImGui::PopStyleVar();
            ImGui::EndGroup();

            // Top-right controls: Minimise & Delete Branch B
            ImGui::SetCursorPos(ImVec2(btnAreaX1, headerY1));

            if (renderCenteredMinimiseButton("##minBr1", ImVec2(ui::px(24.0f), ui::px(24.0f)), min1)) {
                m_branchMinimized[key1] = !min1;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(min1 ? "Maximise Branch B" : "Minimise Branch B");

            ImGui::SameLine(0, ui::px(4.0f));
            if (renderCenteredDeleteButton("##delBr1", ImVec2(ui::px(24.0f), ui::px(24.0f)))) {
                pendingDissolveKeepBranch = 0;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete Branch B (Branch A will be kept in rack)");

            // If not minimized, render slots and internal wiring
            if (!min1) {
                float curX1 = envX + envPadX;

                // Wire from envelope entry port to first slot/insert card
                drawRoutingWire(dl, toScreen(envX, card1MidY), toScreen(curX1, card1MidY), false);

                for (size_t s = 0; s < numSlots1; ++s) {
                    auto* bSlot = br1->getSlot(s);
                    if (!bSlot) continue;

                    ImGui::SetCursorPos(ImVec2(curX1, card1Top));
                    renderPluginSlot(bSlot, static_cast<int>(s), blockIndex, 1);
                    curX1 += cardW;

                    drawRoutingWire(dl, toScreen(curX1, card1MidY), toScreen(curX1 + slotWireW, card1MidY), false);
                    curX1 += slotWireW;
                }

                // Branch B Inline Insert Button Card
                ImGui::SetCursorPos(ImVec2(curX1, card1Top));
                ImGui::BeginGroup();
                ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.cardBg.vec4);
                char insertBr1Card[32];
                std::snprintf(insertBr1Card, sizeof(insertBr1Card), "InsertBr1Card_%d", blockIndex);
                ImGui::BeginChild(insertBr1Card, ImVec2(insertCardW, cardH), true, ImGuiWindowFlags_NoScrollbar);
                ImGui::SetCursorPos(ImVec2((insertCardW - ui::px(48.0f)) * 0.5f, (cardH - ui::px(48.0f)) * 0.5f));
                if (renderCenteredPlusButton("##AddPluginBr1", ImVec2(ui::px(48.0f), ui::px(48.0f)), ui::px(11.0f))) {
                    m_pluginBrowserModal->open(blockIndex, 1);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Insert plugin into Branch B");
                }
                ImGui::EndChild();
                ImGui::PopStyleColor();

                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("RACK_CARD_PAYLOAD")) {
                        const auto* pl = static_cast<const CardDragPayload*>(p->Data);
                        if (pl && pl->sourceIndex >= 0) {
                            moveSlot(pl->sourceIndex, pl->sourceBlockIndex, pl->sourceBranchIndex,
                                     -1, blockIndex, 1);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::EndGroup();
                curX1 += insertCardW;

                // Wire from insert card to envelope exit port
                if (curX1 < envX + envelopeW) {
                    drawRoutingWire(dl, toScreen(curX1, card1MidY), toScreen(envX + envelopeW, card1MidY), false);
                }
            }
        }
    }

    // =========================================================================
    // 4. CONVERGENCE & COMBINER NODE
    // =========================================================================
    // Right Exit Ports on Envelopes
    dl->AddCircleFilled(toScreen(envX + envelopeW, port0Y), 6.0f, tokens.cables.socketRing);
    dl->AddCircleFilled(toScreen(envX + envelopeW, port0Y), 3.5f, tokens.signal.branchA);

    dl->AddCircleFilled(toScreen(envX + envelopeW, port1Y), 6.0f, tokens.cables.socketRing);
    dl->AddCircleFilled(toScreen(envX + envelopeW, port1Y), 3.5f, tokens.signal.branchB);

    const float convLeadW = ui::px(36.0f);
    const float combW = ui::px(104.0f);
    const float combH = ui::px(60.0f);
    const float combLeft = envX + envelopeW + convLeadW;
    const float combTop = centerY - (combH * 0.5f);

    drawRoutingWire(dl, toScreen(envX + envelopeW, port0Y), toScreen(combLeft, centerY), false);
    drawRoutingWire(dl, toScreen(envX + envelopeW, port1Y), toScreen(combLeft, centerY), false);

    // Combiner card background
    dl->AddRectFilled(toScreen(combLeft, combTop), toScreen(combLeft + combW, combTop + combH), tokens.surfaces.combinerBg, 6.0f);
    dl->AddRect(toScreen(combLeft, combTop), toScreen(combLeft + combW, combTop + combH), tokens.borders.combinerBorder, 6.0f, 0, 1.2f);

    // Combiner Ports
    dl->AddCircleFilled(toScreen(combLeft, centerY), 4.0f, tokens.cables.core);
    dl->AddCircleFilled(toScreen(combLeft + combW, centerY), 4.0f, tokens.cables.core);

    ImGui::SetCursorPos(ImVec2(combLeft + ui::px(6.0f), combTop + ui::px(6.0f)));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ui::px(4.0f), ui::px(2.0f)));

    ImGui::SetCursorPosX(combLeft + (combW - ImGui::CalcTextSize("SUM / BLEND").x) * 0.5f);
    ImGui::TextColored(tokens.text.accent.vec4, "SUM / BLEND");

    float blend = block->blend();
    ImGui::SetCursorPosX(combLeft + ui::px(8.0f));
    ImGui::SetNextItemWidth(combW - ui::px(16.0f));
    const bool blendChanged = ResettableSliderFloat("##ABBlend", &blend, -1.0f, 1.0f, 0.0f, "A:B %+.2f");
    if (blendChanged) {
        block->setBlend(blend);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() || (blendChanged && !ImGui::IsItemActive())) {
        m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Parallel Blend"),
                                   "Parallel Blend", state::UndoActionType::ParameterTweak, "Parallel:Blend");
        if (ImGui::IsItemDeactivatedAfterEdit()) m_undoManager.commitActiveTransaction();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Branch A / B Crossfade Blend\n0.00: 50/50 Equal Sum\n-1.00: 100%% Branch A\n+1.00: 100%% Branch B\nDouble-click to center");
    }

    ImGui::PopStyleVar();
    ImGui::EndGroup();

    // Set cursor to the right edge of combiner node for downstream serial routing
    ImGui::SetCursorPosX(combLeft + combW);

    // Handle deferred branch dissolve if triggered
    if (pendingDissolveKeepBranch >= 0) {
        int branchToDelete = (pendingDissolveKeepBranch == 1) ? 0 : 1;
        auto* delBr = block->getBranch(branchToDelete);
        if (delBr) {
            for (size_t s = 0; s < delBr->numSlots(); ++s) {
                auto* slot = delBr->getSlot(s);
                if (slot) {
                    if (m_dspTweakSlot == slot) m_dspTweakSlot = nullptr;
                    if (m_expandedSlot == slot) m_expandedSlot = nullptr;
                    if (m_pendingGuiEditSlot == slot) m_pendingGuiEditSlot = nullptr;
                    if (m_focusedSlot == slot) {
                        m_focusedSlot = nullptr;
                        m_focusedSlotIndex = -1;
                    }
                    m_guiParamBaseline.erase(slot);
                    m_slotMacros.erase(slot);
                    auto* pInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
                    if (pInst) plugins::PluginWindowManager::instance().closePluginWindow(pInst);
                }
            }
        }
        m_graph.dissolveParallelBlock(blockIndex, pendingDissolveKeepBranch);
        syncMacrosFromGraph();
        m_undoManager.pushSnapshot(
            state::SceneManager::captureGraphSnapshot(m_graph, "Delete Branch"),
            "Delete Branch",
            state::UndoActionType::StructuralMutation
        );
    }

    ImGui::PopID();
}

void RackView::renderBottomBar() {
    ImGui::Separator();
    const auto& tokens = themeTokens();

    // Left side: Settings button (clean name without brackets)
    {
        ScopedStyleColor settingsGuard({
            {ImGuiCol_Button, tokens.surfaces.buttonBg.vec4},
            {ImGuiCol_ButtonHovered, tokens.surfaces.buttonBgHovered.vec4},
            {ImGuiCol_Text, tokens.text.primary.vec4}
        });
        if (CenteredButton("Settings", ImVec2(80.0f, Metrics::buttonHeight()))) {
            m_settingsModal->open();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open Audio, Plugin, and Host Settings");
    }

    ImGui::SameLine(0, 8);
    // Practice Tools (Looper & Backing Track Player) toggle button
    bool toolsActive = m_practiceToolsModal && m_practiceToolsModal->isOpen();
    {
        ScopedStyleColor toolsGuard(ImGuiCol_Button, tokens.surfaces.buttonBgActive.vec4, toolsActive);
        if (CenteredButton("Practice Tools", ImVec2(105.0f, Metrics::buttonHeight()))) {
            m_practiceToolsModal->toggle();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Open Practice Tools (Riff Looper & WAV Backing Track Player)");
    }

    ImGui::SameLine(0, 8);
    {
        ScopedStyleColor saveRiffGuard({
            {ImGuiCol_Button, tokens.surfaces.buttonBg.vec4},
            {ImGuiCol_ButtonHovered, tokens.surfaces.buttonBgHovered.vec4},
            {ImGuiCol_Text, tokens.text.accent.vec4}
        });
        if (CenteredButton("★ Save Last Riff", ImVec2(125.0f, Metrics::buttonHeight()))) {
            saveLastRiff();
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", "Export circular audio buffer to %USERPROFILE%\\Documents\\Praccy\\Riffs\\ (Ctrl+Shift+R)");
    }

    ImGui::SameLine(0, 8);
    {
        ScopedStyleColor mapColorGuard({
            {ImGuiCol_Button, tokens.surfaces.buttonBgActive.vec4},
            {ImGuiCol_Border, tokens.borders.mapModeViolet.vec4}
        }, m_midiMapMode);

        if (CenteredButton("MIDI Map", ImVec2(75.0f, Metrics::buttonHeight()))) {
            m_midiMapMode = !m_midiMapMode;
            if (m_midiMapMode) {
                m_keyMapMode = false;
                m_awaitingKeyTarget.active = false;
            }
            triggerHudToast(m_midiMapMode ? "MIDI MAP MODE: ACTIVE" : "MIDI MAP MODE: OFF");
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Toggle MIDI Map Mode (Ctrl+M)");
    }

    ImGui::SameLine(0, 6);
    {
        ScopedStyleColor keyColorGuard({
            {ImGuiCol_Button, tokens.surfaces.buttonBgActive.vec4},
            {ImGuiCol_Border, tokens.borders.mapModeViolet.vec4}
        }, m_keyMapMode);

        if (CenteredButton("Key Map", ImVec2(75.0f, Metrics::buttonHeight()))) {
            m_keyMapMode = !m_keyMapMode;
            if (m_keyMapMode) {
                m_midiMapMode = false;
            } else {
                m_awaitingKeyTarget.active = false;
            }
            triggerHudToast(m_keyMapMode ? "KEY MAP MODE: ACTIVE" : "KEY MAP MODE: OFF");
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Toggle Key Map Mode (Ctrl+K)");
    }

    ImGui::SameLine(0, 14);
    ImGui::AlignTextToFramePadding();

    // Left side: MIDI status / learn
    if (m_midi.isLearning()) {
        ImGui::TextColored(tokens.text.warning.vec4, "[*] MIDI LEARN ACTIVE: Move any knob, slider, or press a footswitch to bind...");
        ImGui::SameLine(0, 12);
        if (CenteredButton("Cancel")) {
            m_midi.cancelLearning();
        }
    } else {
        ImGui::TextDisabled("MIDI: %s", m_midi.lastActivityDescription().c_str());
    }

    // Right side: ASIO Activity & Real-Time DSP Status Pill
    const double sr = m_asio.currentSampleRate();
    const int bufSize = m_asio.currentBufferSize();
    const double latencyMs = (sr > 0.0) ? (static_cast<double>(bufSize) / sr * 1000.0) : 0.0;
    float dspLoad = m_dspLoadPercent ? m_dspLoadPercent->load(std::memory_order_relaxed) : 0.0f;
    uint32_t drops = m_dspDropouts ? m_dspDropouts->load(std::memory_order_relaxed) : 0;

    char statusText[160];
    if (m_asio.isRunning()) {
        const char* srFormat = (std::fmod(sr, 1000.0) == 0.0) ? "%.0f kHz" : "%.1f kHz";
        char srBuf[32];
        std::snprintf(srBuf, sizeof(srBuf), srFormat, sr / 1000.0);

        if (drops > 0) {
            std::snprintf(statusText, sizeof(statusText), "ACTIVE  |  %s  |  %d spls (%.1f ms)  |  DSP: %.0f%%  |  DROPS: %u",
                          srBuf, bufSize, latencyMs, dspLoad, drops);
        } else {
            std::snprintf(statusText, sizeof(statusText), "ACTIVE  |  %s  |  %d spls (%.1f ms)  |  DSP: %.0f%%",
                          srBuf, bufSize, latencyMs, dspLoad);
        }
    } else {
        std::snprintf(statusText, sizeof(statusText), "STOPPED");
    }

    const float pillW = ImGui::CalcTextSize(statusText).x + 36.0f;
    const float hotkeyBtnW = 28.0f;
    const float spacingBetween = 8.0f;
    const float availW = ImGui::GetContentRegionAvail().x;
    const float rightMargin = 14.0f;
    const float totalRightW = pillW + hotkeyBtnW + spacingBetween + rightMargin;

    if (availW > totalRightW) {
        ImGui::SameLine(ImGui::GetCursorPosX() + availW - totalRightW);
    } else {
        ImGui::SameLine(0, 8);
    }

    // Hotkey button with vector keyboard glyph immediately to the left of DSP pill
    const bool isLegendVisible = m_showHotkeyLegend || ImGui::GetIO().KeyAlt;
    {
        ScopedStyleColor hkGuard({
            {ImGuiCol_Button, isLegendVisible ? tokens.text.accent.vec4 : tokens.surfaces.buttonBg.vec4},
            {ImGuiCol_Border, isLegendVisible ? tokens.borders.focus.vec4 : tokens.borders.subtle.vec4}
        });
        if (CenteredButton("##HotkeyLegendToggleBtn", ImVec2(hotkeyBtnW, Metrics::kButtonHeight))) {
            m_showHotkeyLegend = !m_showHotkeyLegend;
        }

        // Draw crisp vector keyboard glyph inside button
        ImVec2 btnMin = ImGui::GetItemRectMin();
        ImVec2 btnMax = ImGui::GetItemRectMax();
        ImVec2 center((btnMin.x + btnMax.x) * 0.5f, (btnMin.y + btnMax.y) * 0.5f);
        ImU32 glyphColor = isLegendVisible ? tokens.text.inverse.u32 : tokens.text.primary.u32;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Keyboard frame
        dl->AddRect(ImVec2(center.x - 7.5f, center.y - 5.0f), ImVec2(center.x + 7.5f, center.y + 5.0f), glyphColor, 1.5f, 0, 1.2f);
        // Keys row 1
        dl->AddLine(ImVec2(center.x - 5.0f, center.y - 2.5f), ImVec2(center.x - 3.0f, center.y - 2.5f), glyphColor, 1.0f);
        dl->AddLine(ImVec2(center.x - 1.0f, center.y - 2.5f), ImVec2(center.x + 1.0f, center.y - 2.5f), glyphColor, 1.0f);
        dl->AddLine(ImVec2(center.x + 3.0f, center.y - 2.5f), ImVec2(center.x + 5.0f, center.y - 2.5f), glyphColor, 1.0f);
        // Spacebar
        dl->AddLine(ImVec2(center.x - 4.0f, center.y + 2.0f), ImVec2(center.x + 4.0f, center.y + 2.0f), glyphColor, 1.2f);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Keyboard Shortcuts & Hotkey Legend (Hold Alt or Click to Toggle)");
    }

    ImGui::SameLine(0, spacingBetween);
    renderStatusPill(m_asio.isRunning(), statusText);
}

void RackView::renderMeter(const char* label, float level, float width, float height) {
    renderMeter(label, level, level, false, width, height, nullptr);
}

void RackView::renderMeter(const char* id, float level, float holdLevel, bool isClipped, float width, float height, bool* outClicked) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const auto& tokens = themeTokens();

    // 1. Recessed track background
    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), tokens.surfaces.frameBg, 2.0f);

    const float db = audio::DspUtils::gainToDb(level);
    const float norm = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);

    const float innerX1 = pos.x + 1.0f;
    const float innerX2 = pos.x + width - 1.0f;
    const float innerYBottom = pos.y + height - 1.0f;
    const float innerYTop = pos.y + 1.0f;
    const float innerH = std::max(0.0f, innerYBottom - innerYTop);

    // 2. Smooth continuous gradient meter bar (seamless, no blocks)
    if (norm > 0.001f && innerH > 0.0f && innerX2 > innerX1) {
        const float fillH = norm * innerH;
        const float yFillTop = innerYBottom - fillH;

        constexpr float kNormOptimal = 0.70f;   // -18 dBFS
        constexpr float kNormWarning = 0.90f;   // -6 dBFS
        constexpr float kNormClip    = 0.9917f; // -0.5 dBFS

        const auto& cNormal  = tokens.signal.meterNormal;
        const auto& cOptimal = tokens.signal.meterOptimal;
        const auto& cWarning = tokens.signal.meterWarning;
        const auto& cClip    = tokens.signal.meterClip;

        auto lerpColor = [](const ColorToken& c1, const ColorToken& c2, float t) noexcept -> ImU32 {
            t = std::clamp(t, 0.0f, 1.0f);
            const float r = c1.r + (c2.r - c1.r) * t;
            const float g = c1.g + (c2.g - c1.g) * t;
            const float b = c1.b + (c2.b - c1.b) * t;
            const float a = c1.a + (c2.a - c1.a) * t;
            const auto r8 = static_cast<uint32_t>(std::clamp(r * 255.0f + 0.5f, 0.0f, 255.0f));
            const auto g8 = static_cast<uint32_t>(std::clamp(g * 255.0f + 0.5f, 0.0f, 255.0f));
            const auto b8 = static_cast<uint32_t>(std::clamp(b * 255.0f + 0.5f, 0.0f, 255.0f));
            const auto a8 = static_cast<uint32_t>(std::clamp(a * 255.0f + 0.5f, 0.0f, 255.0f));
            return (a8 << 24) | (b8 << 16) | (g8 << 8) | r8;
        };

        const float yOpt = innerYBottom - kNormOptimal * innerH;
        if (norm <= kNormOptimal) {
            const float t = norm / kNormOptimal;
            const ImU32 topCol = lerpColor(cNormal, cOptimal, t);
            drawList->AddRectFilledMultiColor(
                ImVec2(innerX1, yFillTop), ImVec2(innerX2, innerYBottom),
                topCol, topCol, cNormal.u32, cNormal.u32);
        } else {
            drawList->AddRectFilledMultiColor(
                ImVec2(innerX1, yOpt), ImVec2(innerX2, innerYBottom),
                cOptimal.u32, cOptimal.u32, cNormal.u32, cNormal.u32);

            const float yWarn = innerYBottom - kNormWarning * innerH;
            if (norm <= kNormWarning) {
                const float t = (norm - kNormOptimal) / (kNormWarning - kNormOptimal);
                const ImU32 topCol = lerpColor(cOptimal, cWarning, t);
                drawList->AddRectFilledMultiColor(
                    ImVec2(innerX1, yFillTop), ImVec2(innerX2, yOpt),
                    topCol, topCol, cOptimal.u32, cOptimal.u32);
            } else {
                drawList->AddRectFilledMultiColor(
                    ImVec2(innerX1, yWarn), ImVec2(innerX2, yOpt),
                    cWarning.u32, cWarning.u32, cOptimal.u32, cOptimal.u32);

                const float yClip = innerYBottom - kNormClip * innerH;
                if (norm <= kNormClip) {
                    const float t = (norm - kNormWarning) / (kNormClip - kNormWarning);
                    const ImU32 topCol = lerpColor(cWarning, cClip, t);
                    drawList->AddRectFilledMultiColor(
                        ImVec2(innerX1, yFillTop), ImVec2(innerX2, yWarn),
                        topCol, topCol, cWarning.u32, cWarning.u32);
                } else {
                    drawList->AddRectFilledMultiColor(
                        ImVec2(innerX1, yClip), ImVec2(innerX2, yWarn),
                        cClip.u32, cClip.u32, cWarning.u32, cWarning.u32);

                    drawList->AddRectFilled(
                        ImVec2(innerX1, yFillTop), ImVec2(innerX2, yClip),
                        cClip.u32);
                }
            }
        }
    }

    // 3. Peak-Hold Indicator: 2px line at hold level
    const float holdDb = audio::DspUtils::gainToDb(holdLevel);
    const float holdNorm = std::clamp((holdDb + 60.0f) / 60.0f, 0.0f, 1.0f);
    if (holdNorm > 0.001f && innerH > 0.0f && innerX2 > innerX1) {
        const float yHold = std::clamp(std::floor(innerYBottom - holdNorm * innerH), innerYTop, innerYBottom);
        const ImU32 tickCol = (holdNorm >= 0.9917f) ? tokens.signal.meterClip.u32 :
                              ((holdNorm >= 0.90f) ? tokens.signal.meterWarning.u32 : tokens.signal.meterOptimal.u32);
        drawList->AddLine(ImVec2(innerX1, yHold), ImVec2(innerX2, yHold), tickCol, 2.0f);
    }

    // 4. Calibrated dBFS reference ticks (-6, -12, -18, -36 dB) on tall meters
    if (height >= 48.0f && innerH > 0.0f) {
        constexpr float kTickDbs[] = { -6.0f, -12.0f, -18.0f, -36.0f };
        for (float tDb : kTickDbs) {
            const float tNorm = (tDb + 60.0f) / 60.0f;
            const float tickY = std::floor(innerYBottom - tNorm * innerH);
            drawList->AddLine(ImVec2(pos.x + 1.0f, tickY), ImVec2(pos.x + 3.5f, tickY), tokens.borders.strong.u32, 1.0f);
        }
    }

    // 5. Sticky clip indicator (red top cap)
    const bool showClip = isClipped || (db >= -0.2f);
    if (showClip && innerH > 0.0f && innerX2 > innerX1) {
        drawList->AddRectFilled(ImVec2(innerX1, innerYTop), ImVec2(innerX2, std::min(innerYBottom, innerYTop + 4.0f)), tokens.signal.meterClip.u32);
    }

    // 6. Subtle framing border
    drawList->AddRect(pos, ImVec2(pos.x + width, pos.y + height), tokens.borders.subtle, 2.0f);

    // 7. Interactive click target to reset clip indicator
    ImGui::SetCursorScreenPos(pos);
    char btnId[64];
    std::snprintf(btnId, sizeof(btnId), "##MtrBtn_%s", (id && id[0] != '\0') ? id : "unnamed");
    if (ImGui::InvisibleButton(btnId, ImVec2(width, height))) {
        if (outClicked) {
            *outClicked = true;
        }
    }
    if (ImGui::IsItemHovered() && showClip) {
        ImGui::SetTooltip("Clip indicator (Click to clear)");
    }
}









void RackView::renderDspTweakModal() {
    if (!m_dspTweakSlot) return;

    auto* pInst = dynamic_cast<plugins::IPluginInstance*>(m_dspTweakSlot->innerNode());
    if (!pInst) {
        m_dspTweakSlot = nullptr;
        return;
    }

    const size_t numParams = pInst->numParameters();
    float winHeight = std::max(220.0f, 130.0f + static_cast<float>(numParams) * 36.0f);
    ImGui::SetNextWindowSize(ImVec2(390, winHeight), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    char titleBuf[64];
    std::snprintf(titleBuf, sizeof(titleBuf), "%s Parameters##DspTweak", m_dspTweakSlot->name().c_str());

    const auto& tokens = themeTokens();
    if (ImGui::Begin(titleBuf, &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(tokens.text.accent.vec4, "%s", m_dspTweakSlot->name().c_str());
        ImGui::TextDisabled("Vendor: %s | Version: %s", pInst->vendor().c_str(), pInst->version().c_str());
        ImGui::Separator();
        ImGui::Spacing();

        if (numParams == 0) {
            ImGui::TextDisabled("No modifiable parameters available.");
        } else {
            float availW = ImGui::GetContentRegionAvail().x;
            for (size_t i = 0; i < numParams; ++i) {
                auto desc = pInst->getParameterDesc(i);
                float val = pInst->getParameterValue(desc.id);

                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", desc.name.c_str());
                ImGui::SameLine(95.0f);
                ImGui::SetNextItemWidth(availW - 95.0f);

                char label[64];
                std::snprintf(label, sizeof(label), "##p_%zu", i);
                const bool paramChanged = ResettableSliderFloat(label, &val, desc.minValue, desc.maxValue, desc.defaultValue, "%.2f");
                const bool paramDeactivatedAfterEdit = ImGui::IsItemDeactivatedAfterEdit();
                const bool paramDiscreteChange = paramChanged && !ImGui::IsItemActive();

                if (paramChanged) {
                    if (m_dspTweakSlot) {
                        m_dspTweakSlot->setParameter(desc.id, val);
                    }
                    if (pInst) {
                        pInst->setParameterValue(desc.id, val);
                        pInst->clearGuiParamEdit();
                    }
                    if (m_dspTweakSlot) {
                        for (int m = 0; m < 4; ++m) {
                            auto& mac = m_dspTweakSlot->macros()[m];
                            if (mac.bound && mac.paramId == desc.id) {
                                mac.currentValue = val;
                                m_slotMacros[m_dspTweakSlot][m].currentValue = val;
                            }
                        }
                        refreshGuiParamBaseline(m_dspTweakSlot);
                    }
                }
                if ((paramDeactivatedAfterEdit || paramDiscreteChange) && m_dspTweakSlot) {
                    refreshGuiParamBaseline(m_dspTweakSlot);
                    m_undoManager.pushSnapshot(
                        state::SceneManager::captureGraphSnapshot(m_graph, m_dspTweakSlot->name() + ": " + desc.name),
                        m_dspTweakSlot->name() + ": " + desc.name,
                        state::UndoActionType::ParameterTweak,
                        m_dspTweakSlot->name() + ":dsp:" + desc.name
                    );
                    if (paramDeactivatedAfterEdit) {
                        m_undoManager.commitActiveTransaction();
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Double-click to reset (default: %.2f)", desc.defaultValue);
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (CenteredButton("Close", ImVec2(80, 24))) {
            open = false;
        }
    }
    ImGui::End();

    if (!open) {
        m_dspTweakSlot = nullptr;
    }
}

void RackView::renderUpdateModal() {
    if (!m_showUpdateModal) return;
    const auto& tokens = themeTokens();

    ImGui::OpenPopup("Praccy Updates##Modal");
    ImGui::SetNextWindowSize(ImVec2(520, 390), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::BeginPopupModal("Praccy Updates##Modal", &m_showUpdateModal, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::Text("Installed Version: %s", praccy::kVersionString);
        ImGui::Spacing();

        // Option to enable beta builds from dev branch
        bool includeBeta = m_cachedConfig.checkBetaUpdates;
        if (ImGui::Checkbox("Enable beta builds from 'dev' branch on GitHub", &includeBeta)) {
            m_cachedConfig.checkBetaUpdates = includeBeta;
            m_cachedConfig.save();
            UpdateChecker::instance().checkForUpdates(includeBeta);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Check for cutting-edge development commits directly from origin/dev on GitHub");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        auto info = UpdateChecker::instance().getInfo();

        if (info.status == UpdateStatus::Checking) {
            ImGui::TextColored(tokens.text.accent.vec4, "Checking GitHub repository for %s updates...", includeBeta ? "beta" : "stable");
            ImGui::Spacing();
            ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 8));
        } else if (info.status == UpdateStatus::UpToDate) {
            ImGui::TextColored(tokens.text.success.vec4, "[v] You are up to date!");
            ImGui::TextDisabled("Channel: %s", includeBeta ? "Beta (dev branch)" : "Stable (Releases)");
            ImGui::Text("Latest version: %s", info.latestVersion.c_str());
        } else if (info.status == UpdateStatus::UpdateAvailable) {
            ImGui::TextColored(tokens.text.warning.vec4, "[*] New %s build available!", includeBeta ? "Beta" : "Release");
            ImGui::Text("Latest: %s", info.latestVersion.c_str());
            if (!info.releaseTitle.empty()) {
                ImGui::TextColored(tokens.text.primary.vec4, "%s", info.releaseTitle.c_str());
            }
            if (!info.publishedDate.empty()) {
                ImGui::TextDisabled("Date: %s", info.publishedDate.c_str());
            }

            ImGui::Spacing();
            if (!info.assetUrl.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Button, tokens.signal.active.withAlpha(0.45f).vec4);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tokens.signal.active.withAlpha(0.65f).vec4);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, tokens.signal.active.withAlpha(0.85f).vec4);
                if (CenteredButton("Download & Apply Update", ImVec2(210, 30))) {
                    UpdateChecker::instance().startDownload();
                }
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0, 10);
            }
            if (CenteredButton("Open in GitHub ->", ImVec2(150, 30))) {
                ShellExecuteA(nullptr, "open", info.downloadUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            if (info.assetUrl.empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("Note: Direct binary asset not found for this commit; click 'Open in GitHub' to view.");
            }
        } else if (info.status == UpdateStatus::Downloading) {
            ImGui::TextColored(tokens.text.accent.vec4, "Downloading update package...");
            if (!info.assetName.empty()) {
                ImGui::TextDisabled("File: %s", info.assetName.c_str());
            }

            char progBuf[64];
            if (info.totalBytes > 0) {
                float dlMB = static_cast<float>(info.downloadedBytes) / (1024.0f * 1024.0f);
                float totMB = static_cast<float>(info.totalBytes) / (1024.0f * 1024.0f);
                std::snprintf(progBuf, sizeof(progBuf), "%.1f / %.1f MB (%.0f%%)", dlMB, totMB, info.downloadProgress * 100.0f);
            } else if (info.downloadedBytes > 0) {
                float dlMB = static_cast<float>(info.downloadedBytes) / (1024.0f * 1024.0f);
                std::snprintf(progBuf, sizeof(progBuf), "%.1f MB downloaded", dlMB);
            } else {
                std::snprintf(progBuf, sizeof(progBuf), "Connecting...");
            }

            ImGui::Spacing();
            ImGui::ProgressBar(info.downloadProgress, ImVec2(-1, 22), progBuf);
            ImGui::Spacing();

            if (info.downloadProgress >= 1.0f) {
                ImGui::TextColored(tokens.text.warning.vec4, "Extracting and verifying package files...");
            } else {
                ImGui::TextDisabled("Please wait while Praccy streams the update package...");
            }
        } else if (info.status == UpdateStatus::ReadyToInstall) {
            ImGui::TextColored(tokens.text.success.vec4, "[v] Update downloaded and ready to apply!");
            ImGui::TextWrapped("Praccy will now close, copy the updated files into place, and restart automatically.");
            ImGui::Spacing();

            ImGui::PushStyleColor(ImGuiCol_Button, tokens.signal.active.withAlpha(0.45f).vec4);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, tokens.signal.active.withAlpha(0.65f).vec4);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, tokens.signal.active.withAlpha(0.85f).vec4);
            if (CenteredButton("Restart & Apply Update Now", ImVec2(230, 32))) {
                UpdateChecker::instance().applyUpdateAndRestart();
            }
            ImGui::PopStyleColor(3);
        } else if (info.status == UpdateStatus::Error) {
            ImGui::TextColored(tokens.text.error.vec4, "[!] Update check / download failed");
            ImGui::TextWrapped("%s", info.errorMessage.c_str());
            if (!info.downloadUrl.empty()) {
                ImGui::Spacing();
                if (CenteredButton("Open GitHub in Browser ->", ImVec2(200, 28))) {
                    ShellExecuteA(nullptr, "open", info.downloadUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
        }

        float bottomY = std::max(ImGui::GetCursorPosY() + 12.0f, 335.0f);
        ImGui::SetCursorPosY(bottomY);
        ImGui::Separator();

        bool isDownloading = (info.status == UpdateStatus::Downloading);
        if (!isDownloading) {
            if (CenteredButton("Check Again", ImVec2(110, 26))) {
                UpdateChecker::instance().checkForUpdates(includeBeta);
            }
            ImGui::SameLine();
        }
        if (CenteredButton("Close", ImVec2(90, 26))) {
            m_showUpdateModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

static std::string detectPresetSoundProfile(const std::string& name) {
    std::string lower;
    lower.resize(name.size());
    std::transform(name.begin(), name.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower.find("metal") != std::string::npos || lower.find("djent") != std::string::npos ||
        lower.find("lead") != std::string::npos || lower.find("high gain") != std::string::npos ||
        lower.find("heavy") != std::string::npos || lower.find("shred") != std::string::npos ||
        lower.find("5150") != std::string::npos) {
        return "High Gain Metal";
    }
    if (lower.find("clean") != std::string::npos || lower.find("glassy") != std::string::npos ||
        lower.find("chime") != std::string::npos || lower.find("chorus") != std::string::npos ||
        lower.find("acoustic") != std::string::npos || lower.find("jazz") != std::string::npos) {
        return "Glassy Clean";
    }
    if (lower.find("ambient") != std::string::npos || lower.find("shimmer") != std::string::npos ||
        lower.find("space") != std::string::npos || lower.find("delay") != std::string::npos ||
        lower.find("wash") != std::string::npos || lower.find("reverb") != std::string::npos ||
        lower.find("ethereal") != std::string::npos) {
        return "Ambient FX";
    }
    if (lower.find("breakup") != std::string::npos || lower.find("crunch") != std::string::npos ||
        lower.find("blues") != std::string::npos || lower.find("drive") != std::string::npos ||
        lower.find("boost") != std::string::npos || lower.find("overdrive") != std::string::npos ||
        lower.find("plexi") != std::string::npos) {
        return "Edge of Breakup";
    }
    return "Edge of Breakup";
}

void RackView::renderPresetBrowserModal() {
    if (!m_showPresetBrowserModal) return;
    const auto& tokens = themeTokens();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float initW = vp ? std::clamp(vp->Size.x * 0.70f, 680.0f, 1050.0f) : 740.0f;
    const float initH = vp ? std::clamp(vp->Size.y * 0.72f, 480.0f, 800.0f) : 520.0f;

    ImGui::OpenPopup("Preset Chain Browser##Modal");
    ImGui::SetNextWindowSize(ImVec2(initW, initH), ImGuiCond_Appearing);
    if (vp) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(600, 400), ImVec2(vp->Size.x * 0.95f, vp->Size.y * 0.95f));
    }
    ImGui::SetNextWindowPos(vp ? vp->GetCenter() : ImVec2(100.0f, 100.0f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, tokens.surfaces.popupBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.subtle.vec4);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Spec::kModalCornerRadius);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Spec::kModalWindowPadding, Spec::kModalWindowPadding));

    if (ImGui::BeginPopupModal("Preset Chain Browser##Modal", &m_showPresetBrowserModal, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        // Search-as-you-type fuzzy filter
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(tokens.text.muted.vec4, "SEARCH:");
        ImGui::SameLine();
        const float searchW = std::max(180.0f, ImGui::GetContentRegionAvail().x - 100.0f);
        ImGui::SetNextItemWidth(searchW);
        ImGui::InputTextWithHint("##PresetSearchInput", "Search presets by name or tone profile...", m_presetSearchFilter, sizeof(m_presetSearchFilter));

        ImGui::SameLine();
        if (CenteredButton("Clear##ClearFilter", ImVec2(70, Metrics::buttonHeight()))) {
            m_presetSearchFilter[0] = '\0';
        }

        ImGui::Spacing();

        // Dynamic Sound Profile Filter Tags (ALL, then FAVORITES *, then categories; scrollable if needed)
        std::vector<std::string> catPills = { "ALL", "FAVORITES *" };
        auto allCats = m_scenes.allPresetCategories();
        for (const auto& c : allCats) {
            std::string upperC = c;
            std::transform(upperC.begin(), upperC.end(), upperC.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
            if (std::find(catPills.begin(), catPills.end(), upperC) == catPills.end()) {
                catPills.push_back(upperC);
            }
        }

        const float pillsChildH = Metrics::buttonHeight() + 6.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ColorToken::transparent().vec4);
        ImGui::BeginChild("##CategoryPillsScroll", ImVec2(0.0f, pillsChildH), false, ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Metrics::buttonHeight() * 0.5f);
        for (size_t c = 0; c < catPills.size(); ++c) {
            bool isSelected = (m_selectedCategoryTagIndex == static_cast<int>(c));
            ScopedStyleColor btnGuard(ImGuiCol_Button, isSelected ? tokens.surfaces.buttonBgActive.vec4 : tokens.surfaces.buttonBg.vec4);
            ScopedStyleColor borderGuard(ImGuiCol_Border, isSelected ? tokens.borders.focus.vec4 : tokens.borders.subtle.vec4);

            if (CenteredButton(catPills[c].c_str(), ImVec2(0.0f, Metrics::buttonHeight()))) {
                m_selectedCategoryTagIndex = static_cast<int>(c);
            }

            if (c + 1 < catPills.size()) {
                ImGui::SameLine(0, 6);
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Preset Table inside scrollable child zone (3 columns: Fav, Name, Category)
        const float footerHeight = Metrics::kFooterHeight;
        const float tableHeight = ImGui::GetContentRegionAvail().y - footerHeight;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.surfaces.cardBg.vec4);
        ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.subtle.vec4);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Spec::kCornerRadius);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));

        if (ImGui::BeginChild("PresetListChild", ImVec2(0, tableHeight), true)) {
            if (!m_userPresetsValid) {
                m_cachedUserPresetNames = m_scenes.savedPresetNames();
                m_userPresetsValid = true;
            }
            const auto& presetNames = m_cachedUserPresetNames;

            // Prepare lowercase filter string
            std::string filterLower = m_presetSearchFilter;
            std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

            int displayedCount = 0;

            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, Spec::tableCellPadding());
            if (ImGui::BeginTable("PresetTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Fav", ImGuiTableColumnFlags_WidthFixed, 42.0f);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch, 0.42f);
                ImGui::TableHeadersRow();

                for (const auto& pName : presetNames) {
                    std::string category = m_scenes.presetCategory(pName);
                    if (category.empty()) {
                        category = detectPresetSoundProfile(pName);
                    }
                    bool isFav = isPresetFavorite(pName);

                    // Category Filter Check
                    if (m_selectedCategoryTagIndex == 1) {
                        // Option 1 is Favorites
                        if (!isFav) continue;
                    } else if (m_selectedCategoryTagIndex >= 2 && m_selectedCategoryTagIndex < static_cast<int>(catPills.size())) {
                        std::string targetCat = catPills[m_selectedCategoryTagIndex];
                        std::string catUpper = category;
                        std::transform(catUpper.begin(), catUpper.end(), catUpper.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                        if (catUpper != targetCat) continue;
                    }

                    // Text Search Check
                    if (!filterLower.empty()) {
                        std::string nameLower = pName;
                        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        std::string catLower = category;
                        std::transform(catLower.begin(), catLower.end(), catLower.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                        if (nameLower.find(filterLower) == std::string::npos &&
                            catLower.find(filterLower) == std::string::npos) {
                            continue;
                        }
                    }

                    displayedCount++;
                    const float rowH = Spec::tableRowHeight();
                    ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);

                    bool isAuditioned = (m_auditionedPreset == pName);
                    if (isAuditioned) {
                        ImU32 highlightCol = tokens.surfaces.cardBgSelected.u32;
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, highlightCol);
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, highlightCol);
                    }

                    // Column 0: Row Click Target & Favorite Star
                    ImGui::TableNextColumn();
                    const ImVec2 cellStart = ImGui::GetCursorPos();

                    const float selectH = std::max(0.0f, rowH - ImGui::GetStyle().ItemSpacing.y);
                    std::string selectId = "##SelectPresetRow_" + std::to_string(displayedCount);
                    if (ImGui::Selectable(selectId.c_str(), false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, selectH))) {
                        loadPresetChain(pName);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Click to load preset (10ms crossfade)");
                    }

                    if (isAuditioned) {
                        ImVec2 rMin = ImGui::GetItemRectMin();
                        ImVec2 rMax = ImGui::GetItemRectMax();
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        dl->AddRectFilled(rMin, ImVec2(rMin.x + 3.5f, rMax.y), tokens.borders.focus.u32, 1.5f);
                    }

                    // Reset cursor back to start of cell so star is on the SAME row as the text and category
                    ImGui::SetCursorPos(cellStart);
                    Spec::alignTableCellCenter(20.0f, rowH);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
                    std::string starId = "##Fav_" + pName;
                    if (renderCenteredStarButton(starId.c_str(), ImVec2(20.0f, 20.0f), isFav)) {
                        toggleFavoritePreset(pName);
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(isFav ? "Remove from Favorites" : "Add to Favorites");
                    }

                    // Column 1: Preset Name (Vertically centered)
                    ImGui::TableNextColumn();
                    Spec::alignTableCellCenter(ImGui::GetFontSize(), rowH);
                    if (isAuditioned) {
                        ImGui::TextColored(tokens.text.accent.vec4, "%s", pName.c_str());
                    } else {
                        ImGui::TextUnformatted(pName.c_str());
                    }

                    // Column 2: Category Badge (Vertically centered)
                    ImGui::TableNextColumn();
                    const float badgeH = Spec::tableBadgeHeight();
                    Spec::alignTableCellCenter(badgeH, rowH);
                    ImU32 badgeBorder = tokens.borders.subtle.u32;
                    ImU32 badgeText = tokens.text.secondary.u32;
                    if (category == "High Gain Metal") {
                        badgeText = tokens.signal.recording.u32;
                        badgeBorder = tokens.signal.recording.u32;
                    } else if (category == "Glassy Clean") {
                        badgeText = tokens.text.accent.u32;
                        badgeBorder = tokens.text.accent.u32;
                    } else if (category == "Ambient FX") {
                        badgeText = tokens.signal.tunerFlat.u32;
                        badgeBorder = tokens.signal.tunerFlat.u32;
                    } else if (category == "Edge of Breakup") {
                        badgeText = tokens.signal.overdubbing.u32;
                        badgeBorder = tokens.signal.overdubbing.u32;
                    }

                    renderBadgePill(category.c_str(), tokens.surfaces.cardBg.u32, badgeBorder, badgeText, badgeH);
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Category: %s\nRight-click to change or create new category", category.c_str());
                    }

                    std::string catPopupId = "##CatPopup_" + pName;
                    if (ImGui::BeginPopupContextItem(catPopupId.c_str())) {
                        ImGui::TextColored(tokens.text.accent.vec4, "Set Category for '%s':", pName.c_str());
                        ImGui::Separator();
                        auto availableCats = m_scenes.allPresetCategories();
                        for (const auto& catItem : availableCats) {
                            bool isCurrent = (category == catItem);
                            if (ImGui::MenuItem(catItem.c_str(), nullptr, isCurrent)) {
                                m_scenes.setPresetCategory(pName, catItem);
                                triggerHudToast("Updated '" + pName + "' category to: " + catItem);
                            }
                        }
                        ImGui::Separator();
                        ImGui::TextDisabled("New Category:");
                        static char s_newCatBuffer[64] = "";
                        ImGui::SetNextItemWidth(160.0f);
                        ImGui::InputTextWithHint("##NewCatInput", "Category Name...", s_newCatBuffer, sizeof(s_newCatBuffer));
                        ImGui::SameLine();
                        if (CenteredButton("+ Add", ImVec2(50, Metrics::buttonHeight()))) {
                            if (s_newCatBuffer[0] != '\0') {
                                m_scenes.setPresetCategory(pName, s_newCatBuffer);
                                triggerHudToast("Created and set category: " + std::string(s_newCatBuffer));
                                s_newCatBuffer[0] = '\0';
                                ImGui::CloseCurrentPopup();
                            }
                        }
                        ImGui::EndPopup();
                    }
                }
                ImGui::EndTable();
                ImGui::PopStyleVar();
            }

            if (displayedCount == 0) {
                ImGui::Spacing();
                ImGui::TextDisabled("  No matching presets found.");
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);

        ImGui::Separator();
        ImGui::Spacing();
        if (CenteredButton("Close", ImVec2(90, Metrics::buttonHeight()))) {
            m_showPresetBrowserModal = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}





void RackView::triggerHudToast(std::string message) {
    m_hudToastText = std::move(message);
    m_hudToastTimer = kHudToastDuration;
}

void RackView::renderFloatingHudToast() {
    if (m_hudToastTimer <= 0.0f || m_hudToastText.empty()) return;

    m_hudToastTimer -= ImGui::GetIO().DeltaTime;
    if (m_hudToastTimer < 0.0f) m_hudToastTimer = 0.0f;

    const float alpha = computeHudToastAlpha(m_hudToastTimer, kHudToastDuration);
    if (alpha <= 0.001f) return;

    const auto& tokens = themeTokens();
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    const ImVec2 textSize = ImGui::CalcTextSize(m_hudToastText.c_str());
    const float padX = 24.0f;
    const float padY = 10.0f;
    const float boxW = textSize.x + padX * 2.0f + 20.0f;
    const float boxH = textSize.y + padY * 2.0f;

    const float posX = vp->Pos.x + (vp->Size.x - boxW) * 0.5f;
    const float posY = vp->Pos.y + 44.0f;

    ImGui::SetNextWindowPos(ImVec2(posX, posY));
    ImGui::SetNextWindowSize(ImVec2(boxW, boxH));
    ImGui::SetNextWindowBgAlpha(0.0f);

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoInputs |
                                  ImGuiWindowFlags_NoDecoration |
                                  ImGuiWindowFlags_AlwaysAutoResize |
                                  ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoFocusOnAppearing |
                                  ImGuiWindowFlags_NoNav |
                                  ImGuiWindowFlags_NoMove;

    if (ImGui::Begin("##FloatingHudToastOverlay", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const ImU32 bgCol = tokens.surfaces.cardBg.withAlpha(tokens.surfaces.cardBg.a * alpha * 0.95f).u32;
        const ImU32 borderCol = tokens.borders.focus.withAlpha(tokens.borders.focus.a * alpha * 0.90f).u32;
        const ImU32 textCol = tokens.text.primary.withAlpha(alpha).u32;
        const ImU32 ledCol = tokens.signal.active.withAlpha(alpha).u32;

        dl->AddRectFilled(ImVec2(posX, posY), ImVec2(posX + boxW, posY + boxH), bgCol, 8.0f);
        dl->AddRect(ImVec2(posX, posY), ImVec2(posX + boxW, posY + boxH), borderCol, 8.0f, 0, 1.5f);

        const ImVec2 dotCenter(posX + 16.0f, posY + boxH * 0.5f);
        dl->AddCircleFilled(dotCenter, 4.0f, ledCol, 16);

        const ImVec2 textPos(posX + 28.0f, posY + (boxH - textSize.y) * 0.5f);
        dl->AddText(textPos, textCol, m_hudToastText.c_str());
    }
    ImGui::End();
}

void RackView::saveLastRiff() {
    std::vector<float> stagingL;
    std::vector<float> stagingR;
    size_t capturedFrames = 0;
    double sampleRate = m_asio.isRunning() ? m_asio.currentSampleRate() : 48000.0;
    if (sampleRate <= 0.0) sampleRate = 48000.0;

    bool captured = m_riffBuffer.latchAndCopySlice(stagingL, stagingR, capturedFrames, sampleRate);
    if (!captured || capturedFrames == 0) {
        triggerHudToast("NO AUDIO RECORDED IN RIFF BUFFER");
        return;
    }
    stagingL.resize(capturedFrames);
    stagingR.resize(capturedFrames);

    std::filesystem::path riffDir;
    if (!m_cachedConfig.riffDirectory.empty()) {
        riffDir = m_cachedConfig.riffDirectory;
    } else {
        std::string userProfile = "";
        const char* up = std::getenv("USERPROFILE");
        if (up && *up) {
            userProfile = up;
        } else {
            userProfile = ".";
        }
        riffDir = std::filesystem::path(userProfile) / "Documents" / "Praccy" / "Riffs";
    }
    try {
        std::filesystem::create_directories(riffDir);
    } catch (...) {}

    auto now = std::time(nullptr);
    std::tm tmNow{};
#if defined(_WIN32)
    localtime_s(&tmNow, &now);
#else
    localtime_r(&now, &tmNow);
#endif

    std::string activePreset = m_auditionedPreset;
    if (activePreset.empty()) {
        int activeScene = m_scenes.activeSceneIndex();
        if (activeScene >= 0 && activeScene < static_cast<int>(m_scenes.numScenes())) {
            const auto* sc = m_scenes.getScene(activeScene);
            if (sc && !sc->name.empty()) {
                activePreset = sc->name;
            }
        }
    }
    if (activePreset.empty() && !m_favoritePresets.empty()) {
        activePreset = m_favoritePresets.front();
    }

    const std::string riffFileName = formatRiffFilename(activePreset, m_metronome.bpm(), tmNow);
    std::filesystem::path outPath = riffDir / riffFileName;

    uint32_t sr = static_cast<uint32_t>(sampleRate);
    std::string pathStr = outPath.string();

    utils::asyncWriteWav24Bit(pathStr, std::move(stagingL), std::move(stagingR), sr, nullptr);
    triggerHudToast("SAVED RIFF: " + riffFileName);
}

void RackView::cycleAdjacentScenePreset(int delta) {
    if (m_scenes.numScenes() == 0) return;
    int current = m_scenes.activeSceneIndex();
    int maxScenes = static_cast<int>(std::min<size_t>(m_scenes.numScenes(), 8));
    int next = std::clamp(current + delta, 0, maxScenes - 1);
    if (next != current) {
        if (m_scenes.applyScene(next, m_graph, true /* allowAsync */)) {
            syncMacrosFromGraph();
            const auto* sc = m_scenes.getScene(next);
            std::string name = sc ? sc->name : ("Preset " + std::to_string(next + 1));
            triggerHudToast("SCENE: " + name);
            m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, name),
                                       name,
                                       state::UndoActionType::SceneSwitch);
        }
    }
}

void RackView::initMacrosForSlot(audio::PluginSlot* slot) {
    if (!slot) return;
    if (slot->hasMacrosInitialized()) {
        m_slotMacros[slot] = slot->macros();
        return;
    }
    if (m_slotMacros.find(slot) != m_slotMacros.end()) {
        slot->macros() = m_slotMacros[slot];
        slot->setMacrosInitialized(true);
        return;
    }

    std::array<DeviceMacro, 4> macros;
    macros[0] = {"GAIN", 0, false, false, false, 0.0f, 0.0f, 0.0f, 1.0f};
    macros[1] = {"TONE", 0, false, false, false, 0.5f, 0.5f, 0.0f, 1.0f};
    macros[2] = {"VOL",  0, false, false, true,  slot->outputGainDb(), 0.0f, -24.0f, 12.0f};
    macros[3] = {"MIX",  0, false, true,  false, slot->dryWet() * 100.0f, 100.0f, 0.0f, 100.0f};

    auto* pInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    if (pInst) {
        static std::unordered_map<std::string, std::array<DeviceMacro, 4>> s_pluginMacroDiscoveryCache;
        std::string pluginKey = pInst->path().empty() ? pInst->name() : pInst->path();

        auto cacheIt = s_pluginMacroDiscoveryCache.find(pluginKey);
        if (cacheIt != s_pluginMacroDiscoveryCache.end()) {
            macros = cacheIt->second;
            for (int m = 0; m < 4; ++m) {
                if (macros[m].bound) {
                    macros[m].currentValue = pInst->getParameterValue(macros[m].paramId);
                } else if (macros[m].isSlotTrim) {
                    macros[m].currentValue = slot->outputGainDb();
                } else if (macros[m].isSlotMix) {
                    macros[m].currentValue = slot->dryWet() * 100.0f;
                }
            }
        } else {
            const std::regex regexGain(R"((gain|drive|dist|overdrive|input|crunch|preamp))", std::regex::icase);
            const std::regex regexTone(R"((tone|eq|treble|bass|mid|filter|bright|presence|cut))", std::regex::icase);
            const std::regex regexVol(R"((vol|volume|master|level|output|post))", std::regex::icase);
            const std::regex regexMix(R"((mix|blend|dry|wet))", std::regex::icase);

            auto scoreParam = [](const std::string& name, const std::string& target) -> int {
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                int score = 10;
                if (lower == target) score = 100;
                else if (lower.rfind("master", 0) == 0 || lower.rfind("main", 0) == 0) score = 80;
                else if (lower.find(target) != std::string::npos) score = 60;
                if (lower.find("ch 2") != std::string::npos || lower.find("ch2") != std::string::npos ||
                    lower.find("channel 2") != std::string::npos) score -= 20;
                return score;
            };

            int bestScore[4] = {-1, -1, -1, -1};
            size_t nParams = pInst->numParameters();
            for (size_t i = 0; i < nParams; ++i) {
                auto desc = pInst->getParameterDesc(i);
                if (std::regex_search(desc.name, regexGain)) {
                    int sc = scoreParam(desc.name, "gain");
                    if (sc > bestScore[0]) {
                        bestScore[0] = sc;
                        macros[0] = {desc.name, desc.id, true, false, false,
                                     pInst->getParameterValue(desc.id), desc.defaultValue, desc.minValue, desc.maxValue};
                    }
                }
                if (std::regex_search(desc.name, regexTone)) {
                    int sc = scoreParam(desc.name, "tone");
                    if (sc > bestScore[1]) {
                        bestScore[1] = sc;
                        macros[1] = {desc.name, desc.id, true, false, false,
                                     pInst->getParameterValue(desc.id), desc.defaultValue, desc.minValue, desc.maxValue};
                    }
                }
                if (std::regex_search(desc.name, regexVol)) {
                    int sc = scoreParam(desc.name, "volume");
                    if (sc > bestScore[2]) {
                        bestScore[2] = sc;
                        macros[2] = {desc.name, desc.id, true, false, false,
                                     pInst->getParameterValue(desc.id), desc.defaultValue, desc.minValue, desc.maxValue};
                    }
                }
                if (std::regex_search(desc.name, regexMix)) {
                    int sc = scoreParam(desc.name, "mix");
                    if (sc > bestScore[3]) {
                        bestScore[3] = sc;
                        macros[3] = {desc.name, desc.id, true, false, false,
                                     pInst->getParameterValue(desc.id), desc.defaultValue, desc.minValue, desc.maxValue};
                    }
                }
            }
            if (!pluginKey.empty()) {
                s_pluginMacroDiscoveryCache[pluginKey] = macros;
            }
        }
    }

    slot->macros() = macros;
    slot->setMacrosInitialized(true);
    m_slotMacros[slot] = macros;
}

void RackView::refreshGuiParamBaseline(audio::PluginSlot* slot) {
    if (!slot) return;
    auto* pInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    if (!pInst) return;

    const size_t nParams = pInst->numParameters();
    auto& baseline = m_guiParamBaseline[slot];
    baseline.resize(nParams);
    for (size_t p = 0; p < nParams; ++p) {
        auto desc = pInst->getParameterDesc(p);
        baseline[p] = desc.currentValue;
    }
}

void RackView::pollPluginGuiEdits(bool forceFlushPending) {
    auto pollSlot = [&](audio::PluginSlot* slot) {
        if (!slot) return;
        initMacrosForSlot(slot);
        auto* pluginInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
        if (!pluginInst) return;

        uint32_t editedParamId = 0;
        bool gestureEnded = false;
        bool hasGuiEdit = pluginInst->consumeGuiParamEdit(editedParamId, gestureEnded);

        const bool isWindowOpen = plugins::PluginWindowManager::instance().isWindowOpen(pluginInst);
        const size_t nParams = pluginInst->numParameters();

        if (m_guiParamBaseline.find(slot) == m_guiParamBaseline.end()) {
            refreshGuiParamBaseline(slot);
        }

        if (hasGuiEdit && editedParamId != 0xFFFFFFFFu) {
            float curVal = pluginInst->getParameterValue(editedParamId);
            slot->setParameter(editedParamId, curVal);
        }

        // Only scan the full parameter bank on bulk preset changes (0xFFFFFFFFu) or at a throttled 4 Hz cadence
        const bool throttledPollTick = (ImGui::GetFrameCount() % 15) == 0;
        if (((isWindowOpen && throttledPollTick) || (hasGuiEdit && editedParamId == 0xFFFFFFFFu)) && !ImGui::IsAnyItemActive()) {
            auto& baseline = m_guiParamBaseline[slot];
            if (baseline.size() != nParams) {
                refreshGuiParamBaseline(slot);
            } else {
                for (size_t p = 0; p < nParams; ++p) {
                    auto desc = pluginInst->getParameterDesc(p);
                    const float curVal = desc.currentValue;
                    if (std::abs(curVal - baseline[p]) > 1e-4f) {
                        baseline[p] = curVal;
                        slot->setParameter(desc.id, curVal);
                        if (!hasGuiEdit) {
                            editedParamId = desc.id;
                            hasGuiEdit = true;
                        }
                    }
                }
            }
        }

        if (hasGuiEdit) {
            // Sync front-panel macros with the updated plugin parameter values
            for (int m = 0; m < 4; ++m) {
                auto& mac = slot->macros()[m];
                if (mac.bound) {
                    mac.currentValue = pluginInst->getParameterValue(mac.paramId);
                }
            }
            m_slotMacros[slot] = slot->macros();

            std::string paramName;
            if (editedParamId != 0xFFFFFFFFu) {
                for (size_t p = 0; p < nParams; ++p) {
                    auto desc = pluginInst->getParameterDesc(p);
                    if (desc.id == editedParamId) {
                        paramName = desc.name;
                        break;
                    }
                }
            }

            m_pendingGuiEditSlot = slot;
            m_pendingGuiEditTargetKey = slot->name() + ":gui:" + std::to_string(editedParamId);
            m_pendingGuiEditDesc = paramName.empty() ? ("GUI: " + slot->name())
                                                     : (slot->name() + ": " + paramName);
        }

        if (m_pendingGuiEditSlot == slot) {
            const bool mouseDown = pluginInst->isGuiEditingActive() ||
                                   (isWindowOpen && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0));
            if (mouseDown && !gestureEnded && !forceFlushPending) {
                m_guiEditWasMouseDown = true;
            } else {
                refreshGuiParamBaseline(slot);
                for (int m = 0; m < 4; ++m) {
                    auto& mac = slot->macros()[m];
                    if (mac.bound) {
                        mac.currentValue = pluginInst->getParameterValue(mac.paramId);
                    }
                }
                m_slotMacros[slot] = slot->macros();

                m_undoManager.pushSnapshot(
                    state::SceneManager::captureGraphSnapshot(m_graph, m_pendingGuiEditDesc),
                    m_pendingGuiEditDesc,
                    state::UndoActionType::ParameterTweak,
                    m_pendingGuiEditTargetKey
                );
                if (gestureEnded || m_guiEditWasMouseDown || forceFlushPending) {
                    m_undoManager.commitActiveTransaction();
                }
                m_pendingGuiEditSlot = nullptr;
                m_pendingGuiEditTargetKey.clear();
                m_pendingGuiEditDesc.clear();
                m_guiEditWasMouseDown = false;
            }
        }
    };

    for (size_t i = 0; i < m_graph.numNodes(); ++i) {
        auto* node = m_graph.getNode(i);
        if (!node) continue;
        if (node->type() == audio::NodeType::Plugin) {
            pollSlot(dynamic_cast<audio::PluginSlot*>(node));
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            if (block) {
                for (size_t b = 0; b < block->numBranches(); ++b) {
                    auto* branch = block->getBranch(b);
                    if (!branch) continue;
                    for (size_t s = 0; s < branch->numSlots(); ++s) {
                        pollSlot(branch->getSlot(s));
                    }
                }
            }
        }
    }
}

void RackView::syncMacrosFromGraph() {
    m_pendingGuiEditSlot = nullptr;
    m_pendingGuiEditTargetKey.clear();
    m_pendingGuiEditDesc.clear();
    m_guiEditWasMouseDown = false;

    auto syncSlot = [&](audio::PluginSlot* slot) {
        if (!slot) return;
        if (!slot->hasMacrosInitialized()) {
            m_slotMacros.erase(slot);
            initMacrosForSlot(slot);
        }
        auto* pInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
        for (int m = 0; m < 4; ++m) {
            auto& mac = slot->macros()[m];
            if (mac.bound && pInst) {
                mac.currentValue = pInst->getParameterValue(mac.paramId);
            } else if (mac.isSlotTrim) {
                mac.currentValue = slot->outputGainDb();
            } else if (mac.isSlotMix) {
                mac.currentValue = slot->dryWet() * 100.0f;
            }
        }
        m_slotMacros[slot] = slot->macros();
        if (pInst) {
            pInst->clearGuiParamEdit();
        }
        refreshGuiParamBaseline(slot);
    };

    for (size_t i = 0; i < m_graph.numNodes(); ++i) {
        auto* node = m_graph.getNode(i);
        if (!node) continue;
        if (node->type() == audio::NodeType::Plugin) {
            syncSlot(dynamic_cast<audio::PluginSlot*>(node));
        } else if (node->type() == audio::NodeType::ParallelSplitMerge) {
            auto* block = dynamic_cast<audio::ParallelSplitMergeBlock*>(node);
            if (block) {
                for (size_t b = 0; b < block->numBranches(); ++b) {
                    auto* branch = block->getBranch(b);
                    if (!branch) continue;
                    for (size_t s = 0; s < branch->numSlots(); ++s) {
                        syncSlot(branch->getSlot(s));
                    }
                }
            }
        }
    }
}

void RackView::moveSlot(int srcIdx, int srcBlockIdx, int srcBranchIdx,
                        int dstIdx, int dstBlockIdx, int dstBranchIdx) {
    if (srcIdx < 0) return;

    // Check if source and target are the same location
    if (srcBlockIdx == dstBlockIdx && srcBranchIdx == dstBranchIdx) {
        if (dstIdx == srcIdx) return; // Same slot, no-op
    }

    std::string slotName = "Slot";

    // 1. Intra-serial reordering
    if (srcBranchIdx == -1 && dstBranchIdx == -1) {
        if (dstIdx < 0) {
            dstIdx = static_cast<int>(m_graph.numNodes()) - 1;
        }
        if (srcIdx == dstIdx) return;
        auto* node = m_graph.getNode(static_cast<size_t>(srcIdx));
        if (node) slotName = node->name();
        m_graph.reorderNodes(static_cast<size_t>(srcIdx), static_cast<size_t>(dstIdx));
    }
    // 2. Intra-branch reordering within the same branch
    else if (srcBlockIdx == dstBlockIdx && srcBranchIdx == dstBranchIdx && srcBranchIdx >= 0) {
        auto* blk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(srcBlockIdx));
        auto* br = blk ? blk->getBranch(srcBranchIdx) : nullptr;
        if (!br) return;
        if (dstIdx < 0) {
            dstIdx = static_cast<int>(br->numSlots()) - 1;
        }
        if (srcIdx == dstIdx) return;
        auto* slot = br->getSlot(static_cast<size_t>(srcIdx));
        if (slot) slotName = slot->name();
        br->reorderSlots(static_cast<size_t>(srcIdx), static_cast<size_t>(dstIdx));
    }
    // 3. From Serial Rack into a Branch
    else if (srcBranchIdx == -1 && dstBranchIdx >= 0) {
        if (m_asio.isRunning()) {
            auto scene = state::SceneManager::captureGraphSnapshot(m_graph, "Move");
            if (static_cast<size_t>(srcIdx) < scene.nodes.size()) {
                auto node = std::move(scene.nodes[srcIdx]);
                scene.nodes.erase(scene.nodes.begin() + srcIdx);
                slotName = node.slot.name;
                size_t actualDstBlock = static_cast<size_t>(dstBlockIdx);
                if (srcIdx < dstBlockIdx && actualDstBlock > 0) {
                    actualDstBlock--;
                }
                if (actualDstBlock < scene.nodes.size() && scene.nodes[actualDstBlock].kind == state::NodePreset::Kind::ParallelBlock) {
                    auto& branches = scene.nodes[actualDstBlock].branches;
                    if (static_cast<size_t>(dstBranchIdx) < branches.size()) {
                        auto& brSlots = branches[dstBranchIdx].slots;
                        size_t insertIdx = (dstIdx < 0 || static_cast<size_t>(dstIdx) >= brSlots.size()) ? brSlots.size() : static_cast<size_t>(dstIdx);
                        brSlots.insert(brSlots.begin() + insertIdx, std::move(node.slot));
                        m_scenes.applyGraphSnapshot(scene, m_graph);
                    }
                }
            }
        } else {
            auto node = m_graph.takeSerialNode(static_cast<size_t>(srcIdx));
            if (!node) return;
            slotName = node->name();
            auto* rawSlot = dynamic_cast<audio::PluginSlot*>(node.get());
            if (!rawSlot) {
                // Cannot put non-plugin node into a branch; put back in serial
                m_graph.insertSerialNode(static_cast<size_t>(srcIdx), std::move(node));
                return;
            }
            node.release();
            std::unique_ptr<audio::PluginSlot> slot(rawSlot);

            auto* blk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(dstBlockIdx));
            auto* br = blk ? blk->getBranch(dstBranchIdx) : nullptr;
            if (!br) {
                m_graph.insertSerialNode(static_cast<size_t>(srcIdx), std::move(slot));
                return;
            }

            if (dstIdx < 0 || static_cast<size_t>(dstIdx) >= br->numSlots()) {
                br->addSlot(std::move(slot));
            } else {
                br->insertSlot(static_cast<size_t>(dstIdx), std::move(slot));
            }
        }
    }
    // 4. From a Branch into Serial Rack
    else if (srcBranchIdx >= 0 && dstBranchIdx == -1) {
        if (m_asio.isRunning()) {
            auto scene = state::SceneManager::captureGraphSnapshot(m_graph, "Move");
            if (static_cast<size_t>(srcBlockIdx) < scene.nodes.size() && scene.nodes[srcBlockIdx].kind == state::NodePreset::Kind::ParallelBlock) {
                auto& branches = scene.nodes[srcBlockIdx].branches;
                if (static_cast<size_t>(srcBranchIdx) < branches.size()) {
                    auto& brSlots = branches[srcBranchIdx].slots;
                    if (static_cast<size_t>(srcIdx) < brSlots.size()) {
                        auto slotPreset = std::move(brSlots[srcIdx]);
                        brSlots.erase(brSlots.begin() + srcIdx);
                        slotName = slotPreset.name;

                        state::NodePreset newNode;
                        newNode.kind = state::NodePreset::Kind::Plugin;
                        newNode.slot = std::move(slotPreset);

                        size_t insertIdx = (dstIdx < 0 || static_cast<size_t>(dstIdx) >= scene.nodes.size()) ? scene.nodes.size() : static_cast<size_t>(dstIdx);
                        scene.nodes.insert(scene.nodes.begin() + insertIdx, std::move(newNode));
                        m_scenes.applyGraphSnapshot(scene, m_graph);
                    }
                }
            }
        } else {
            auto* blk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(srcBlockIdx));
            auto* br = blk ? blk->getBranch(srcBranchIdx) : nullptr;
            if (!br) return;
            auto slot = br->takeSlot(static_cast<size_t>(srcIdx));
            if (!slot) return;
            slotName = slot->name();

            if (dstIdx < 0 || static_cast<size_t>(dstIdx) >= m_graph.numNodes()) {
                m_graph.addSerialNode(std::move(slot));
            } else {
                m_graph.insertSerialNode(static_cast<size_t>(dstIdx), std::move(slot));
            }
        }
    }
    // 5. From one Branch into another Branch (Branch A -> Branch B, or Branch B -> Branch A)
    else if (srcBranchIdx >= 0 && dstBranchIdx >= 0) {
        if (m_asio.isRunning()) {
            auto scene = state::SceneManager::captureGraphSnapshot(m_graph, "Move");
            if (static_cast<size_t>(srcBlockIdx) < scene.nodes.size() && static_cast<size_t>(dstBlockIdx) < scene.nodes.size()) {
                auto& srcBlock = scene.nodes[srcBlockIdx];
                auto& dstBlock = scene.nodes[dstBlockIdx];
                if (srcBlock.kind == state::NodePreset::Kind::ParallelBlock && dstBlock.kind == state::NodePreset::Kind::ParallelBlock) {
                    if (static_cast<size_t>(srcBranchIdx) < srcBlock.branches.size() && static_cast<size_t>(dstBranchIdx) < dstBlock.branches.size()) {
                        auto& srcSlots = srcBlock.branches[srcBranchIdx].slots;
                        auto& dstSlots = dstBlock.branches[dstBranchIdx].slots;
                        if (static_cast<size_t>(srcIdx) < srcSlots.size()) {
                            auto slotPreset = std::move(srcSlots[srcIdx]);
                            srcSlots.erase(srcSlots.begin() + srcIdx);
                            slotName = slotPreset.name;

                            size_t insertIdx = (dstIdx < 0 || static_cast<size_t>(dstIdx) >= dstSlots.size()) ? dstSlots.size() : static_cast<size_t>(dstIdx);
                            dstSlots.insert(dstSlots.begin() + insertIdx, std::move(slotPreset));
                            m_scenes.applyGraphSnapshot(scene, m_graph);
                        }
                    }
                }
            }
        } else {
            auto* srcBlk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(srcBlockIdx));
            auto* srcBr = srcBlk ? srcBlk->getBranch(srcBranchIdx) : nullptr;
            if (!srcBr) return;
            auto slot = srcBr->takeSlot(static_cast<size_t>(srcIdx));
            if (!slot) return;
            slotName = slot->name();

            auto* dstBlk = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(dstBlockIdx));
            auto* dstBr = dstBlk ? dstBlk->getBranch(dstBranchIdx) : nullptr;
            if (!dstBr) {
                srcBr->insertSlot(static_cast<size_t>(srcIdx), std::move(slot));
                return;
            }

            if (dstIdx < 0 || static_cast<size_t>(dstIdx) >= dstBr->numSlots()) {
                dstBr->addSlot(std::move(slot));
            } else {
                dstBr->insertSlot(static_cast<size_t>(dstIdx), std::move(slot));
            }
        }
    }

    syncMacrosFromGraph();
    triggerHudToast("MOVED: " + slotName);
    m_undoManager.pushSnapshot(
        state::SceneManager::captureGraphSnapshot(m_graph, "Move " + slotName),
        "Move " + slotName,
        state::UndoActionType::StructuralMutation
    );
}

void RackView::renderHotkeyLegendOverlay() {
    const auto& tokens = themeTokens();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (!vp) return;

    const float overlayW = 440.0f;
    const ImVec2 anchorPos(vp->Pos.x + vp->Size.x - 14.0f,
                           vp->Pos.y + vp->Size.y - 42.0f);

    ImGui::SetNextWindowPos(anchorPos, ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(overlayW, 0.0f), ImGuiCond_Always);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, tokens.surfaces.popupBg.vec4);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.borders.focus.vec4);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##HotkeyLegendOverlayWindow", nullptr, flags)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(tokens.text.accent.vec4, "KEYBOARD SHORTCUTS");

        ImGui::SameLine();
        ImGui::TextDisabled("(Hold Alt or toggle ⌨)");

        if (m_showHotkeyLegend) {
            ImGui::SameLine(ImGui::GetWindowWidth() - 32.0f);
            if (ImGui::SmallButton("x")) {
                m_showHotkeyLegend = false;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Dismiss Hotkey Legend");
            }
        }

        ImGui::Separator();
        ImGui::Spacing();

        auto renderKeyRow = [&](const char* keys, const char* desc) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None, Spec::tableCompactRowHeight());
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(ImGui::GetFontSize(), Spec::tableCompactRowHeight());
            if (g_fontMono) ImGui::PushFont(g_fontMono);
            ImGui::TextColored(tokens.text.accent.vec4, "%s", keys);
            if (g_fontMono) ImGui::PopFont();
            ImGui::TableNextColumn();
            Spec::alignTableCellCenter(ImGui::GetFontSize(), Spec::tableCompactRowHeight());
            ImGui::TextUnformatted(desc);
        };

        if (ImGui::BeginTable("##HotkeyTable", 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);

            renderKeyRow("Ctrl + Z", "Undo last parameter or slot edit");
            renderKeyRow("Ctrl + Y / Shift+Z", "Redo last action");
            renderKeyRow("Ctrl + Shift + R", "Save retroactive riff (WAV)");
            renderKeyRow("Ctrl + P / Q", "Spotlight plugin browser & search");
            renderKeyRow("Ctrl + E", "Export signal chain as DAW preset");
            renderKeyRow("Ctrl + M", "Toggle MIDI learn mapping mode");
            renderKeyRow("Ctrl + K", "Toggle key mapping mode");
            renderKeyRow("1 - 8", "Recall preset scenes 1 through 8");
            renderKeyRow("[  /  ]", "Switch adjacent preset scene");
            renderKeyRow("0", "Bypass / activate selected plugin");
            renderKeyRow("Esc", "Close active popup / cancel mode");
            renderKeyRow("Alt", "Hold to show this shortcut legend");

            ImGui::EndTable();
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void RackView::processKeyMapCapture() {
    if (!m_keyMapMode || !m_awaitingKeyTarget.active) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_awaitingKeyTarget.active = false;
        triggerHudToast("KEY MAP CANCELLED");
        return;
    }

    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const auto key = static_cast<ImGuiKey>(k);
        const bool isTargetKey = (key >= ImGuiKey_0 && key <= ImGuiKey_9) ||
                                 (key >= ImGuiKey_A && key <= ImGuiKey_Z) ||
                                 (key >= ImGuiKey_F1 && key <= ImGuiKey_F12);
        if (!isTargetKey) {
            continue;
        }

        if (ImGui::IsKeyPressed(key, false)) {
            auto& bindings = m_config.keyBindings;
            bindings.erase(
                std::remove_if(bindings.begin(), bindings.end(),
                    [this, k](const state::KeyBindingEntry& entry) {
                        const bool sameKey = (entry.key == k);
                        const bool sameTarget = (entry.slotIndex == m_awaitingKeyTarget.slotIndex &&
                                                 entry.branchIndex == m_awaitingKeyTarget.branchIndex &&
                                                 entry.branchSlotIndex == m_awaitingKeyTarget.branchSlotIndex &&
                                                 entry.macroIndex == m_awaitingKeyTarget.macroIndex &&
                                                 entry.targetType == m_awaitingKeyTarget.targetType &&
                                                 entry.paramId == m_awaitingKeyTarget.paramId);
                        return sameKey || sameTarget;
                    }),
                bindings.end());

            state::KeyBindingEntry newEntry;
            newEntry.key = k;
            newEntry.slotIndex = m_awaitingKeyTarget.slotIndex;
            newEntry.branchIndex = m_awaitingKeyTarget.branchIndex;
            newEntry.branchSlotIndex = m_awaitingKeyTarget.branchSlotIndex;
            newEntry.macroIndex = m_awaitingKeyTarget.macroIndex;
            newEntry.targetType = m_awaitingKeyTarget.targetType;
            newEntry.paramId = m_awaitingKeyTarget.paramId;
            bindings.push_back(newEntry);
            m_config.save();

            const char* keyName = ImGui::GetKeyName(key);
            std::string toast = "KEY BOUND: [" + std::string(keyName ? keyName : "?") + "] -> [" + m_awaitingKeyTarget.targetLabel + "]";
            triggerHudToast(toast);

            m_awaitingKeyTarget.active = false;
            break;
        }
    }
}

int RackView::getBoundKeyForTarget(int slotIndex, int blockIndex, int branchIndex, int branchSlotIndex, uint32_t targetType, uint32_t paramId) const {
    const int effSlot = (blockIndex >= 0 ? blockIndex : slotIndex);
    const int effBranchSlot = (blockIndex >= 0 ? (branchSlotIndex >= 0 ? branchSlotIndex : slotIndex) : branchSlotIndex);
    for (const auto& entry : m_config.keyBindings) {
        if (entry.slotIndex == effSlot &&
            entry.branchIndex == branchIndex &&
            entry.branchSlotIndex == effBranchSlot &&
            entry.targetType == targetType &&
            entry.paramId == paramId) {
            return entry.key;
        }
    }
    return 0;
}

void RackView::dispatchKeyBindings() {
    for (const auto& entry : m_config.keyBindings) {
        if (entry.key == 0) continue;
        const auto key = static_cast<ImGuiKey>(entry.key);
        if (!ImGui::IsKeyPressed(key, false)) {
            continue;
        }

        audio::PluginSlot* slot = nullptr;
        audio::ParallelSplitMergeBlock* block = nullptr;

        if (entry.branchIndex == -1) {
            if (entry.slotIndex >= 0 && static_cast<size_t>(entry.slotIndex) < m_graph.numNodes()) {
                slot = dynamic_cast<audio::PluginSlot*>(m_graph.getNode(static_cast<size_t>(entry.slotIndex)));
            }
        } else {
            if (entry.slotIndex >= 0 && static_cast<size_t>(entry.slotIndex) < m_graph.numNodes()) {
                block = dynamic_cast<audio::ParallelSplitMergeBlock*>(m_graph.getNode(static_cast<size_t>(entry.slotIndex)));
                if (block && entry.branchIndex >= 0 && static_cast<size_t>(entry.branchIndex) < block->numBranches()) {
                    auto* br = block->getBranch(static_cast<size_t>(entry.branchIndex));
                    if (br && entry.branchSlotIndex >= 0 && static_cast<size_t>(entry.branchSlotIndex) < br->numSlots()) {
                        slot = br->getSlot(static_cast<size_t>(entry.branchSlotIndex));
                    }
                }
            }
        }

        if (entry.targetType == static_cast<uint32_t>(midi::BindingTargetType::BranchMute)) {
            if (block && entry.branchIndex >= 0 && static_cast<size_t>(entry.branchIndex) < block->numBranches()) {
                auto* br = block->getBranch(static_cast<size_t>(entry.branchIndex));
                if (br) {
                    br->setMuted(!br->isMuted());
                    triggerHudToast(std::string("BRANCH ") + (entry.branchIndex == 0 ? "A" : "B") + (br->isMuted() ? " MUTED" : " UNMUTED"));
                    m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Toggle Branch Mute"),
                                               "Branch Mute",
                                               state::UndoActionType::ParameterTweak);
                    m_undoManager.commitActiveTransaction();
                }
            }
        } else if (entry.targetType == static_cast<uint32_t>(midi::BindingTargetType::BranchSolo)) {
            if (block && entry.branchIndex >= 0 && static_cast<size_t>(entry.branchIndex) < block->numBranches()) {
                auto* br = block->getBranch(static_cast<size_t>(entry.branchIndex));
                if (br) {
                    br->setSolo(!br->isSolo());
                    triggerHudToast(std::string("BRANCH ") + (entry.branchIndex == 0 ? "A" : "B") + (br->isSolo() ? " SOLO ON" : " SOLO OFF"));
                    m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Toggle Branch Solo"),
                                               "Branch Solo",
                                               state::UndoActionType::ParameterTweak);
                    m_undoManager.commitActiveTransaction();
                }
            }
        } else if (entry.targetType == static_cast<uint32_t>(midi::BindingTargetType::SlotBypass)) {
            if (slot) {
                slot->setBypassed(!slot->isBypassed());
                triggerHudToast(slot->name() + (slot->isBypassed() ? ": BYPASSED" : ": ACTIVE"));
                m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Bypass " + slot->name()),
                                           "Bypass: " + slot->name(),
                                           state::UndoActionType::ParameterTweak,
                                           slot->name() + ":Bypass",
                                           true);
                m_undoManager.commitActiveTransaction();
            }
        } else if (entry.targetType == static_cast<uint32_t>(midi::BindingTargetType::SlotDryWet)) {
            if (slot) {
                const float cur = slot->dryWet();
                const float next = (cur > 0.5f) ? 0.0f : 1.0f;
                slot->setDryWet(next);
                for (size_t m = 0; m < 4; ++m) {
                    if (slot->macros()[m].isSlotMix) {
                        slot->macros()[m].currentValue = next * 100.0f;
                        m_slotMacros[slot][m].currentValue = next * 100.0f;
                    }
                }
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s MIX: %.0f%%", slot->name().c_str(), static_cast<double>(next * 100.0f));
                triggerHudToast(buf);
                m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Mix " + slot->name()),
                                           "Mix: " + slot->name(),
                                           state::UndoActionType::ParameterTweak,
                                           slot->name() + ":Mix",
                                           true);
                m_undoManager.commitActiveTransaction();
            }
        } else if (entry.targetType == static_cast<uint32_t>(midi::BindingTargetType::SlotOutputGain)) {
            if (slot) {
                const float cur = slot->outputGainDb();
                const float next = (std::abs(cur) < 0.1f) ? -24.0f : 0.0f;
                slot->setOutputGainDb(next);
                for (size_t m = 0; m < 4; ++m) {
                    if (slot->macros()[m].isSlotTrim) {
                        slot->macros()[m].currentValue = next;
                        m_slotMacros[slot][m].currentValue = next;
                    }
                }
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s TRIM: %+.1f dB", slot->name().c_str(), static_cast<double>(next));
                triggerHudToast(buf);
                m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Trim " + slot->name()),
                                           "Trim: " + slot->name(),
                                           state::UndoActionType::ParameterTweak,
                                           slot->name() + ":Trim",
                                           true);
                m_undoManager.commitActiveTransaction();
            }
        } else if (entry.targetType == static_cast<uint32_t>(midi::BindingTargetType::PluginParameter)) {
            const int rawMacroIdx = (entry.macroIndex >= 0 && entry.macroIndex < 4) ? entry.macroIndex : entry.branchSlotIndex;
            if (slot && rawMacroIdx >= 0 && rawMacroIdx < 4) {
                const size_t mIdx = static_cast<size_t>(rawMacroIdx);
                auto& macro = slot->macros()[mIdx];
                const float span = (macro.maxValue > macro.minValue) ? (macro.maxValue - macro.minValue) : 1.0f;
                float val = macro.currentValue + 0.25f * span;
                if (val > macro.maxValue + 0.01f * span) {
                    val = macro.minValue;
                }
                val = std::clamp(val, macro.minValue, macro.maxValue);
                macro.currentValue = val;
                m_slotMacros[slot][mIdx].currentValue = val;

                if (macro.isSlotMix) {
                    slot->setDryWet(val / 100.0f);
                } else if (macro.isSlotTrim) {
                    slot->setOutputGainDb(val);
                } else {
                    slot->setParameter(macro.paramId, val);
                }
                refreshGuiParamBaseline(slot);
                const float normPct = ((val - macro.minValue) / span) * 100.0f;
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%s %s: %.0f%%", slot->name().c_str(), macro.label.c_str(), static_cast<double>(normPct));
                triggerHudToast(buf);
                m_undoManager.pushSnapshot(state::SceneManager::captureGraphSnapshot(m_graph, "Macro " + macro.label),
                                           "Macro: " + macro.label,
                                           state::UndoActionType::ParameterTweak,
                                           slot->name() + ":" + macro.label,
                                           true);
                m_undoManager.commitActiveTransaction();
            }
        }
    }
}

} // namespace praccy::ui
