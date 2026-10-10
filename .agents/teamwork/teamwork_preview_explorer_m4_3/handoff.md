# Implementation Blueprint: Milestone 4 (Features 24, 25, 26, 27, 28)

**Agent**: `teamwork_preview_explorer_m4_3`  
**Milestone**: Milestone 4 (Modular UI Refactoring & Practice Suite Overhaul - Requirement R4)  
**Deliverable**: Concrete Implementation Blueprint for Features 24, 25, 26, 27, and Unit Tests (Feature 28)  
**Date**: 2026-10-07  

---

## 1. Observation

### 1.1 Existing Implementations & Codebase State

#### Feature 24: Quick Looper Rendering
- **Source File**: `src/ui/rack_view.cpp:3289-3300`
- **Observed Code**:
  ```cpp
  // Progress Bar
  float progress = m_looper.playheadNormalized();
  char progressOverlay[64];
  if (loopState == tools::LooperState::Recording) {
      std::snprintf(progressOverlay, sizeof(progressOverlay), "Recording: %.1f s", m_looper.loopLengthSeconds());
  } else if (m_looper.loopLengthSeconds() > 0.05) {
      std::snprintf(progressOverlay, sizeof(progressOverlay), "%.1f / %.1f s", progress * m_looper.loopLengthSeconds(), m_looper.loopLengthSeconds());
  } else {
      std::snprintf(progressOverlay, sizeof(progressOverlay), "Ready");
  }
  ImGui::ProgressBar(progress, ImVec2(ImGui::GetContentRegionAvail().x - 10.0f, 22.0f), progressOverlay);
  ```
- **Finding**: Currently, the Quick Looper renders a linear horizontal `ImGui::ProgressBar`. It lacks circular progress visualization, state-dependent color encoding, and playhead indicator.
- **Looper State API**: `src/tools/quick_looper.h:10-16` defines `LooperState`: `Empty`, `Recording`, `Playing`, `Overdubbing`, `Stopped`.
- **Looper Math API**: `QuickLooper::playheadNormalized()` (`src/tools/quick_looper.cpp:74-78`) calculates `head / loopLength`.

#### Feature 25: Win32 Drag-and-Drop & Window Setup
- **Source File**: `src/main.cpp:190-201`, `src/main.cpp:590-624`
- **Observed Code in `WinMain`**:
  ```cpp
  HWND hwnd = CreateWindowW(
      wc.lpszClassName,
      L"Praccy - ASIO VST3/CLAP Practice Host",
      WS_OVERLAPPEDWINDOW,
      100, 100, 1280, 720,
      nullptr, nullptr, wc.hInstance, nullptr
  );
  ```
  `DragAcceptFiles(hwnd, TRUE)` is currently not called anywhere in `src/main.cpp`.
- **Observed Code in `WndProc`**:
  `WndProc` (`src/main.cpp:590-624`) handles `WM_ERASEBKGND`, `WM_SIZE`, `WM_SYSCOMMAND`, `WM_DPICHANGED`, and `WM_DESTROY`. It does not handle `WM_DROPFILES`.
- **CMake & Dependencies**: `CMakeLists.txt:95` explicitly links `shell32`. `src/main.cpp:2` includes `<shellapi.h>`. All Win32 Shell drag-and-drop APIs (`DragAcceptFiles`, `DragQueryFileW`, `DragFinish`) are fully accessible without adding dependencies.
- **Audio Loading**: `src/tools/audio_player.h:17` and `src/tools/audio_player.cpp:35-146` define `bool AudioPlayer::loadWavFile(const std::string& filePath)`, supporting 16-bit, 24-bit, 32-bit PCM, and IEEE 32-bit Float with resampling.

#### Feature 26: Scene Recall & Audio Crossfading
- **Source File**: `src/state/scene_manager.cpp:248-288`
- **Observed Code**:
  ```cpp
  bool SceneManager::applyScene(int sceneIndex, audio::GraphEngine& graph) {
      if (sceneIndex < 0 || sceneIndex >= static_cast<int>(m_scenes.size())) return false;
      plugins::PluginWindowManager::instance().closeAllWindows();
      const auto& scene = m_scenes[sceneIndex];
      m_activeSceneIndex = sceneIndex;
      graph.clearNodes();
      // ... nodes added synchronously ...
      graph.prepare(graph.sampleRate(), graph.maxBlockSize());
      m_statusMessage = "Loaded preset: " + scene.name;
      return true;
  }
  ```
- **Observed Graph Engine Mutex**: `src/audio/graph_engine.cpp:672` in `GraphEngine::process()`:
  `std::unique_lock<std::mutex> lock(m_graphMutex, std::try_to_lock);`
  When `graph.clearNodes()` executes on the UI thread under `m_graphMutex`, `process()` bypasses node processing for that cycle and immediately switches abruptly to newly added nodes upon unlock, causing step discontinuities and audible click/pop transients.
- **EqualPowerRamp Primitive**: `src/audio/dsp_utils.h:65-119` already implements `EqualPowerRamp` with `getNextGains(float& gainOld, float& gainNew)`:
  ```cpp
  const float progress = static_cast<float>(m_currentSample) / static_cast<float>(m_totalSamples);
  const float angle = progress * DspUtils::HALF_PI;
  if (m_targetState) {
      gainOld = std::cos(angle);
      gainNew = std::sin(angle);
  }
  ```
  Where $\text{HALF\_PI} = \frac{\pi}{2}$. For activating transition: $g_{\text{out}}(t) = \cos(\frac{\pi}{2}t)$ and $g_{\text{in}}(t) = \sin(\frac{\pi}{2}t)$.
  Energy conservation: $\cos^2(\frac{\pi}{2}t) + \sin^2(\frac{\pi}{2}t) \equiv 1.0$.

#### Feature 27: Preset Feedback & Layout Shift
- **Source File**: `src/ui/rack_view.cpp:1093-1127`
- **Observed Code**:
  ```cpp
  if (m_sceneFeedbackTimer > 0.0f) {
      m_sceneFeedbackTimer -= ImGui::GetIO().DeltaTime;
      ImGui::SameLine(0, 12);
      // ... renders pill ...
      ImGui::Dummy(ImVec2(pillW, frameH));
  }
  ```
  And in `src/ui/rack_view.cpp:529-539`:
  ```cpp
  for (int k = 0; k < 8 && k < static_cast<int>(m_scenes.numScenes()); ++k) {
      if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + k), false)) {
          m_scenes.applyScene(k, m_graph);
          const auto* sc = m_scenes.getScene(k);
          m_sceneFeedbackMsg = "Loaded " + (sc ? sc->name : ("Scene " + std::to_string(k + 1)));
          m_sceneFeedbackTimer = 2.5f;
      }
  }
  ```
- **Finding**: Feedback is rendered inline inside `renderSceneBar()` using `ImGui::SameLine` and `ImGui::Dummy`. Inserting and removing this element whenever a preset is recalled causes noticeable horizontal layout shift for adjacent scene controls.

#### Feature 28: Existing Unit Tests
- **Source File**: `tests/test_praccy.cpp` (1433 lines)
- **Observed Code**: Currently contains 24 tests (`testAudioBuffers` through `testViewportCenteringCalculations`).
- `testQuickLooper` (`tests/test_praccy.cpp:346-392`) tests state cycling but does not test progress math or circular trigonometry.
- Does not test `EqualPowerRamp` energy conservation identity, WAV drop path extension validation, or HUD alpha decay.

---

## 2. Logic Chain

1. **Feature 24 (Quick Looper Circular Ring)**:
   - Dear ImGui provides vector drawing APIs `ImDrawList::PathArcTo` and `ImDrawList::PathStroke` that allow drawing arbitrary arc segments.
   - Playback progress $p = \frac{\text{currentSample}}{\text{loopLength}} \in [0.0, 1.0]$. The arc sweep angle is $\theta = 2\pi \cdot p$.
   - Starting from 12 o'clock ($-\frac{\pi}{2}$ in ImGui screen coordinates, where positive Y is down and clockwise rotation increases angle), the arc spans from $-\frac{\pi}{2}$ to $-\frac{\pi}{2} + \theta$.
   - Drawing a complete background ring ($0$ to $2\pi$) with `tokens.borders.subtle` establishes the loop track.
   - Overlaying the active arc with semantic design tokens establishes instant visual clarity:
     - `Recording`: Red (`tokens.signal.faulted` or `tokens.signal.recording`)
     - `Overdubbing`: Amber (`tokens.signal.accent` / `tokens.signal.overdubbing`)
     - `Playing`: Green (`tokens.signal.active` / `tokens.signal.playing`)
     - `Stopped`: Muted Slate (`tokens.text.muted` / `tokens.signal.stopped`)
   - Centering text inside the circle with `ImGui::CalcTextSize` creates an integrated dial widget without extra layout sprawl.

2. **Feature 25 (WAV Drag-and-Drop `WM_DROPFILES`)**:
   - Calling Win32 `DragAcceptFiles(hwnd, TRUE)` registers Praccy's top-level window as an OLE/Shell drop target.
   - When files are dragged from Windows Explorer onto Praccy, Windows dispatches `WM_DROPFILES` with `wParam = (WPARAM)hDrop`.
   - `DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0)` retrieves the dropped file count.
   - `DragQueryFileW(hDrop, 0, filePathW, MAX_PATH)` retrieves the wide-character path of the first dropped item safely without heap allocation.
   - A case-insensitive check ensures the file extension is `.wav`.
   - The path is converted to a UTF-8 string and dispatched to `AudioPlayer::loadWavFile(path)` (and `QuickLooper::loadWavFile(path)` if looper loading is enabled).
   - Calling `DragFinish(hDrop)` releases Win32 kernel drop resources cleanly.

3. **Feature 26 (Click-Free `EqualPowerRamp` Preset Switching)**:
   - Audible clicks during preset changes are caused by step discontinuities in signal waveforms when plugin parameters or graph topologies switch instantaneously.
   - An equal-power crossfade between the outgoing audio stream $y_{\text{out}}(t)$ and incoming audio stream $y_{\text{in}}(t)$ maintains total acoustic power:
     $P_{\text{total}}(t) = g_{\text{out}}^2(t) + g_{\text{in}}^2(t) = \cos^2\left(\frac{\pi}{2}t\right) + \sin^2\left(\frac{\pi}{2}t\right) = 1.0$
   - Duration: $T = 10\,\text{ms}$, translating to $N = \lfloor 0.010 \times \text{sampleRate} \rfloor$ samples (e.g., 480 samples at 48 kHz).
   - In `GraphEngine`:
     - Maintain an active chain `m_nodes` and a retiring chain `m_retiringNodes`.
     - When `applyScene` is invoked: build and prepare the new chain off the audio thread, lock `m_graphMutex`, move `m_nodes` to `m_retiringNodes`, assign the new chain to `m_nodes`, and initiate `m_sceneCrossfadeRamp.startTransition(true)` for $N$ samples.
     - In `GraphEngine::process()`: while `m_sceneCrossfadeRamp.isTransitioning()`, process the input buffer through both `m_retiringNodes` and `m_nodes`, blending sample-by-sample using $g_{\text{out}}$ and $g_{\text{in}}$.
     - Once transition finishes, deallocation of `m_retiringNodes` is deferred to `processReclamation()` on the UI thread, ensuring zero audio-thread memory frees and maintaining real-time safety.

4. **Feature 27 (Floating HUD Toast Notification)**:
   - Rendering status pills inline inside layout containers alters item coordinates and shifts buttons.
   - A floating overlay rendered in viewport screen space (center-top: $X = \text{viewport.Center.x}$, $Y = \text{viewport.Pos.y} + 44\,\text{px}$) with `ImGuiWindowFlags_NoInputs` guarantees zero layout shift and non-blocking mouse interactions.
   - Toast duration: $1.8\,\text{s}$.
   - Linear alpha decay: $\alpha(t) = \text{clamp}(t / 1.8, 0.0, 1.0)$ provides smooth, predictable fade-out.
   - Background uses `tokens.surfaces.cardBg` with alpha modulation; text uses `tokens.text.primary`.

5. **Feature 28 (Automated Regression Unit Tests)**:
   - Pure functions (`calculateEqualPowerCrossfade`, `computeHudToastAlpha`, `isValidWavFile`) can be tested deterministically in headless environments without GUI/audio hardware dependencies.
   - Energy conservation test asserts $|g_{\text{out}}^2 + g_{\text{in}}^2 - 1.0| < 10^{-5}$ across all discrete sample steps.
   - Angle test asserts $\theta \in [0, 2\pi]$ for all valid playhead positions and verifies zero-division protection when `loopLength == 0`.
   - File extension test exercises case variations (`.wav`, `.WAV`, `.Wav`) and rejects non-WAV extensions.
   - Alpha test verifies clamping, monotonicity, and boundary values at $t = 1.8\,\text{s}$, $t = 0.9\,\text{s}$, and $t = 0.0\,\text{s}$.

---

## 3. Caveats

1. **Modal Refactoring Integration (Feature 20 & 24)**:
   - Explorer 1 is decoupling modals from `src/ui/rack_view.cpp` into `src/ui/modals/practice_tools_modal.cpp`.
   - The circular progress ring implementation blueprint below is designed as a modular function `renderLooperCircularProgressRing(...)` that can be placed in either `practice_tools_modal.cpp` or `rack_view.cpp`.
2. **Audio Thread Processing Load during Crossfade (Feature 26)**:
   - During the 10ms crossfade window (e.g. 480 samples at 48kHz, spanning ~1 to 2 buffers of 256 samples), both the outgoing and incoming plugin chains execute concurrently.
   - If both chains contain heavy DSP nodes, DSP load will temporarily be the sum of both chains for ~10ms before returning to normal.
   - Pre-allocating scratch buffers (`m_retiringBuffer`, `m_activeBuffer`) during `GraphEngine::prepare()` ensures zero heap allocation occurs during this window.
3. **Multi-File Drag-and-Drop Behavior (Feature 25)**:
   - If a user drops multiple files simultaneously, the blueprint loads the first valid `.wav` file encountered.
   - Non-WAV dropped files are silently ignored without error modals or crashes.
4. **No Code Modification Constraint**:
   - This explorer operates in read-only mode. All proposed changes below are structured as drop-in blueprints for workers.

---

## 4. Conclusion & Concrete Implementation Blueprint

### 4.1 Feature 24: Quick Looper Circular Progress Ring

#### Location
`src/ui/modals/practice_tools_modal.cpp` (or `src/ui/rack_view.cpp:3289` if integrated before extraction).

#### Design Specification
- **Center & Dimensions**:
  - Center: calculated from `ImGui::GetCursorScreenPos()` + local offset.
  - Outer radius $R = 52.0\,\text{px}$.
  - Ring stroke thickness: $6.0\,\text{px}$.
  - Segment count: 64 segments for circular fidelity.
  - Reserved layout box: `ImGui::Dummy(ImVec2(120.0f, 120.0f))` ensuring zero layout jump.
- **Trigonometry**:
  - ImGui angle convention: $0\,\text{rad}$ is 3 o'clock (positive X), $\frac{\pi}{2}\,\text{rad}$ is 6 o'clock (positive Y).
  - Top position (12 o'clock): $\alpha_{\text{start}} = -\frac{\pi}{2} \approx -1.5707963\,\text{rad}$.
  - Playhead normalized: $p = \text{std::clamp}(\text{looper.playheadNormalized}(), 0.0\text{f}, 1.0\text{f})$.
  - End angle: $\alpha_{\text{end}} = \alpha_{\text{start}} + 2\pi \cdot p$.
- **Color Mapping**:
  - `Recording`: `tokens.signal.faulted` (Red `#EB4034`)
  - `Overdubbing`: `tokens.signal.accent` / `tokens.signal.overdubbing` (Amber `#FFA023`)
  - `Playing`: `tokens.signal.active` (Green `#34C759`)
  - `Stopped`: `tokens.text.muted` (Muted Slate `#747C8A`)
  - `Empty`: Background track only (`tokens.borders.subtle`).
- **Playhead Needle Indicator**:
  - A filled dot rendered at $(\text{center}.x + R \cos(\alpha_{\text{end}}), \text{center}.y + R \sin(\alpha_{\text{end}}))$ with radius $4.5\,\text{px}$ and white border.
- **Inner Typography Readout**:
  - Top text: state pill label ("REC", "PLAY", "DUB", "STOP", "READY") in bold UI font.
  - Bottom text: elapsed / total time readout in monospace font (`g_fontMono`).

#### Implementation Blueprint (C++ Code)
```cpp
void renderLooperCircularProgressRing(tools::QuickLooper& looper) {
    const auto& tokens = themeTokens();
    const tools::LooperState state = looper.state();
    const float progress = std::clamp(looper.playheadNormalized(), 0.0f, 1.0f);
    const double loopSec = looper.loopLengthSeconds();

    const float widgetSize = 120.0f;
    const float radius = 50.0f;
    const float thickness = 6.0f;

    const ImVec2 screenPos = ImGui::GetCursorScreenPos();
    const ImVec2 center(screenPos.x + widgetSize * 0.5f, screenPos.y + widgetSize * 0.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // 1. Determine State Colors & Strings
    ImU32 arcColor = tokens.text.muted.u32;
    const char* stateText = "READY";
    switch (state) {
        case tools::LooperState::Recording:
            arcColor = tokens.signal.faulted.u32;
            stateText = "REC";
            break;
        case tools::LooperState::Overdubbing:
            arcColor = tokens.signal.overdubbing.u32; // Amber
            stateText = "DUB";
            break;
        case tools::LooperState::Playing:
            arcColor = tokens.signal.active.u32; // Green
            stateText = "PLAY";
            break;
        case tools::LooperState::Stopped:
            arcColor = tokens.text.muted.u32;
            stateText = "STOP";
            break;
        case tools::LooperState::Empty:
        default:
            arcColor = tokens.borders.subtle.u32;
            stateText = "READY";
            break;
    }

    // 2. Background Track Ring
    dl->PathArcTo(center, radius, 0.0f, 2.0f * IM_PI, 64);
    dl->PathStroke(tokens.borders.subtle.u32, 0, thickness);

    // 3. Active Progress Arc
    const float startAngle = -IM_PI * 0.5f; // 12 o'clock
    float endAngle = startAngle;

    if (state == tools::LooperState::Recording) {
        // While recording, pulse or show continuous progress against max seconds (60s)
        float recProgress = static_cast<float>(std::fmod(ImGui::GetTime() * 0.8, 1.0));
        endAngle = startAngle + (2.0f * IM_PI * recProgress);
    } else if (progress > 0.001f) {
        endAngle = startAngle + (2.0f * IM_PI * progress);
    }

    if (endAngle > startAngle) {
        dl->PathArcTo(center, radius, startAngle, endAngle, 64);
        dl->PathStroke(arcColor, 0, thickness);

        // Playhead Indicator Dot
        const float dotX = center.x + radius * std::cos(endAngle);
        const float dotY = center.y + radius * std::sin(endAngle);
        dl->AddCircleFilled(ImVec2(dotX, dotY), thickness * 0.75f, arcColor, 16);
        dl->AddCircle(ImVec2(dotX, dotY), thickness * 0.75f, tokens.text.primary.u32, 16, 1.5f);
    }

    // 4. Center Typography
    const ImVec2 stateSize = ImGui::CalcTextSize(stateText);
    dl->AddText(ImVec2(center.x - stateSize.x * 0.5f, center.y - stateSize.y - 2.0f), arcColor, stateText);

    char timeBuf[32];
    if (state == tools::LooperState::Recording) {
        std::snprintf(timeBuf, sizeof(timeBuf), "REC");
    } else if (loopSec > 0.05) {
        std::snprintf(timeBuf, sizeof(timeBuf), "%.1fs", progress * loopSec);
    } else {
        std::snprintf(timeBuf, sizeof(timeBuf), "--.-s");
    }

    if (g_fontMono) ImGui::PushFont(g_fontMono);
    const ImVec2 timeSize = ImGui::CalcTextSize(timeBuf);
    dl->AddText(ImVec2(center.x - timeSize.x * 0.5f, center.y + 4.0f), tokens.text.secondary.u32, timeBuf);
    if (g_fontMono) ImGui::PopFont();

    // 5. Reserve Layout Space
    ImGui::Dummy(ImVec2(widgetSize, widgetSize));
}
```

---

### 4.2 Feature 25: WAV File Drag-and-Drop (`WM_DROPFILES`)

#### Location
`src/main.cpp` and `src/tools/quick_looper.h/.cpp`.

#### Design Specification
1. **Window Registration**:
   In `src/main.cpp` inside `WinMain`, immediately following window creation:
   ```cpp
   DragAcceptFiles(hwnd, TRUE);
   ```
2. **Context Registration**:
   Define a drop context accessible to `WndProc`:
   ```cpp
   struct DropTargetContext {
       praccy::tools::AudioPlayer* player{nullptr};
       praccy::tools::QuickLooper* looper{nullptr};
   };
   static DropTargetContext s_dropContext;
   ```
   In `WinMain` after constructing `player` and `looper`:
   ```cpp
   s_dropContext.player = &player;
   s_dropContext.looper = &looper;
   ```
3. **Extension Validator Helper**:
   Author pure, testable validation helper:
   ```cpp
   inline bool isValidWavFile(const std::filesystem::path& path) noexcept {
       if (!path.has_extension()) return false;
       auto ext = path.extension().wstring();
       for (auto& c : ext) c = static_cast<wchar_t>(::towlower(c));
       return ext == L".wav";
   }
   ```
4. **`WndProc` Message Handling**:
   In `WndProc`:
   ```cpp
   case WM_DROPFILES: {
       HDROP hDrop = reinterpret_cast<HDROP>(wParam);
       UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
       if (fileCount > 0) {
           wchar_t filePathW[MAX_PATH];
           UINT len = DragQueryFileW(hDrop, 0, filePathW, MAX_PATH);
           if (len > 0) {
               std::filesystem::path droppedPath(filePathW);
               if (isValidWavFile(droppedPath)) {
                   std::string pathUtf8 = droppedPath.u8string();
                   if (s_dropContext.player) {
                       s_dropContext.player->loadWavFile(pathUtf8);
                   }
                   if (s_dropContext.looper) {
                       s_dropContext.looper->loadWavFile(pathUtf8);
                   }
               }
           }
       }
       DragFinish(hDrop);
       return 0;
   }
   ```
5. **`QuickLooper::loadWavFile` Support**:
   Add to `src/tools/quick_looper.h`:
   ```cpp
   bool loadWavFile(const std::string& filePath);
   ```
   In `src/tools/quick_looper.cpp`:
   Read WAV frames using RIFF parser, populate `m_loopBufferL` / `m_loopBufferR` up to `m_maxFrames`, set `m_loopLength` to frames count, set `m_head = 0`, and set `m_state = LooperState::Stopped`.

---

### 4.3 Feature 26: 10ms Click-Free `EqualPowerRamp` Preset Switching

#### Location
`src/audio/graph_engine.h`, `src/audio/graph_engine.cpp`, and `src/state/scene_manager.cpp`.

#### Mathematical Formulation
- **Crossfade Duration**:
  $N_{\text{ramp}} = \text{static\_cast<uint32\_t>}(m\_sampleRate \times 0.010)$ samples (10ms).
- **Ramp Gains**:
  For normalized step $t = \frac{s}{N_{\text{ramp}}} \in [0, 1]$:
  $$g_{\text{out}}(t) = \cos\left(\frac{\pi}{2}t\right), \quad g_{\text{in}}(t) = \sin\left(\frac{\pi}{2}t\right)$$
- **Constant Energy Identity**:
  $$g_{\text{out}}^2(t) + g_{\text{in}}^2(t) = \cos^2\left(\frac{\pi}{2}t\right) + \sin^2\left(\frac{\pi}{2}t\right) \equiv 1.0 \quad \forall t \in [0, 1]$$
- **Signal Blending**:
  For each sample frame $s \in [0, \text{numSamples}-1]$ across channel $ch$:
  $$y[ch][s] = g_{\text{out}}(s) \cdot y_{\text{retiring}}[ch][s] + g_{\text{in}}(s) \cdot y_{\text{active}}[ch][s]$$

#### Engine Architecture Blueprint
1. **Header Updates (`src/audio/graph_engine.h`)**:
   Add to `GraphEngine`:
   ```cpp
   public:
       void crossfadeToNodes(std::vector<std::unique_ptr<AudioNode>> newNodes);
       [[nodiscard]] bool isSceneCrossfading() const noexcept {
           return m_sceneCrossfadeRamp.isTransitioning();
       }

   private:
       EqualPowerRamp m_sceneCrossfadeRamp;
       std::vector<std::unique_ptr<AudioNode>> m_retiringNodes;
       OwnedAudioBuffer m_retiringBuffer;
       OwnedAudioBuffer m_activeBuffer;
   ```
2. **Buffer Pre-allocation in `GraphEngine::prepare`**:
   In `src/audio/graph_engine.cpp:prepare`:
   ```cpp
   m_retiringBuffer.resize(2, maxBlockSize);
   m_activeBuffer.resize(2, maxBlockSize);
   m_sceneCrossfadeRamp.reset(static_cast<uint32_t>(sampleRate * 0.010));
   ```
3. **Crossfade Trigger in `GraphEngine::crossfadeToNodes`**:
   ```cpp
   void GraphEngine::crossfadeToNodes(std::vector<std::unique_ptr<AudioNode>> newNodes) {
       std::lock_guard<std::mutex> lock(m_graphMutex);
       m_retiringNodes = std::move(m_nodes);
       m_nodes = std::move(newNodes);
       const uint32_t rampSamples = static_cast<uint32_t>(m_sampleRate * 0.010);
       m_sceneCrossfadeRamp.reset(std::max(1u, rampSamples));
       m_sceneCrossfadeRamp.startTransition(true);
   }
   ```
4. **Real-Time Safe Audio Blending in `GraphEngine::process`**:
   Replace the node execution loop in `src/audio/graph_engine.cpp:670-686` with:
   ```cpp
   {
       std::unique_lock<std::mutex> lock(m_graphMutex, std::try_to_lock);
       if (lock.owns_lock()) {
           if (m_sceneCrossfadeRamp.isTransitioning()) {
               // 1. Process Retiring (Outgoing) Chain
               auto retView = m_retiringBuffer.view(numSamples);
               retView.copyFrom(mainView);
               for (auto& node : m_retiringNodes) {
                   auto stepScratch = m_scratchBuffer.view(numSamples);
                   AudioProcessContext ctx{
                       .input = retView,
                       .output = stepScratch,
                       .sampleRate = m_sampleRate,
                       .numSamples = numSamples
                   };
                   node->process(ctx);
                   retView.copyFrom(stepScratch);
               }

               // 2. Process Active (Incoming) Chain
               auto actView = m_activeBuffer.view(numSamples);
               actView.copyFrom(mainView);
               for (auto& node : m_nodes) {
                   auto stepScratch = m_scratchBuffer.view(numSamples);
                   AudioProcessContext ctx{
                       .input = actView,
                       .output = stepScratch,
                       .sampleRate = m_sampleRate,
                       .numSamples = numSamples
                   };
                   node->process(ctx);
                   actView.copyFrom(stepScratch);
               }

               // 3. Sample-by-Sample EqualPower Crossfade
               const uint32_t numCh = mainView.numChannels();
               for (uint32_t s = 0; s < numSamples; ++s) {
                   float gainOut = 0.0f, gainIn = 0.0f;
                   m_sceneCrossfadeRamp.getNextGains(gainOut, gainIn);
                   for (uint32_t ch = 0; ch < numCh; ++ch) {
                       float* out = mainView.channel(ch);
                       const float* oldS = retView.channel(ch);
                       const float* newS = actView.channel(ch);
                       out[s] = (oldS[s] * gainOut) + (newS[s] * gainIn);
                   }
               }
           } else {
               // Standard Single-Chain Execution Path
               for (auto& node : m_nodes) {
                   auto scratchView = m_scratchBuffer.view(numSamples);
                   AudioProcessContext ctx{
                       .input = mainView,
                       .output = scratchView,
                       .sampleRate = m_sampleRate,
                       .numSamples = numSamples
                   };
                   node->process(ctx);
                   mainView.copyFrom(scratchView);
               }
           }
       }
   }
   ```
5. **UI-Thread Node Reclamation**:
   In `GraphEngine::processReclamation()`:
   ```cpp
   void GraphEngine::processReclamation() noexcept {
       std::unique_lock<std::mutex> lock(m_graphMutex, std::try_to_lock);
       if (!lock.owns_lock()) return;
       // Deallocate retiring nodes ONLY off audio thread once crossfade completes
       if (!m_sceneCrossfadeRamp.isTransitioning() && !m_retiringNodes.empty()) {
           m_retiringNodes.clear();
       }
       for (auto& node : m_nodes) {
           if (auto* splitBlock = dynamic_cast<ParallelSplitMergeBlock*>(node.get())) {
               splitBlock->collectReclaimedSlots();
           }
       }
   }
   ```
6. **`SceneManager::applyScene` Integration**:
   In `src/state/scene_manager.cpp:248-288`:
   Assemble the new node list on the UI thread without destroying existing nodes, then invoke `graph.crossfadeToNodes(std::move(newNodes))`.

---

### 4.4 Feature 27: Floating HUD Toast Notification

#### Location
`src/ui/rack_view.h` and `src/ui/rack_view.cpp`.

#### Design Specification
- **Positioning**: Center-top of the main viewport.
  $X = \text{viewport.Pos.x} + (\text{viewport.Size.x} - \text{boxWidth}) \times 0.5f$  
  $Y = \text{viewport.Pos.y} + 44.0\,\text{px}$ (comfortably clear of the top menu bar).
- **Appearance**:
  - Background: `tokens.surfaces.cardBg` with alpha modulation and $8.0\,\text{px}$ rounding.
  - Border: `tokens.borders.focus` / `tokens.borders.cardGlowActive` with $1.5\,\text{px}$ stroke.
  - Accent LED: Green indicator dot (`tokens.signal.active`) with pulsing glow.
  - Text: `tokens.text.primary` in bold UI font, message format `"PRESET [N] RECALLED"`.
- **Timing & Decay**:
  - Total duration: $1.8\,\text{s}$.
  - Pure calculation function:
    ```cpp
    inline float computeHudToastAlpha(float remainingTime, float totalDuration = 1.8f) noexcept {
        if (remainingTime <= 0.0f || totalDuration <= 0.0f) return 0.0f;
        if (remainingTime >= totalDuration) return 1.0f;
        return std::clamp(remainingTime / totalDuration, 0.0f, 1.0f);
    }
    ```
- **Interaction**:
  - Window flags: `ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove`.
  - Non-blocking: mouse events pass straight through to rack nodes beneath.
- **Zero Layout Shift**:
  - The previous inline status indicator in `renderSceneBar()` is removed.
  - The floating HUD is rendered at the end of `RackView::render()` in independent screen overlay space.

#### Implementation Blueprint (C++ Code)
In `src/ui/rack_view.h`:
```cpp
public:
    void triggerHudToast(std::string message);

private:
    void renderFloatingHudToast();

    std::string m_hudToastText;
    float m_hudToastTimer{0.0f};
    static constexpr float kHudToastDuration = 1.8f;
```

In `src/ui/rack_view.cpp`:
```cpp
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

        const ImU32 bgCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
            tokens.surfaces.cardBg.r,
            tokens.surfaces.cardBg.g,
            tokens.surfaces.cardBg.b,
            tokens.surfaces.cardBg.a * alpha * 0.95f
        ));
        const ImU32 borderCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
            tokens.borders.focus.r,
            tokens.borders.focus.g,
            tokens.borders.focus.b,
            tokens.borders.focus.a * alpha * 0.90f
        ));
        const ImU32 textCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
            tokens.text.primary.r,
            tokens.text.primary.g,
            tokens.text.primary.b,
            alpha
        ));
        const ImU32 ledCol = ImGui::ColorConvertFloat4ToU32(ImVec4(
            tokens.signal.active.r,
            tokens.signal.active.g,
            tokens.signal.active.b,
            alpha
        ));

        // Background card
        dl->AddRectFilled(ImVec2(posX, posY), ImVec2(posX + boxW, posY + boxH), bgCol, 8.0f);
        dl->AddRect(ImVec2(posX, posY), ImVec2(posX + boxW, posY + boxH), borderCol, 8.0f, 0, 1.5f);

        // LED dot
        const ImVec2 dotCenter(posX + 16.0f, posY + boxH * 0.5f);
        dl->AddCircleFilled(dotCenter, 4.0f, ledCol, 16);

        // Readout text
        const ImVec2 textPos(posX + 28.0f, posY + (boxH - textSize.y) * 0.5f);
        dl->AddText(textPos, textCol, m_hudToastText.c_str());
    }
    ImGui::End();
}
```

Triggering in hotkey loop (`src/ui/rack_view.cpp:529-539`):
```cpp
if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_1 + k), false)) {
    m_scenes.applyScene(k, m_graph);
    triggerHudToast("PRESET " + std::to_string(k + 1) + " RECALLED");
}
```

---

### 4.5 Feature 28 & Unit Tests (`tests/test_praccy.cpp`)

Four targeted regression suites designed to test mathematical identities, validation logic, and transition dynamics:

```cpp
// -------------------------------------------------------------
// Test 25: EqualPowerRamp Energy Conservation
// -------------------------------------------------------------
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

    std::cout << "PASSED\n";
}

// -------------------------------------------------------------
// Test 26: Quick Looper Circular Progress Math & State Transitions
// -------------------------------------------------------------
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
    looper.process(inView, outView);

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

// -------------------------------------------------------------
// Test 27: Drag-and-Drop Extension Validation
// -------------------------------------------------------------
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

// -------------------------------------------------------------
// Test 28: Floating HUD Alpha Decay Computation
// -------------------------------------------------------------
void testFloatingHudAlphaDecayComputation() {
    std::cout << "[TEST] Floating HUD Alpha Decay Computation... ";

    auto computeAlpha = [](float remainingTime, float totalDuration = 1.8f) -> float {
        if (remainingTime <= 0.0f || totalDuration <= 0.0f) return 0.0f;
        if (remainingTime >= totalDuration) return 1.0f;
        return std::clamp(remainingTime / totalDuration, 0.0f, 1.0f);
    };

    const float duration = 1.8f;

    // 1. Exact boundary values
    assert(computeAlpha(1.8f, duration) == 1.0f);
    assert(computeAlpha(2.5f, duration) == 1.0f); // Upper clamp
    assert(computeAlpha(0.0f, duration) == 0.0f);
    assert(computeAlpha(-0.5f, duration) == 0.0f); // Lower clamp

    // 2. Midpoint value
    assert(std::abs(computeAlpha(0.9f, duration) - 0.5f) < 1e-4f);

    // 3. Strict monotonic decrease over time
    float prevAlpha = 1.0f;
    for (float t = 1.8f; t >= 0.0f; t -= 0.05f) {
        float a = computeAlpha(t, duration);
        assert(a <= prevAlpha + 1e-6f);
        assert(a >= 0.0f && a <= 1.0f);
        prevAlpha = a;
    }

    std::cout << "PASSED\n";
}
```

Registration in `tests/test_praccy.cpp:main()`:
```cpp
testEqualPowerRampEnergyConservation();
testQuickLooperCircularProgressMath();
testWavDragAndDropExtensionValidation();
testFloatingHudAlphaDecayComputation();
```
Increases passing unit test count from 24/24 to 28/28.

---

## 5. Verification Method

### 5.1 Verification Commands
1. **Compilation Check**:
   ```pwsh
   cmake -B build -G "MinGW Makefiles"
   cmake --build build --target test_praccy -j4
   ```
2. **Unit Test Execution**:
   ```pwsh
   ./build/test_praccy.exe
   ```
   **Expected Output**:
   `[TEST] EqualPowerRamp Energy Conservation Identity... PASSED`  
   `[TEST] QuickLooper Circular Progress & Trigonometry... PASSED`  
   `[TEST] WAV Drag-and-Drop Extension Validation... PASSED`  
   `[TEST] Floating HUD Alpha Decay Computation... PASSED`  
   `ALL TESTS PASSED SUCCESSFULLY! (28/28)`
3. **Static Analysis & Hardcoded Color Audit**:
   ```pwsh
   python scripts/check_hardcoded_colors.py
   ```
   **Expected Output**: Zero hardcoded `IM_COL32` or raw `ImVec4` color literals.

### 5.2 Files to Inspect
- `src/tools/quick_looper.h` & `src/tools/quick_looper.cpp`: `loadWavFile` interface and implementation.
- `src/audio/graph_engine.h` & `src/audio/graph_engine.cpp`: `crossfadeToNodes`, `m_sceneCrossfadeRamp`, dual-buffer blend loop, and UI-thread reclamation in `processReclamation`.
- `src/main.cpp`: `DragAcceptFiles(hwnd, TRUE)`, `WM_DROPFILES` handler in `WndProc`.
- `src/ui/rack_view.cpp` (and `practice_tools_modal.cpp`): `renderLooperCircularProgressRing` and `renderFloatingHudToast`.
- `tests/test_praccy.cpp`: Regression tests 25, 26, 27, 28.

### 5.3 Invalidation Conditions
- Any dynamic heap allocation occurring within `GraphEngine::process()` during scene crossfade.
- Any deallocation of `m_retiringNodes` executing on the real-time audio thread.
- Any nonzero layout shift occurring in `renderSceneBar()` when presets are switched.
- Acceptance of non-WAV files in drag-and-drop validation.
- Departure from equal-power identity ($g_{\text{out}}^2 + g_{\text{in}}^2 \neq 1.0$).
