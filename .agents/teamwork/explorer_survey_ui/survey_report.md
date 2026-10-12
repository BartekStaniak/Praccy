# Praccy V2.4 Stage 1 UI Ergonomics & Visual Polish Survey Report

**Author**: UI Ergonomics & Visual Polish Explorer  
**Date**: 2026-10-12  
**Milestone**: Praccy V2.4 Stage 1 (Musician Velocity & Pro-Audio Workflow Engine)  
**Scope**: Requirements R3, R4, R5, Gates 1.4 & 1.5, and Feature/Workflow Acceptance Criteria  

---

## 1. Executive Summary

This survey provides a comprehensive architectural and code-level investigation of Praccy's existing user interface, state management, design tokens, and ergonomics subsystems. Praccy V2.0 has established a stable modular UI foundation with a clean 4-theme design token system (`src/ui/design_tokens.h`), a custom D3D11/ImGui swapchain (`src/main.cpp`), and 10 passing CTest targets. 

However, V2.4 Stage 1 introduces demanding pro-audio ergonomics and visual fidelity requirements:
1. **R3 (Transactional Undo/Redo & State Management)**: Currently, no `UndoManager` exists; preset switching and parameter tweaks are destructive and non-revertible. No A/B comparison toggle exists in `SceneBar`.
2. **R4 (Front-Panel Macros, Tactile Rack Physics & Performance Ergonomics)**: Cards currently display only fixed mix/trim sliders without device macro knobs. `renderRotaryKnob` is declared in `rack_view.h` but is unimplemented in `rack_view.cpp`. There is zero horizontal drag-and-drop card reordering physics, no Global MIDI/Key Map Mode with pulsating violet borders, no focused card shortcuts, and no 800-point overview waveform strip in Backing Track.
3. **R5 (Visual Polish, Design Token Strictness & List Virtualization)**: The Spotlight browser (`PluginBrowserModal`) iterates linearly through all scanned plugins without `ImGuiListClipper`, causing frame drops with large plugin libraries. The Spotlight footer text collides at >100% DPI due to hardcoded pixel offsets. Level meters lack DIN IEC 60268-10 PPM ballistics and 4-segment vertex-colored bars. Cables lack audio-reactive glow envelopes. Top ribbon padding, looper idle contrast, and active scene button styling require standardization.

This report documents the exact delta, defines all required classes, structs, and function signatures, establishes interface contracts, and details the verification path.

---

## 2. Current State Analysis

### 2.1 State Management & Presets (`src/state/scene_manager.*`)
- **Existing Assets**:
  - `SceneManager` manages up to 8 default scene presets (`s1: Clean`, `s2: Crunch`, `s3: Lead`, `s4: Ambient`) plus user-saved preset chains stored in `presets.ini`.
  - Defined structs: `PluginSlotPreset`, `BranchPreset`, `NodePreset`, `ScenePreset` in `src/state/scene_manager.h` (lines 11–42).
  - Snapshot Capture & Application:
    - `captureCurrentScene(int sceneIndex, audio::GraphEngine& graph)` (lines 159–246 of `scene_manager.cpp`) iterates through graph nodes, serializes parameters (`dryWet`, `inputGainDb`, `outputGainDb`), plugin state blobs (`inner->saveState()`), and parallel branch topology.
    - `applyScene(int sceneIndex, audio::GraphEngine& graph)` (lines 248–292 of `scene_manager.cpp`) instantiates slot nodes and triggers `graph.crossfadeToNodes(...)` with 10ms click-free crossfade.
- **Current Deficiencies**:
  - **No `UndoManager`**: Zero undo/redo history tracking exists. There is no history stack, no transaction coalescing state machine, and no undo/redo hotkey handler.
  - **No A/B Memory State**: `SceneManager` tracks only `m_activeSceneIndex` (line 72). It does not hold temporary A/B preset snapshot buffers.

### 2.2 Rack View & Signal Chain (`src/ui/rack_view.*`)
- **Existing Assets**:
  - Monolithic rack controller (`src/ui/rack_view.cpp`, 2,348 lines).
  - Rendering pipeline: `renderPracticeRibbon()` -> `renderSceneBar()` -> `renderSignalRack()` -> `renderBottomBar()`.
  - Cable rendering: `drawCubicHermiteCable()` (lines 32–124) renders 5 visual layers with Bezier drop shadow, outer sleeve (3.5px), core wire (1.8px), socket pins, and animated pulse dots.
  - Card rendering: `renderPluginSlot()` (lines 1049–1415) renders 240x224px cards with header bar (badge pill, truncated title, split button, delete button, sliding pill active toggle), body preview (thumbnail image or procedural vector sine polyline), parameter readout header, and two sliders (`Mix: 0..100%`, `Trim: -24..+12dB`).
- **Current Deficiencies**:
  - **Unimplemented Rotary Knob**: `bool renderRotaryKnob(...)` is declared at `src/ui/rack_view.h:61` but is never defined in `rack_view.cpp` (empty gap lines 2052–2060).
  - **No Front-Panel Macros**: Plugin cards lack the 4 front-panel mini-knob macro controls.
  - **No Card Drag-and-Drop**: Rack cards are rendered at static horizontal offsets (`currentX += cardW + wireW`). No `ImGui::BeginDragDropSource` or `BeginDragDropTarget` exists for reordering plugin nodes.
  - **No Global Map Mode**: No `Ctrl+M` (MIDI Map Mode) or `Ctrl+K` (Key Map Mode). No pulsating violet canvas border.
  - **No Focused Card Shortcuts**: No keyboard navigation for `[` / `]` preset cycling, `Q` pre-filtered Spotlight, or `0` bypass toggle.
  - **Meter Ballistics**: `renderMeter()` (lines 2029–2051) draws a single unsegmented rectangle filled with a solid color. It lacks DIN IEC 60268-10 ballistics, peak-hold bars, and vertex-colored multi-segment rendering.
  - **Missing Dock Controls**: Bottom bar lacks `[★ Save Last Riff]` button and retroactive riff capture UI.

### 2.3 Modals & Spotlight Browser (`src/ui/modals/*`)
- **`PluginBrowserModal` (`src/ui/modals/plugin_browser_modal.*`)**:
  - Keyboard-driven modal palette (560x420px) with fuzzy search filtering across name, vendor, category, and format.
  - **Virtualization Defect**: Lines 372–458 loop over all items in `m_filteredItems`. For libraries with 1,500+ plugins, 1,500 full ImGui selectable/text items are evaluated every frame, dropping FPS significantly below 144 FPS.
  - **DPI Collision Defect**: Lines 464–466 position the footer plugin count at `SameLine(modalSize.x - 170.0f)`. At 125%, 150%, and 200% DPI, the left shortcut description `[UP/DOWN] Navigate    [ENTER] Insert Plugin    [ESC] Dismiss` exceeds 390px, causing the two text elements to collide and overlap.
  - **No Favorites / Pinned Scope**: `m_recentPlugins` exists, but there is no `Tab` key scope cycling (`All` / `Favorites` / `Recents`), no `F` key favorite toggling, no pinned zero-query sorting for favorites, and no search score boosting.
  - **Format Suffix Clutter**: Plugins named with `.vst3` or `.clap` suffixes display redundant extensions alongside their format badges.
- **`PracticeToolsModal` (`src/ui/modals/practice_tools_modal.*`)**:
  - Contains Quick Looper tab and Backing Track Player tab.
  - Looper circular ring: lines 28–116 render a circular arc with state colors (REC, DUB, PLAY, STOP, READY).
  - **Contrast Defect**: Looper idle/empty state text uses `tokens.borders.subtle`, which falls below WCAG AA contrast ratio. Needs promotion to `#9CA3AF`.
  - **Backing Track Defect**: Lines 247–251 use a plain `ImGui::SliderFloat("##PlayerSeek", ...)` without an overview waveform or interactive loop points.
- **`SettingsModal` (`src/ui/modals/settings_modal.*`)**:
  - Contains Audio Tab (ASIO driver, buffer size, latency), Plugins Tab (search paths with native `IFileOpenDialog`), Themes Tab, Updates Tab, and About Tab.
  - Lacks Retroactive Buffer duration selection (1, 2, 5, 10 minutes) and RAM footprint telemetry HUD.

### 2.4 Design System Tokens & Color Linter (`src/ui/design_tokens.h`, `scripts/check_hardcoded_colors.py`)
- **Current Status**:
  - `py scripts/check_hardcoded_colors.py` passes with **0 violations**:
    `SUCCESS: Clean! 0 hardcoded IM_COL32 / ImVec4 literals found in F:\Projects\Praccy\src\ui.`
  - `design_tokens.h` defines 4 switchable themes (`ObsidianStudio`, `CyberMidnight`, `NordicSlate`, `VintageConsole`).
  - Scrim Alpha: Currently `tokens.surfaces.modalOverlay` uses alpha values 215–220 (~84%). Needs alignment to 0.75 alpha (191 / 255).
  - Active Scene Color: Currently uses `tokens.signal.active` (bright neon green). Needs dark emerald surface (`#104B2D` / `rgba(16, 75, 45, 0.90)`) with focus border.

---

## 3. Required Classes, Structs, and UI Functions

### 3.1 R3. Transactional Undo/Redo & State Management

#### Class: `praccy::state::UndoManager`
- **Location**: `src/state/undo_manager.h`, `src/state/undo_manager.cpp`
- **Specification**:
  - Maximum history capacity: `static constexpr size_t kMaxHistoryLevels = 64;`
  - Ring buffer or `std::deque<UndoTransaction>` capped at 64 elements.
  - Discard oldest record on push when capacity is reached.
- **Data Structures**:
  ```cpp
  namespace praccy::state {

  enum class UndoActionType : uint8_t {
      ParameterTweak,     // Continuous knob/slider drag
      StructuralMutation, // Add slot, delete slot, reorder, split, dissolve
      SceneSwitch,        // Preset recalled or scene button pressed
      MidiCcAutomation    // External MIDI CC streaming
  };

  struct UndoTransaction {
      ScenePreset snapshot;
      std::string description;
      UndoActionType actionType{UndoActionType::ParameterTweak};
      std::string targetIdentifier; // Slot/Param key for continuous coalescing
      std::chrono::steady_clock::time_point timestamp;
  };

  class UndoManager {
  public:
      UndoManager() = default;

      /// Record a state snapshot with coalescing logic
      void pushSnapshot(ScenePreset snapshot,
                        std::string description,
                        UndoActionType actionType,
                        const std::string& targetKey = "");

      /// Finalize pending coalescing group (e.g. on mouse release)
      void commitActiveTransaction() noexcept;

      /// Check if undo/redo stacks are available
      [[nodiscard]] bool canUndo() const noexcept;
      [[nodiscard]] bool canRedo() const noexcept;

      /// Restore previous state (applies to graph with 10ms crossfade)
      bool undo(audio::GraphEngine& graph, SceneManager& scenes);

      /// Re-apply next state (applies to graph with 10ms crossfade)
      bool redo(audio::GraphEngine& graph, SceneManager& scenes);

      /// Clear history
      void clear() noexcept;

      [[nodiscard]] size_t undoCount() const noexcept;
      [[nodiscard]] size_t redoCount() const noexcept;
      [[nodiscard]] const std::string& lastActionDescription() const noexcept;

  private:
      static constexpr size_t kMaxHistoryLevels = 64;
      static constexpr int64_t kCoalesceIdleThresholdMs = 400;

      std::deque<UndoTransaction> m_undoStack;
      std::vector<UndoTransaction> m_redoStack;
      std::string m_lastActionDesc;
      bool m_inContinuousGroup{false};
  };

  } // namespace praccy::state
  ```
- **Coalescing Rules & Logic**:
  1. **Continuous Parameter Sweeps**: When dragging a slider or macro knob, `pushSnapshot` is invoked repeatedly. If `actionType == ParameterTweak` and `targetKey` matches the active transaction and `now - timestamp < 400ms`, the top snapshot's final state is overwritten in place; no new history level is created.
  2. **Mouse Release**: `commitActiveTransaction()` is called when `ImGui::IsItemDeactivatedAfterEdit()` evaluates to true, locking the transaction.
  3. **Inactivity Timeout**: If `now - timestamp >= 400ms`, subsequent tweaks begin a new discrete transaction.
  4. **Structural Mutations & Scene Switches**: Always push as discrete transactions immediately clearing the redo stack.

#### SceneBar A/B Tone Snapshot Comparison Toggle
- **Location**: `src/ui/rack_view.cpp` (`renderSceneBar()`)
- **Specification**:
  - Two memory slots: `ScenePreset m_toneSnapshotA` and `ScenePreset m_toneSnapshotB`.
  - State flag: `enum class ToneSnapshotSlot { None, A, B } m_activeToneSnapshot{ToneSnapshotSlot::A};`
  - Toggle UI: Two segmented pill buttons `[ A ]` and `[ B ]` rendered in `renderSceneBar()` alongside scene presets.
  - When user clicks `[ B ]`:
    - If `m_toneSnapshotA` is empty, current graph state is captured into `m_toneSnapshotA`.
    - If `m_toneSnapshotB` is populated, graph is smoothly transitioned using `graph.crossfadeToNodes(...)` with an equal-power 10ms ramp.
    - If `m_toneSnapshotB` is empty, current state is cloned to B as a baseline.
  - A small `[Store -> B]` / `[Copy A->B]` button allows copying snapshot memory.
  - Active snapshot is highlighted with `tokens.borders.focus` and active emerald fill.

---

### 3.2 R4. Front-Panel Macros, Tactile Rack Physics & Performance Ergonomics

#### Front-Panel Macro Controls & Heuristic Discovery
- **Location**: `src/ui/rack_view.cpp` (`renderPluginSlot()`), `src/audio/graph_engine.h` (`PluginSlot`)
- **Mini-Knob Specifications**:
  - 4 tactile mini-knobs per card (radius $r = 11.0\text{px}$, diameter $22\text{px}$).
  - Angular sweep: $270^\circ$ (from $-135^\circ$ to $+135^\circ$, where $0^\circ$ is 12 o'clock).
  - Mouse sensitivity: vertical drag with fine control when Shift is held.
  - Double-click resets to default parameter value.
- **Fuzzy Regex Discovery Heuristics**:
  Each `PluginSlot` discovers 4 macros upon plugin instantiation via case-insensitive regex against `PluginParameterDesc::name`:
  1. **Macro 1 — Gain / Drive**: `(?i)(gain|drive|dist|overdrive|input|crunch|preamp)`
  2. **Macro 2 — Tone / EQ**: `(?i)(tone|eq|treble|bass|mid|filter|bright|presence|cut)`
  3. **Macro 3 — Volume / Master**: `(?i)(vol|volume|master|level|output|post)`
  4. **Macro 4 — Mix / Blend**: `(?i)(mix|blend|dry|wet)`
- **Macro State Struct**:
  ```cpp
  struct DeviceMacro {
      std::string label;       // "GAIN", "TONE", "VOL", "MIX"
      uint32_t paramId{0};     // Plugin parameter ID
      bool bound{false};
      float currentValue{0.0f};
      float defaultValue{0.0f};
      float minValue{0.0f};
      float maxValue{1.0f};
  };
  ```
- **Context Menu Reassignment**: Right-clicking any macro mini-knob opens `ImGui::BeginPopupContextItem()` displaying all plugin parameters (`pInst->getParameterDesc(i)`), allowing manual reassignment or unbinding.

#### Implementation of `renderRotaryKnob`
- **Location**: `src/ui/rack_view.cpp` (implementing `rack_view.h:61`)
- **Signature**:
  ```cpp
  bool RackView::renderRotaryKnob(const char* label, float* value, float minVal, float maxVal,
                                  float radius, const char* format, float defaultVal);
  ```
- **Geometry & Rendering**:
  - Center track circle stroked with `tokens.surfaces.frameBg`.
  - Value arc stroked with `tokens.text.accent` from $-135^\circ$ to angle $\theta = -135^\circ + 270^\circ \cdot \text{norm}$.
  - Indicator needle line from center towards rim stroked with `tokens.text.primary`.
  - Micro parameter label rendered centered beneath the knob.

#### Horizontal Drag-and-Drop Card Reordering Physics
- **Location**: `src/ui/rack_view.cpp` (`renderSignalRack()`)
- **Payload Protocol**:
  ```cpp
  struct CardDragPayload {
      int sourceIndex{-1};
      int sourceBlockIndex{-1};
      int sourceBranchIndex{-1};
  };
  ```
- **Drag Source**: Card headers wrap an invisible drag region:
  ```cpp
  if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
      CardDragPayload payload{slotIndex, blockIndex, branchIndex};
      ImGui::SetDragDropPayload("RACK_CARD_PAYLOAD", &payload, sizeof(payload));
      // Ghost card preview
      ImGui::Text("Moving: %s", slot->name().c_str());
      ImGui::EndDragDropSource();
  }
  ```
- **Drag Target**: Inter-card drop indicators (20px width between cards) and card rectangles register as drop targets:
  ```cpp
  if (ImGui::BeginDragDropTarget()) {
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("RACK_CARD_PAYLOAD")) {
          const auto* pl = static_cast<const CardDragPayload*>(p->Data);
          // Execute node reordering
          executeCardReorder(pl->sourceIndex, targetIndex);
      }
      ImGui::EndDragDropTarget();
  }
  ```
- **Real-Time Safety Execution**:
  - `GraphEngine::moveSerialNode(size_t fromIndex, size_t toIndex)` executes the pointer swap within `m_nodes` protected by `m_graphMutex`.
  - Equal-power 64-sample micro-crossfade between adjacent slots ensures zero clicks or transient pops during live dragging.

#### Global MIDI & Key Map Mode
- **Location**: `src/ui/rack_view.cpp` (`render()`, `renderBottomBar()`)
- **Keybindings**: `Ctrl+M` (Toggle MIDI Map Mode), `Ctrl+K` (Toggle Key Map Mode).
- **Pulsating Violet Border**:
  When active, a full-viewport pulsating border is rendered on the foreground draw list:
  $$\alpha(t) = 0.45f + 0.45f \cdot \sin(4.0 \cdot t)$$
  Drawn with thickness 3.0px using `tokens.borders.mapModeViolet` (`rgba(168, 85, 247, alpha)`).
- **Click-to-Learn Overlay**: Mappable controls (macro knobs, bypass pills, volume sliders, scene buttons) display a translucent violet hover overlay. Clicking any control arms it for the next incoming MIDI CC or keyboard scancode.
- **Strict `!io.WantTextInput` Guard**:
  ```cpp
  ImGuiIO& io = ImGui::GetIO();
  if (!io.WantTextInput) {
      // Only process global hotkeys when NOT typing into text fields
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { ... }
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M, false)) { ... }
      if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false)) { ... }
  }
  ```

#### Focused Card Shortcuts
- **State**: `audio::PluginSlot* m_focusedSlot{nullptr}; int m_focusedSlotIndex{-1};`
- **Shortcuts** (guarded by `!io.WantTextInput`):
  - `[` / `]`: Cycle adjacent plugin/preset within category with 10ms crossfade.
  - `Q`: Open Spotlight modal pre-filtered to the focused card's category.
  - `0`: Toggle bypass on the focused card (`slot->setBypassed(!slot->isBypassed())`).

#### Interactive 800-Point Overview Waveform Strip in Backing Track
- **Location**: `src/ui/modals/practice_tools_modal.cpp` (`renderBackingTrackTab()`)
- **Audio Pre-computation**: In `tools::AudioPlayer::loadWavFile()`, pre-compute 800 peak points:
  $$P_i = \max_{k \in [\lfloor i \cdot N/800 \rfloor, \lfloor (i+1) \cdot N/800 \rfloor)} |x[k]|$$
- **UI Rendering**:
  - Waveform strip dimensions: width $W = \text{avail.x}$, height $H = 54\text{px}$.
  - Min/max mirrored bars rendered around horizontal center line $y_{mid} = y_0 + H/2$.
  - Playhead marker line rendered at $x = x_0 + W \cdot \text{positionNormalized()}$.
  - Draggable Loop Start (`[L`) and Loop End (`R]`) handles.
  - Click-to-seek: clicking anywhere on the strip sets playhead position immediately:
    $$\text{seekPos} = \text{std::clamp}((x_{mouse} - x_0) / W, 0.0f, 1.0f)$$

---

### 3.3 R5. Visual Polish, Design Token Strictness & List Virtualization

#### Spotlight `ImGuiListClipper` Virtualization
- **Location**: `src/ui/modals/plugin_browser_modal.cpp` (`render()`)
- **Implementation**:
  ```cpp
  const float rowH = 34.0f;
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(m_filteredItems.size()), rowH);
  while (clipper.Step()) {
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          renderPluginRow(i);
      }
  }
  clipper.End();
  ```
- **Performance Guarantee**: Only ~8 to 10 rows visible in the 300px list container are submitted to ImGui per frame, ensuring a constant 144 FPS with 1,500+ plugins.

#### Elimination of Spotlight Footer Text Collision Across 100%–200% DPI
- **Location**: `src/ui/modals/plugin_browser_modal.cpp` (lines 464–466)
- **Current Issue**:
  `SameLine(modalSize.x - 170.0f)` collides with the navigation hint at high DPI scaling.
- **Resolution**:
  ```cpp
  const char* navHint = "[UP/DOWN] Navigate   [ENTER] Insert   [F] Star   [TAB] Scope   [ESC] Dismiss";
  char countBuf[48];
  std::snprintf(countBuf, sizeof(countBuf), "%zu plugins available", m_filteredItems.size());

  const ImVec2 navSize = ImGui::CalcTextSize(navHint);
  const ImVec2 countSize = ImGui::CalcTextSize(countBuf);
  const float availW = ImGui::GetContentRegionAvail().x;

  ImGui::TextColored(tokens.text.muted.vec4, "%s", navHint);
  if (availW > navSize.x + countSize.x + 16.0f) {
      ImGui::SameLine(availW - countSize.x);
      ImGui::TextColored(tokens.text.muted.vec4, "%s", countBuf);
  }
  ```

#### Spotlight Favorites & Pinned Scope
- **Location**: `src/ui/modals/plugin_browser_modal.h`, `plugin_browser_modal.cpp`
- **Scope Navigation**:
  - `enum class PaletteScope : uint8_t { All = 0, Favorites = 1, Recents = 2 };`
  - `Tab` key cycles scope forward.
  - Scope header tabs displayed above search box: `[ All ]`, `[ ★ Favorites ]`, `[ ↺ Recents ]`.
- **`F` Key Toggle**:
  - Pressing `F` while an item is selected toggles its presence in `AppConfig::favoritePlugins`.
  - Persisted to disk via `AppConfig::save()`.
  - Star icon (`★` / `renderCenteredStarButton`) rendered in the row.
- **Sorting & Scoring**:
  - When query is empty and Scope is `All`: Favorites are pinned to the top, followed by Recents, followed by All plugins alphabetically.
  - When query is active: Favorites receive a `+250` fuzzy score boost.

#### Clean Visual Hierarchy Polish
1. **Eliminate VST3 Double-Badging & Suffix Clutter**:
   - Strip `.vst3` and `.clap` extensions from `desc->name` before display.
   - Do not append format text to titles when the format badge is already shown.
2. **Deepen Modal Dimming Scrim to 0.75 Alpha**:
   - In `src/ui/design_tokens.h`:
     Set `tokens.surfaces.modalOverlay` to `ColorToken(8, 10, 12, 191)` across all 4 themes (where $191/255 = 0.749 \approx 0.75$).
3. **Restyle Active Scene Button**:
   - Dark emerald background token: `ColorToken(16, 75, 45, 230)` (`#104B2D`).
   - Border token: `tokens.borders.focus` (`#FFB326` or `#00F0FF`).
4. **Promote Looper Idle Text Contrast**:
   - Promote idle/ready state text color from `tokens.borders.subtle` to `#9CA3AF` (`ColorToken(156, 163, 175)`), satisfying WCAG AA 4.5:1 contrast against panel backgrounds.
5. **Standardize Top Ribbon FramePadding to 6x4**:
   - Tuner, Metronome, Noise Gate, Master Output child windows in `RackView::renderPracticeRibbon()` standardized to `ImGuiStyleVar_FramePadding` = `ImVec2(6.0f, 4.0f)`.

#### Audio Visualizers: 4-Segment Vertex-Colored PPM Meters
- **Specification**: DIN IEC 60268-10 Type I PPM ballistics:
  - Attack time: $0\text{ms}$ (instant response to peak amplitudes).
  - Decay time: $\tau = 350\text{ms}$ ($20\text{dB}$ decay in $1.7\text{s}$, differential equation: $v(t) = v_{prev} \cdot e^{-dt / 0.35}$).
  - Peak hold: $1.2\text{s}$ hold time before dropping.
- **4 Color Segments**:
  - Segment 1 ($-\infty$ to $-18\text{dB}$): Normal Green (`tokens.signal.meterNormal`).
  - Segment 2 ($-18\text{dB}$ to $-6\text{dB}$): Optimal Cyan/Green (`tokens.signal.active`).
  - Segment 3 ($-6\text{dB}$ to $-0.5\text{dB}$): Warning Amber (`tokens.signal.meterWarning`).
  - Segment 4 ($\ge -0.5\text{dB}$): Clip Red (`tokens.signal.meterClip`).
- **Rendering**: Vertex-colored filled quad segments (`dl->AddRectFilledMultiColor`) or 4 distinct segment rects with a 1.5px horizontal peak hold indicator bar.

#### Audio-Reactive Hermite Cable Glow
- **Location**: `src/ui/rack_view.cpp` (`drawCubicHermiteCable()`)
- **Specification**: When audio signal is present ($signalPeak > 0.02f$), an ambient Bezier glow envelope is stroked underneath the sleeve:
  $$\text{glowWidth} = 6.0f + 8.0f \cdot \sqrt{signalPeak}$$
  $$\text{glowAlpha} = \text{std::clamp}(0.15f + 0.65f \cdot signalPeak, 0.0f, 0.85f)$$
  Color derived from `tokens.cables.pulseGlow`.

---

## 4. Token Strictness & Color Lint Status

### 4.1 Verification Run of `scripts/check_hardcoded_colors.py`
Running the color linter via `py scripts/check_hardcoded_colors.py` yields:
```
------------------------------------------------------------------------
SUCCESS: Clean! 0 hardcoded IM_COL32 / ImVec4 literals found in F:\Projects\Praccy\src\ui.
Design tokens fully enforced across all UI translation units.
```

### 4.2 Strict Enforcement Contract
1. **Zero Raw Literals**: Neither `IM_COL32(...)` nor `ImVec4(...)` may appear in any file under `src/ui/` except `design_tokens.h`.
2. **New Semantic Tokens Required**:
   To satisfy V2.4 Stage 1 requirements without violating `check_hardcoded_colors.py`, the following semantic tokens must be added to `src/ui/design_tokens.h`:
   - `tokens.surfaces.sceneActiveEmerald`: Dark emerald active scene background (`rgba(16, 75, 45, 0.90)`).
   - `tokens.borders.mapModeViolet`: Global Map Mode pulsating border (`rgba(168, 85, 247, 1.0)`).
   - `tokens.text.looperIdle`: `#9CA3AF` contrast-promoted idle text token.
   - `tokens.signal.meterOptimal`: $-18\text{dB}$ to $-6\text{dB}$ meter transition token.
   - Adjust `tokens.surfaces.modalOverlay` alpha to $191$ ($0.75$).

---

## 5. Interface Contracts with Audio Engine & State Management

| Subsystem | Contract / Rule | Mechanism |
|---|---|---|
| **UndoManager <-> GraphEngine** | Restoring an undo/redo snapshot must never cause audio clicks. | `graph.crossfadeToNodes(std::move(newNodes))` using 10ms equal-power crossfade. |
| **UndoManager <-> UI Sliders** | Continuous sweeps must not flood the history stack with 100s of frames. | Coalesce snapshots within a 400ms idle window; commit only on `ImGui::IsItemDeactivatedAfterEdit()`. |
| **A/B Tone Comparison** | Seamless toggling between Snapshot A and B without state loss. | Dual `ScenePreset` memory buffers crossfaded via `crossfadeToNodes`. |
| **Macro Mini-Knobs <-> Plugin Host** | Thread-safe, wait-free parameter updates. | UI invokes `pInst->setParameterValue(paramId, val)` which writes to atomic floats / SPSC automation table. |
| **Card Drag-and-Drop Reorder** | Reordering plugin nodes must execute with 0 dropouts and 0 mutex contention. | Serial node move guarded by `m_graphMutex` on UI thread; audio thread processes block-boundary crossfade. |
| **Global Map Mode Hotkeys** | Typing in text boxes must never fire hotkeys. | All keyboard shortcuts strictly guarded by `!ImGui::GetIO().WantTextInput`. |
| **Backing Track Waveform** | Waveform overview generation must not block the audio thread or UI render thread. | Audio data downsampled into 800 peak points upon file load on worker thread in `AudioPlayer`. |
| **PPM Level Meter** | Accurate DIN IEC 60268-10 ballistics with peak hold. | $0\text{ms}$ attack, $350\text{ms}$ decay computed per-block, peak hold updated per frame. |

---

## 6. Verification and Acceptance Testing

### 6.1 Existing Regression Suite
The existing 10 CTest test targets in `build` currently pass with 100% success rate:
- `test_praccy` (28/28 sub-tests passed)
- `test_asio_driver`
- `test_challenger_m1`
- `test_challenger_m1_2`
- `test_challenger_m2`
- `test_challenger_m2_1`
- `test_challenger_m3_1`
- `test_challenger_m3_2`
- `test_challenger_m4_1`
- `test_challenger_m4_2`

### 6.2 Proposed Test Cases for Stage 1 Harness (`tests/test_praccy_v2_1.cpp`)
1. **`testUndoManagerHistoryLevelsAndCoalescing`**:
   - Push 70 distinct transactions; verify stack is strictly capped at 64 levels.
   - Simulate continuous parameter tweaks within <400ms intervals; verify snapshots are coalesced into 1 record.
   - Advance time >400ms; verify next tweak creates a second transaction.
   - Verify `undo()` and `redo()` restore expected parameter states.
2. **`testSceneBarToneSnapshotAB`**:
   - Store state in Snapshot A, modify graph, store state in Snapshot B.
   - Toggle A/B; verify correct node structures and parameters are loaded.
3. **`testMacroFuzzyRegexHeuristics`**:
   - Provide mocked plugin descriptors with parameter names (`Drive`, `Amp Tone`, `Master Volume`, `Wet Mix`, `Presence`, `Custom 1`).
   - Assert heuristic maps Macro 1 -> `Drive`, Macro 2 -> `Amp Tone`, Macro 3 -> `Master Volume`, Macro 4 -> `Wet Mix`.
4. **`testHotkeysGuardedByWantTextInput`**:
   - Assert that when `io.WantTextInput == true`, hotkeys (`Ctrl+Z`, `Ctrl+M`, `1-8`, `[`, `]`, `Q`, `0`) return without action.
5. **`testSpotlightVirtualizationAndDpiSafety`**:
   - Populate mock library with 2,000 plugin descriptors.
   - Verify `ImGuiListClipper` displays only visible subset without rendering all 2,000 items.
   - Assert footer string lengths at 100%, 125%, 150%, 200% DPI do not collide.
6. **`testPpmBallisticsMath`**:
   - Verify DIN IEC 60268-10 decay formula: initial peak decaying by $20\text{dB}$ over $1.7\text{s}$ ($\tau = 350\text{ms}$).
   - Verify $1.2\text{s}$ peak hold latch duration.
7. **`testHardcodedColorLinterPurity`**:
   - Execute `py scripts/check_hardcoded_colors.py`; assert exit code 0.
