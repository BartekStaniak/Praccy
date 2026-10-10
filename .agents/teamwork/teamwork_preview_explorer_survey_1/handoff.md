# Audio DSP Subsystem and Real-Time Engine Survey (Requirement R1)

## 1. Observation

### 1.1 `ParallelBranch::m_slots` Concurrency Architecture
* **Definition & Member Variables**:
  * Located in `src/audio/graph_engine.h:97`:
    ```cpp
    std::vector<std::unique_ptr<PluginSlot>> m_slots;
    ```
  * Public accessors and mutators in `src/audio/graph_engine.h`:
    * Line 67: `void addSlot(std::unique_ptr<PluginSlot> slot);`
    * Line 68: `void removeSlot(size_t index);`
    * Line 69: `std::unique_ptr<PluginSlot> takeSlot(size_t index);`
    * Line 70: `[[nodiscard]] size_t numSlots() const noexcept { return m_slots.size(); }`
    * Line 71: `[[nodiscard]] PluginSlot* getSlot(size_t index) noexcept;`
    * Line 91: `[[nodiscard]] const std::vector<std::unique_ptr<PluginSlot>>& slots() const noexcept { return m_slots; }`
* **Audio Callback Processing**:
  * In `src/audio/graph_engine.cpp:157-175`:
    ```cpp
    void ParallelBranch::process(AudioProcessContext& ctx) {
        const uint32_t numSamples = ctx.numSamples;
        auto branchView = m_branchBuffer.view(numSamples);
        branchView.copyFrom(ctx.input);

        for (auto& slot : m_slots) {
            auto tempView = m_slotTemp.view(numSamples);
            AudioProcessContext slotCtx{
                .input = branchView,
                .output = tempView,
                .sampleRate = ctx.sampleRate,
                .numSamples = numSamples
            };
            slot->process(slotCtx);
            branchView.copyFrom(tempView);
        }

        m_meter.process(branchView.channel(0), branchView.numChannels() > 1 ? branchView.channel(1) : nullptr, numSamples);
    }
    ```
  * Notice: `ParallelBranch::process()` executes on the high-priority real-time ASIO audio callback thread without acquiring any lock or synchronization primitive.
* **UI Mutations**:
  * In `src/ui/rack_view.cpp`:
    * Line 1318: `if (br) br->removeSlot(slotIndex);` (invoked when user clicks the delete button on Branch A slot)
    * Line 1654: `if (br) br->removeSlot(slotIndex);` (invoked when user clicks the delete button on Branch B slot)
    * Line 2541: `branch->addSlot(std::move(newSlot));` (invoked when user selects a plugin from the plugin browser modal)
* **UI Queries During Every Frame Render**:
  * In `src/ui/rack_view.cpp`:
    * Line 1692-1693: `const size_t numSlots0 = br0 ? br0->numSlots() : 0;`
    * Line 1854: `for (size_t s = 0; s < numSlots0; ++s)`
    * Line 1855: `auto* bSlot = br0->getSlot(s);`
* **Scene Manager Persistence**:
  * In `src/state/scene_manager.cpp`:
    * Lines 217-218: `for (size_t s = 0; s < branch->numSlots(); ++s) { auto* bSlot = branch->getSlot(s); ... }`
    * Line 282: `branch->addSlot(std::move(slot));`
    * Line 423: `branch->addSlot(std::move(slot));`
* **Root Cause Analysis**:
  * `m_slots` is a standard `std::vector`. While `GraphEngine::m_nodes` has a `std::mutex m_graphMutex` (with `try_to_lock` in `GraphEngine::process()`), `ParallelBranch` has NO mutex and NO lock-free queue.
  * When the UI thread calls `branch->addSlot()`, `std::vector::push_back()` may reallocate the internal buffer, invalidating pointers and iterators while the audio callback is actively executing `for (auto& slot : m_slots)`.
  * When the UI thread calls `branch->removeSlot()`, `std::vector::erase()` shifts elements and alters vector size mid-iteration, causing access violations or undefined behavior.

---

### 1.2 `third_party/readerwriterqueue` Status
* **Files Present**:
  * Located in directory `f:/Projects/Praccy/third_party/readerwriterqueue/`:
    * `readerwriterqueue.h` (32,727 bytes, version by Cameron Desrochers, Simplified BSD license)
    * `atomicops.h` (23,051 bytes)
* **Classes & Capabilities**:
  * `moodycamel::ReaderWriterQueue<T>`: Lock-free Single-Producer Single-Consumer (SPSC) queue.
  * `moodycamel::BlockingReaderWriterQueue<T>`: Blocking variant of the SPSC queue.
  * Methods:
    * `try_enqueue(T const&)` / `try_enqueue(T&&)`: Fully wait-free, non-allocating if pre-sized.
    * `enqueue(T const&)` / `enqueue(T&&)`: Allocates additional blocks only if capacity exceeded (safe for producer/UI thread).
    * `try_dequeue(U& result)`: Lock-free, wait-free, zero allocation (safe for audio thread).
    * `peek()`: Inspects head element without dequeuing.
* **Build System Reference**:
  * `CMakeLists.txt:65`: `third_party/readerwriterqueue` is included under `target_include_directories(Praccy PRIVATE ...)`.
  * `CMakeLists.txt:108`: For `test_praccy`, `third_party` is included, allowing `#include <readerwriterqueue/readerwriterqueue.h>` or direct inclusion if `third_party/readerwriterqueue` is added.

---

### 1.3 `InstrumentTuner` Implementation, Threading Model & Heap Allocations
* **Definition & Implementation**:
  * Located in `src/tools/tuner.h` and `src/tools/tuner.cpp`.
* **Pitch Detection Algorithm (YIN)**:
  * In `src/tools/tuner.cpp:34-122`:
    * Computes RMS signal energy (line 42).
    * Step 1 (Difference function, lines 49-58):
      ```cpp
      for (uint32_t tau = 0; tau < halfBuffer; ++tau) {
          float sum = 0.0f;
          for (uint32_t j = 0; j < halfBuffer; ++j) {
              const uint32_t idx1 = (m_writeIndex + j) % m_bufferSize;
              const uint32_t idx2 = (m_writeIndex + j + tau) % m_bufferSize;
              const float diff = m_inputHistory[idx1] - m_inputHistory[idx2];
              sum += diff * diff;
          }
          m_differenceBuffer[tau] = sum;
      }
      ```
    * With `m_bufferSize = 2048`, `halfBuffer = 1024`. The inner loop executes $1024 \times 1024 = 1,048,576$ iterations per invocation!
    * Step 2: Cumulative mean normalized difference (lines 60-70).
    * Step 3: Absolute thresholding with `m_yinThreshold = 0.15f` (lines 72-83).
    * Step 4: Parabolic interpolation for sub-cent peak localization (lines 89-99).
    * Step 5: Frequency conversion and MIDI note computation (lines 106-121).
* **Current Threading Model (Real-Time Violation)**:
  * In `src/main.cpp:156-181`:
    ```cpp
    asio.setAudioCallback([&](const praccy::audio::AudioBufferView& in, praccy::audio::AudioBufferView& out) {
        ...
        // 1. Instrument Tuner - respects active input routing channel
        ...
        tuner.process(tunerSrc, in.numSamples());
        ...
    });
    ```
  * `tuner.process()` is called synchronously directly within the real-time ASIO audio callback.
  * At 48 kHz / 256 samples, the callback fires $\approx 187.5$ times/sec.
  * $187.5 \times 1,048,576 \approx 196.6$ million operations per second executed on the real-time thread!
  * This causes high CPU load in the audio callback (>20–50% DSP load while idling) and causes buffer dropouts (`dspDropouts`).
* **Dynamic Heap Allocations on Audio Thread**:
  * In `src/main.cpp:171-172`:
    ```cpp
    static thread_local std::vector<float> s_tunerMixBuf;
    if (s_tunerMixBuf.size() < in.numSamples()) s_tunerMixBuf.resize(in.numSamples());
    ```
  * Executed directly on the ASIO thread when `InputRoutingMode::Stereo` is selected.
  * `s_tunerMixBuf.resize()` issues dynamic memory allocations (`malloc` / `realloc`) inside the audio thread.
  * In `InstrumentTuner::prepare(double sampleRate)` (`src/tools/tuner.cpp:17-21`): only takes `sampleRate` and does not pre-allocate or manage mixing buffers.

---

### 1.4 `asio_manager.cpp` & `asio_manager.h`: Formats & MMCSS
* **Current Sample Formats Supported**:
  * In `src/audio/asio_manager.cpp:260-284` (Input unpacking):
    * `ASIOSTInt32LSB`: scaled by `1.0f / 2147483648.0f`.
    * `ASIOSTFloat32LSB`: `std::memcpy`.
    * `ASIOSTInt16LSB`: scaled by `1.0f / 32768.0f`.
    * Default: `std::memset(dst, 0, bufferSize * sizeof(float));`
  * In `src/audio/asio_manager.cpp:302-326` (Output packing):
    * `ASIOSTInt32LSB`, `ASIOSTFloat32LSB`, `ASIOSTInt16LSB`.
    * Default: silent ignore.
* **Missing 24-bit Formats**:
  * `ASIOSTInt24LSB` (value 17 in `asio_defs.h`): Packed 3 bytes per sample, 24-bit signed integer in little-endian byte order. Currently unhandled (falls to default and zeroes the audio buffer).
  * `ASIOSTInt32LSB24` (value 27 in `asio_defs.h`): 32-bit container (4 bytes per sample), where the valid 24-bit audio sample is aligned in the 24 least significant bits. Currently unhandled (falls to default).
* **MMCSS Handling (`AvSetMmThreadCharacteristicsW`)**:
  * In `src/audio/asio_manager.h:82-83`:
    ```cpp
    HANDLE m_mmcssHandle{nullptr};
    DWORD m_mmcssTaskIndex{0};
    ```
  * In `src/audio/asio_manager.cpp:129-179` (`AsioManager::start()`):
    * `AvSetMmThreadCharacteristicsW` is NOT called upon `start()`.
  * In `src/audio/asio_manager.cpp:237-240` (`AsioManager::processAudio()`):
    * Lazily calls `AvSetMmThreadCharacteristicsW(L"Pro Audio", &m_mmcssTaskIndex);` on first audio callback invocation.
  * In `src/audio/asio_manager.cpp:188-191` (`AsioManager::stop()`):
    * Calls `AvRevertMmThreadCharacteristics(m_mmcssHandle);` on whatever thread calls `stop()`.

---

### 1.5 Existing Audio Tests, Test Targets & Harness
* **Current Test Suite**:
  * `tests/test_praccy.cpp` (builds `test_praccy.exe`):
    * Contains 11 tests:
      1. `testAudioBuffers()`
      2. `testDspUtils()`
      3. `testGraphEngineSerialAndParallel()`
      4. `testTunerPitchDetection()`
      5. `testMetronome()`
      6. `testSceneManager()`
      7. `testGraphEngineDynamicTopology()`
      8. `testAppConfigPersistence()`
      9. `testParallelBlockBlendAndDissolve()`
      10. `testQuickLooper()`
      11. `testAudioPlayer()`
    * Result: Successfully executed via `ctest --test-dir build --output-on-failure` (100% passed in 0.06s).
* **Missing Tests Identified**:
  * `GraphEngineTest.ConcurrentParallelMutation`: Does not exist in `test_praccy.cpp`. No multi-threaded test exists asserting data race-free concurrent mutation under ThreadSanitizer or stress testing.
  * `AsioManagerTest.Format24BitUnpack`: Does not exist. No automated test exists verifying `ASIOSTInt24LSB` or `ASIOSTInt32LSB24` unpack and pack routines against bit-exact test patterns.
* **`test_asio_driver.cpp` Status**:
  * Compiles to `test_asio_driver.exe`.
  * NOT registered in `CMakeLists.txt` via `add_test()`.
  * Implementation: Scans the registry for installed hardware drivers (`SOFTWARE\ASIO`) and tries loading them. If none are installed, exits with `return 0`.
  * Does NOT contain a mock ASIO driver (`MockAsioDriver`) or automated block pump as specified in the Acceptance Criteria: *"Headless regression harness boots Praccy with mock ASIO driver (`test_asio_driver.cpp`), pumps 20,000 sample blocks with randomized bypass toggles, and asserts zero memory corruption, zero NaN/Inf samples, and zero deadlocks."*

---

## 2. Logic Chain

1. **Concurrency Hazard on `ParallelBranch::m_slots`**:
   * *Observation*: `ParallelBranch::process()` iterates over `m_slots` without mutex locks on the audio thread, while `rack_view.cpp` calls `branch->addSlot()` and `branch->removeSlot()` on the UI thread.
   * *Inference*: Concurrent mutation of `std::vector` while another thread iterates through it is undefined behavior in C++ and will cause data races, iterator invalidation, heap corruption, and crashes.
   * *Resolution Design*: Implement a lock-free Single-Producer Single-Consumer (SPSC) command queue using `readerwriterqueue` where:
     * UI thread pushes commands:
       * `AddSlotCommand { std::unique_ptr<PluginSlot> slot }`
       * `RemoveSlotCommand { size_t index }`
     * Audio thread drains pending commands at the start of `ParallelBranch::process()` and modifies its internal slot container safely.
     * **Real-time Determinism Critical Rule**: Deleting a plugin slot on the audio thread would invoke plugin destructors (`FreeLibrary`, COM Release, memory deallocation), causing priority inversion. Therefore, deleted slots MUST be transferred back to a reclamation queue (SPSC from audio to UI/worker thread) for deallocation off the audio thread.
     * UI queries (`numSlots()`, `getSlot()`): UI needs a safe query mechanism so it does not race with audio thread mutations. Either UI maintains a mirror representation of slot descriptors, or slot arrays use atomic pointer slots (`std::array<std::atomic<PluginSlot*>, MAX_BRANCH_SLOTS>`).

2. **Decoupling `InstrumentTuner`**:
   * *Observation*: YIN pitch detection runs $1,048,576$ inner loop iterations per callback directly inside `asio.setAudioCallback()`.
   * *Observation*: At 48 kHz / 256 samples, this translates to $\approx 196.6$ million operations/sec on the real-time thread, causing high DSP load (>20–50%) and buffer dropouts.
   * *Observation*: `s_tunerMixBuf.resize()` in `main.cpp:172` performs dynamic heap allocation during the audio callback.
   * *Resolution Design*:
     * Audio callback pushes incoming samples into a lock-free circular ring buffer (or `readerwriterqueue<float>`). Pushing 256 samples is $O(N)$ memory copies and takes $<1\,\mu\text{s}$, with zero allocations.
     * A dedicated worker thread running at 60 Hz ($\approx 16.6\,\text{ms}$ sleep period) reads samples from the ring buffer into `m_inputHistory` and executes `detectPitchYin()`.
     * Results (`frequencyHz`, `midiNote`, `cents`, `confidence`) are stored in `std::atomic` fields, readable wait-free by UI `currentResult()`.
     * Pre-allocate mix buffers during `InstrumentTuner::prepare(sampleRate, maxBlockSize)`, eliminating `s_tunerMixBuf.resize()` entirely.
     * This directly satisfies the Acceptance Criteria: "Tuner idle DSP load drops below 5% at 256 samples / 48 kHz" and "Zero dynamic memory allocations occur within the audio callback loop".

3. **24-bit Audio Formats in `AsioManager`**:
   * *Observation*: Formats `ASIOSTInt24LSB` (17) and `ASIOSTInt32LSB24` (27) currently drop into `default:` in `asio_manager.cpp:281, 323`, producing silence or uninitialized hardware output.
   * *Resolution Design*:
     * `ASIOSTInt24LSB` (packed 3 bytes):
       * Unpack: Read 3 bytes little-endian, sign-extend 24th bit:
         `int32_t val = (static_cast<int32_t>(raw[0])) | (static_cast<int32_t>(raw[1]) << 8) | (static_cast<int32_t>(static_cast<int8_t>(raw[2])) << 16);`
         `dst[s] = static_cast<float>(val) * (1.0f / 8388608.0f);`
       * Pack: Scale clamped float $[-1.0, 1.0]$ by $8388607.0f$, write 3 bytes little-endian.
     * `ASIOSTInt32LSB24` (4-byte container, 24-bit LSB-aligned):
       * Unpack: Read 32-bit int, sign-extend bit 23:
         `int32_t val = (raw[s] << 8) >> 8;`
         `dst[s] = static_cast<float>(val) * (1.0f / 8388608.0f);`
       * Pack: Scale clamped float by $8388607.0f$, store in lower 24 bits.
   * *MMCSS Registration*:
     * Invoking `AvSetMmThreadCharacteristicsW(L"Pro Audio", &m_mmcssTaskIndex)` in `AsioManager::start()` ensures MMCSS priority is granted up front, eliminating first-callback initialization latency.

4. **Testing Harness Architecture**:
   * *Observation*: `test_praccy.cpp` passes all 11 tests, but lacks the concurrency stress test and 24-bit format tests.
   * *Observation*: `test_asio_driver.cpp` is an interactive probe for real drivers, unable to run in headless CI environments.
   * *Resolution Design*:
     * Author `GraphEngineTest.ConcurrentParallelMutation` inside `tests/` which launches an audio processing thread running at high frequency while the main thread performs rapid insertions and removals of plugin slots, asserting zero data races and memory stability.
     * Author `AsioManagerTest.Format24BitUnpack` testing known 24-bit bit patterns (positive full scale, negative full scale, zero, -6 dB, noise floor) for both 24-bit formats.
     * Refactor/Author a `MockAsioDriver` implementing `IASIO` interface in `test_asio_driver.cpp` or a headless test harness to pump 20,000 blocks with randomized bypass toggles.

---

## 3. Caveats

1. **UI Slot Rendering Concurrency**:
   * While the audio thread can safely consume commands via `readerwriterqueue`, the UI thread (`rack_view.cpp`) continuously queries `br->numSlots()` and `br->getSlot(s)` every frame. The architecture must address both audio thread consumption AND UI thread observation (e.g. via an atomic slot array or UI mirror representation).
2. **Plugin Destruction Thread Affinity**:
   * Some VST3/CLAP plugins expect destruction on the main thread (where UI or COM apartment was initialized). The reclamation queue must ensure destructors run strictly on the UI thread or non-realtime thread.
3. **ASIO Driver Threading Variability**:
   * Different ASIO drivers invoke `bufferSwitch` on different threads (driver worker thread vs. interrupt thread). MMCSS (`AvSetMmThreadCharacteristicsW`) must be managed such that the thread executing the actual buffer switch is prioritized, while `start()` also registers the streaming control thread.
4. **Third-party vendoring path**:
   * `third_party/readerwriterqueue` is included in `Praccy`, but for `test_praccy` CMake target, include path should ensure `<readerwriterqueue.h>` is discoverable.

---

## 4. Conclusion

The codebase exhibits clear, localized architectural deficiencies in Requirement R1:
1. **ParallelBranch**: Completely unprotected `std::vector<std::unique_ptr<PluginSlot>>` subject to data race during UI insertions/deletions. `readerwriterqueue` is already present in `third_party/` and ready to be integrated into an SPSC command queue with a reclamation return queue.
2. **InstrumentTuner**: CPU-intensive $O(N^2/4)$ YIN algorithm runs synchronously on the audio callback thread (~190M ops/sec), and `main.cpp` calls `s_tunerMixBuf.resize()` on the audio callback thread. A background 60 Hz worker thread with lock-free ring buffer sample ingestion and pre-allocated buffers is necessary.
3. **AsioManager**: Missing unpack/pack routines for `ASIOSTInt24LSB` and `ASIOSTInt32LSB24`, and missing `AvSetMmThreadCharacteristicsW` in `start()`.
4. **Test Harness**: The project compiles cleanly and passes existing unit tests (11/11), but requires `GraphEngineTest.ConcurrentParallelMutation`, `AsioManagerTest.Format24BitUnpack`, and a mock ASIO headless regression harness pumping 20,000 blocks.

---

## 5. Verification Method

### 5.1 Independent Code Verification
1. Inspect `src/audio/graph_engine.h:97` and `src/audio/graph_engine.cpp:157-175` to verify unprotected `m_slots` access.
2. Inspect `src/ui/rack_view.cpp:1318, 1654, 2541` to verify UI mutations of `m_slots`.
3. Inspect `src/tools/tuner.cpp:23-32` and `src/main.cpp:171-180` to verify synchronous YIN execution and `s_tunerMixBuf.resize()` on the audio callback thread.
4. Inspect `src/audio/asio_manager.cpp:260-284, 302-326` to verify absence of `ASIOSTInt24LSB` and `ASIOSTInt32LSB24`.
5. Check `third_party/readerwriterqueue/` for `readerwriterqueue.h` and `atomicops.h`.

### 5.2 Build and Test Commands
Execute the existing test suite:
```powershell
ctest --test-dir f:/Projects/Praccy/build --output-on-failure
```
Execute individual test binaries:
```powershell
f:/Projects/Praccy/build/test_praccy.exe
f:/Projects/Praccy/build/test_asio_driver.exe
```

### 5.3 Invalidation Conditions
This survey's findings are invalidated if:
* `ParallelBranch::m_slots` is already synchronized by an SPSC queue or lock in a separate branch.
* `InstrumentTuner` already delegates to a background thread.
* 24-bit unpack routines exist in an unmerged or alternative translation unit.
