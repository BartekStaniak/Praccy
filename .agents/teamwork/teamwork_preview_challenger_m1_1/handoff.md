# Empirical Challenge Report: Milestone 1 — Audio DSP Concurrency & Real-Time Engine

**Agent**: `teamwork_preview_challenger_m1_1`  
**Milestone**: Milestone 1 (Audio DSP Concurrency & Real-Time Engine)  
**Verdict**: **REQUEST_CHANGES**  

---

## 1. Observation

### 1.1 Real-Time Heap Allocation Verification (Zero Audio-Thread Allocations)
- **Harness**: Authored `tests/test_challenger_m1.cpp` containing global overrides for `operator new`, `operator new[]`, `operator delete`, and `operator delete[]` instrumented with `thread_local bool t_isAudioThread` and atomic allocation/deallocation counters (`g_audioThreadAllocations`, `g_audioThreadDeallocations`).
- **Execution Command**:
  ```powershell
  .\build\test_challenger_m1.exe
  ```
- **Observed Result**:
  ```
  [CHALLENGE 3] Real-Time Zero Heap Allocation Verification... 
    Audio blocks streamed: 10162
    Heap allocations on audio thread: 0
    Heap deallocations on audio thread: 0
    -> PASSED: ZERO heap allocations and ZERO heap deallocations across 10,000 audio blocks.
  ```
- During continuous streaming of 10,162 DSP blocks (128 samples @ 48 kHz) executing 24-bit unpack/pack, `InstrumentTuner::pushSamples` (mono and stereo), and `GraphEngine::process` through serial and parallel branches with active `OverdriveEffect` and `StereoDelayEffect`, exactly **0** dynamic memory allocations and **0** deallocations occurred on the audio thread.

### 1.2 24-Bit Sample Unpack & Pack Fidelity (ASIOSTInt24LSB & ASIOSTInt32LSB24)
- **Harness**: Tested key boundary points ($0$, $\pm 1$, $\pm 0.5$, $\pm 0.25$, max positive $8,388,607$, max negative $-8,388,608$), $10,000$ pseudo-random 24-bit signed integers, subnormals (`std::numeric_limits<float>::denorm_min()`), out-of-bounds inputs ($\pm 1000.0f$, $\pm 1.5f$), infinities ($\pm \infty$), and NaNs.
- **Observed Result**:
  ```
  [CHALLENGE 2] Numerical Edge Case & Fidelity Testing for 24-Bit Formats... 
    Part 1: ASIOSTInt24LSB Round-Trip Sweep... PASSED (10017 vectors 100% bit-exact)
    Part 2: ASIOSTInt32LSB24 Round-Trip Sweep with High-Byte DMA Noise... PASSED (10007 vectors 100% bit-exact with noise immunity)
    Part 3: Extreme Inputs, Denormals, Infs, and NaNs... 
      Quiet NaN packed bytes: 0x0 0x0 0x80
      Signaling NaN packed bytes: 0x0 0x0 0x80
      Quiet NaN in Int32LSB24: 0x800000
    -> PASSED: Clamping, denormals, and infinities handled safely without crash or arithmetic overflow.
  ```
- Round-trip bit-exactness is 100% verified across both formats, including high-byte noise immunity on `ASIOSTInt32LSB24`.
- **Finding on NaN inputs**: In `src/audio/asio_manager.cpp` (lines 271, 296), `std::clamp(NaN, -1.0f, 1.0f)` returns `NaN`. When cast via `static_cast<int32_t>(NaN)`, x86 SSE2 produces `0x80000000` (-2147483648), which clamps to `-8388608` (`0x800000`). Consequently, any NaN output from a plugin produces full-scale negative DC (`-1.0f`) on hardware outputs rather than muted silence (`0.0f`).

### 1.3 Concurrency & Audio-Thread Destructor Elimination
- **Harness**: Pushed 14,009 blocks on a high-priority simulated audio callback thread while UI thread performed 1,500 rapid slot mutations (`addSlot`, `removeSlot`, `setGainDb`, `setPan`, `setDryWet`, `setBypassed`) injecting `DtorTrackedNode` instances.
- **Observed Result**:
  ```
  [CHALLENGE 1A] High-contention ParallelBranch concurrency stress test... 
    Audio blocks processed: 14009
    Total destructors run: 3000
    Audio-thread destructors: 0
    -> PASSED: Zero plugin destructions on audio thread under 1500 contention cycles.
  ```
  Across 3,000 slot destructions, exactly **0** occurred on the audio callback thread.

### 1.4 CRITICAL BUG: Command Queue Saturation Causes Dangling Pointer & Use-After-Free
- **Location**: `src/audio/graph_engine.cpp`, lines 177–183:
  ```cpp
  PluginSlot* rawPtr = slot.get();
  m_uiSlots.push_back(rawPtr);

  SlotCommand cmd = SlotCommand::makeAdd(std::move(slot));
  if (!m_commandQueue.try_enqueue(std::move(cmd))) {
      // Queue full fallback
  }
  ```
- **Empirical Test**: Created `audio::ParallelBranch` and called `addSlot()` 300 times without calling `process()` (simulating rapid preset loading, batch additions, or UI actions while audio engine is paused).
- **Observed Result**:
  ```
  [CHALLENGE 1B] Command Queue Saturation & Dangling Pointer Attack... 
    Added 300 slots to isolated branch (capacity 64/128).
    branch.numSlots() reported: 300
    Slots destructed immediately on queue overflow: 173
    [EMPIRICALLY CONFIRMED BUG]: 173 slots were DESTRUCTED due to command queue overflow, but branch.numSlots() reports 300! m_uiSlots holds dangling pointers!
    Slot[127] pointer in m_uiSlots: 0x1ef7a7f7640 (already freed!)
    ...
  ```
- When `m_commandQueue.try_enqueue(std::move(cmd))` fails:
  1. `cmd` goes out of scope and destroys `cmd.slot` immediately on the UI thread.
  2. `m_uiSlots` is **not rolled back** (`m_uiSlots.pop_back()`), retaining `rawPtr` which now points to deallocated memory.
  3. `branch.numSlots()` falsely reports 300 instead of 127.
  4. Calling `branch.getSlot(i)` for $i \ge 127$ returns a dangling pointer, resulting in **Use-After-Free / Heap Corruption / Access Violation**.
  5. If `branch.removeSlot(i)` is subsequently called, `cmd.targetSlot` is not found on the audio thread, causing the audio thread to fall back to `it = m_activeSlots.begin() + cmd.index;`, **accidentally deleting an innocent active slot**.

### 1.5 CONCURRENCY RACE: InstrumentTuner::prepare() vs Active Background Worker Thread
- **Location**: `src/tools/tuner.cpp`, lines 157–171:
  ```cpp
  void InstrumentTuner::prepare(double sampleRate, uint32_t maxBlockSize) {
      m_sampleRate = sampleRate;
      m_maxBlockSize = maxBlockSize;
      const size_t ringCap = std::max<size_t>(32768, maxBlockSize * 8);
      m_ringBuffer.resize(ringCap);
      m_ringBuffer.reset();
  ```
  and `InstrumentTuner::InstrumentTuner` (line 144):
  ```cpp
  InstrumentTuner::InstrumentTuner(uint32_t bufferSize)
      : ... {
      start();
  }
  ```
- `InstrumentTuner` launches `m_workerThread` in its constructor. When `prepare()` is called during engine configuration or sample rate changes, `m_workerThread` is actively running `workerLoop()` and calling `m_ringBuffer.read(...)`.
- `m_ringBuffer.resize()` invokes `m_buffer.assign(cap, 0.0f);`, reallocating the underlying `std::vector` heap buffer while the worker thread concurrently reads it, creating an unsynchronized data race. `prepare()` does not stop the worker thread (`stop()`) prior to buffer reallocation.

### 1.6 CTest Regression Suite Execution
- **Command**:
  ```powershell
  ctest --test-dir build --output-on-failure
  ```
- **Observed Result**:
  ```
  Test project F:/Projects/Praccy/build
      Start 1: test_praccy
  1/2 Test #1: test_praccy ......................   Passed    0.29 sec
      Start 2: test_challenger_m1
  2/2 Test #2: test_challenger_m1 ...............   Passed    0.52 sec

  100% tests passed out of 2
  Total Test time (real) =   0.81 sec
  ```

---

## 2. Logic Chain

1. **Audio Callback Real-Time Safety (Verified)**:
   - Observation 1.1 demonstrated zero heap allocations (`malloc`, `new`) and zero deallocations across 10,162 continuous blocks.
   - Observation 1.3 demonstrated zero destructors executing on the audio callback thread across 14,009 blocks under 1,500 contention cycles.
   - The lock-free SPSC architecture for steady-state streaming is verified real-time safe.

2. **24-Bit ASIO Formats (Verified with 1 Minor Finding)**:
   - Observation 1.2 confirmed 100% bit-exact round-trip accuracy for `ASIOSTInt24LSB` and `ASIOSTInt32LSB24` across >10,000 vectors and boundary values.
   - However, Observation 1.2 showed that `std::clamp` on NaNs produces full-scale negative DC (`-1.0f`) instead of muted silence (`0.0f`).

3. **Critical Defect: Command Queue Saturation in `ParallelBranch::addSlot`**:
   - Observation 1.4 demonstrated that when `m_commandQueue.try_enqueue` returns `false`, `rawPtr` has already been pushed to `m_uiSlots` and is never removed.
   - When the function returns, `cmd` is destroyed and deallocates the `PluginSlot`.
   - `m_uiSlots` now contains dangling pointers to freed memory.
   - Dereferencing these pointers via `getSlot()` results in Use-After-Free / crash.

4. **Concurrency Race in `InstrumentTuner::prepare`**:
   - Observation 1.5 demonstrated that `prepare()` reallocates `m_ringBuffer` via `m_buffer.assign()` while `m_workerThread` is actively running, without stopping the worker or acquiring synchronization.

---

## 3. Caveats

1. The command queue saturation issue only manifests when more than 127 commands are queued before the audio thread processes them. In typical 60 FPS UI rendering with 1–2 slot changes per frame, this threshold is not reached. However, in batch operations, preset changes while audio is stopped, or stress conditions, it immediately causes Use-After-Free.
2. The NaN conversion finding does not cause memory corruption or crashes; it results in an audible DC pulse if an unisolated plugin produces NaNs.

---

## 4. Conclusion

**Verdict**: **REQUEST_CHANGES**

While Milestone 1 achieves remarkable performance—including verified zero heap allocations on the audio callback thread and 100% bit-exact 24-bit audio conversion—the following defects must be resolved:

1. **[CRITICAL] Roll back `m_uiSlots` on `try_enqueue` failure in `ParallelBranch::addSlot`**:
   In `src/audio/graph_engine.cpp`:
   ```cpp
   if (!m_commandQueue.try_enqueue(std::move(cmd))) {
       m_uiSlots.pop_back(); // Prevent dangling pointer in UI registry
   }
   ```
2. **[MEDIUM] Synchronize or stop worker in `InstrumentTuner::prepare`**:
   In `src/tools/tuner.cpp`, `InstrumentTuner::prepare()` must invoke `stop()` before resizing `m_ringBuffer`, and resume `start()` afterwards.
3. **[LOW] Sanitize NaNs in `packInt24LSB` and `packInt32LSB24`**:
   Check `if (std::isnan(src[s])) sample = 0.0f;` to prevent full-scale DC pops on NaN inputs.
4. **[LOW] Fix iterator invalidation risk in `ParallelBranch::process` `SlotCommandType::Clear`**:
   Ensure `m_activeSlots` is not appended to while iterating during emergency reclaim stash.

---

## 5. Verification Method

### 5.1 Compilation & Execution Commands
```powershell
cmake -B build
cmake --build build --config Release --target test_challenger_m1
.\build\test_challenger_m1.exe
```

### 5.2 CTest Execution Command
```powershell
ctest --test-dir build --output-on-failure
```

### 5.3 Files to Inspect
- `tests/test_challenger_m1.cpp`: Complete empirical challenge test harness covering zero-allocation verification, 24-bit fidelity, and concurrency stress testing.
- `src/audio/graph_engine.cpp`: Lines 177–183 (`ParallelBranch::addSlot`).
- `src/tools/tuner.cpp`: Lines 151–171 (`InstrumentTuner::prepare`).
- `src/audio/asio_manager.cpp`: Lines 267–300 (`packInt24LSB`, `packInt32LSB24`).

### 5.4 Invalidation Conditions
- Any occurrence of dangling pointers in `m_uiSlots` when adding >128 slots to `ParallelBranch`.
- Any heap allocation detected on the audio callback thread during streaming.
- Any bit-mismatch in 24-bit unpack/pack round trips.
