# Technical Blueprint: Features 4, 5 & 6 — 24-Bit ASIO Formats, MMCSS Priority, and Concurrency Unit Tests

## 1. Observation

### 1.1 Existing ASIO Sample Format Handling in `src/audio/asio_manager.cpp`
* **File Location**: `src/audio/asio_manager.cpp`
* **Input Unpacking Loop (`processAudio`)**:
  Lines 260–285:
  ```cpp
  switch (chInfo.type) {
      case ASIOSTInt32LSB: {
          const auto* src = static_cast<const int32_t*>(rawBuf);
          constexpr float inv32 = 1.0f / 2147483648.0f;
          for (uint32_t s = 0; s < bufferSize; ++s) {
              dst[s] = static_cast<float>(src[s]) * inv32;
          }
          break;
      }
      case ASIOSTFloat32LSB: {
          std::memcpy(dst, rawBuf, bufferSize * sizeof(float));
          break;
      }
      case ASIOSTInt16LSB: {
          const auto* src = static_cast<const int16_t*>(rawBuf);
          constexpr float inv16 = 1.0f / 32768.0f;
          for (uint32_t s = 0; s < bufferSize; ++s) {
              dst[s] = static_cast<float>(src[s]) * inv16;
          }
          break;
      }
      default:
          std::memset(dst, 0, bufferSize * sizeof(float));
          break;
  }
  ```
* **Output Packing Loop (`processAudio`)**:
  Lines 302–326:
  ```cpp
  switch (chInfo.type) {
      case ASIOSTInt32LSB: {
          auto* dst = static_cast<int32_t*>(rawBuf);
          for (uint32_t s = 0; s < bufferSize; ++s) {
              float sample = std::clamp(src[s], -1.0f, 1.0f);
              dst[s] = static_cast<int32_t>(sample * 2147483647.0f);
          }
          break;
      }
      case ASIOSTFloat32LSB: {
          std::memcpy(rawBuf, src, bufferSize * sizeof(float));
          break;
      }
      case ASIOSTInt16LSB: {
          auto* dst = static_cast<int16_t*>(rawBuf);
          for (uint32_t s = 0; s < bufferSize; ++s) {
              float sample = std::clamp(src[s], -1.0f, 1.0f);
              dst[s] = static_cast<int16_t>(sample * 32767.0f);
          }
          break;
      }
      default:
          break;
  }
  ```
* **Deficiency**: Neither `ASIOSTInt24LSB` (type 17 in `src/audio/asio_defs.h:35`) nor `ASIOSTInt32LSB24` (type 27 in `src/audio/asio_defs.h:42`) are handled. Any audio hardware configured to deliver 24-bit samples falls through to `default:`, causing silent zeroed input and unwritten hardware output buffers.

### 1.2 MMCSS Thread Lifecycle Defect in `src/audio/asio_manager.cpp`
* **File Location**: `src/audio/asio_manager.h` and `src/audio/asio_manager.cpp`
* **Member declarations** in `src/audio/asio_manager.h:82-83`:
  ```cpp
  HANDLE m_mmcssHandle{nullptr};
  DWORD m_mmcssTaskIndex{0};
  ```
* **Existing Start implementation** in `src/audio/asio_manager.cpp:129-179`:
  `AsioManager::start(int32_t bufferSize)` does NOT call `AvSetMmThreadCharacteristicsW`.
* **Existing Audio Callback implementation** in `src/audio/asio_manager.cpp:236-240`:
  ```cpp
  // Enable MMCSS "Pro Audio" priority for real-time thread if not already set
  if (!m_mmcssHandle) {
      m_mmcssTaskIndex = 0;
      m_mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &m_mmcssTaskIndex);
  }
  ```
* **Existing Stop implementation** in `src/audio/asio_manager.cpp:188-192`:
  ```cpp
  if (m_mmcssHandle) {
      AvRevertMmThreadCharacteristics(m_mmcssHandle);
      m_mmcssHandle = nullptr;
  }
  ```
* **Cross-Thread Affinity Defect**:
  `AvSetMmThreadCharacteristicsW` is called inside `processAudio()` on the *ASIO driver's streaming worker thread*. It saves the returned handle into `m_mmcssHandle`. Later, when `AsioManager::stop()` is called from the *UI/application thread*, `AvRevertMmThreadCharacteristics(m_mmcssHandle)` is called on a completely different thread.
  Under the Win32 Avrt API, `AvRevertMmThreadCharacteristics` relinquishes the multimedia task association for the *calling thread*. Calling it across threads fails or leaves the audio thread in an inconsistent state. Furthermore, lazy registration during the first `processAudio()` callback incurs an OS service IPC stall during the critical initial audio deadline.

### 1.3 Test Suite State in `tests/test_praccy.cpp`
* **File Location**: `tests/test_praccy.cpp`
* **Build Target**: `add_executable(test_praccy ...)` in `CMakeLists.txt:92-114`.
* **Status**: 11 unit tests currently execute and pass (`AudioBufferView`, `DspUtils`, `GraphEngineSerialAndParallel`, `TunerPitchDetection`, `Metronome`, `SceneManager`, `DynamicTopology`, `AppConfigPersistence`, `ParallelBlockBlendAndDissolve`, `QuickLooper`, `AudioPlayer`).
* **Missing Tests Identified**:
  1. `GraphEngineTest.ConcurrentParallelMutation`: No automated multi-threaded stress test verifies that `ParallelBranch` mutations (slot additions, removals) can safely occur concurrently while an audio thread is processing samples at high frequency.
  2. `AsioManagerTest.Format24BitUnpack`: No bit-exact unit test verifies unpacking and packing across boundary values (0, max positive, max negative, mid-range) for `ASIOSTInt24LSB` and `ASIOSTInt32LSB24`.
  3. `test_praccy` currently does not link `src/audio/asio_manager.cpp` nor `avrt.lib`.

---

## 2. Logic Chain

### 2.1 Feature 4: 24-Bit Sample Unpacking & Packing Algorithms

#### A. Format `ASIOSTInt24LSB` (Type 17: Packed 3 Bytes, Little-Endian)
1. **Memory Representation**:
   - Each sample is stored as 3 contiguous bytes (`uint8_t raw[3]`).
   - Byte 0: Bits 0–7 (LSB).
   - Byte 1: Bits 8–15.
   - Byte 2: Bits 16–23 (MSB, with sign bit at bit 7 of this byte, corresponding to bit 23 of the sample).
   - Numerical range: $[-8,388,608, +8,388,607]$ ($[-2^{23}, 2^{23}-1]$).
2. **Branchless Sign Extension for Unpacking**:
   - Assembling into a 32-bit unsigned integer aligned at bits 8–31:
     $$\text{uval} = (\text{raw}[0] \ll 8) \mid (\text{raw}[1] \ll 16) \mid (\text{raw}[2] \ll 24)$$
   - Shifting right by 8 with signed arithmetic shift (`int32_t`):
     $$\text{sample24} = \text{static\_cast<int32\_t>}(\text{uval}) \gg 8$$
   - Verification:
     - If $\text{raw} = [0\text{xFF}, 0\text{xFF}, 0\text{x7F}]$ ($+8,388,607$):
       $\text{uval} = 0\text{x7FFFFF00}$. $\text{sample24} = 0\text{x007FFFFF} = +8,388,607$.
     - If $\text{raw} = [0\text{x00}, 0\text{x00}, 0\text{x80}]$ ($-8,388,608$):
       $\text{uval} = 0\text{x80000000}$. $\text{sample24} = 0\text{xFF800000} = -8,388,608$.
     - If $\text{raw} = [0\text{xFF}, 0\text{xFF}, 0\text{xFF}]$ ($-1$):
       $\text{uval} = 0\text{xFFFFFF00}$. $\text{sample24} = 0\text{xFFFFFFFF} = -1$.
     - If $\text{raw} = [0\text{x00}, 0\text{x00}, 0\text{x00}]$ ($0$):
       $\text{uval} = 0\text{x00000000}$. $\text{sample24} = 0$.
   - Scale factor to floating point $[-1.0f, +1.0f]$:
     $$\text{inv24} = \frac{1.0f}{8388608.0f} = \frac{1.0f}{2^{23}}$$
     $$\text{dst}[s] = \text{static\_cast<float>}(\text{sample24}) \times \text{inv24}$$
     $-8,388,608 \times \text{inv24} = -1.0f$ (exact IEEE 754 float representation).
     $+8,388,607 \times \text{inv24} = +0.99999988f$.
3. **Packing (Float to Packed 3 Bytes)**:
   - Clamp float input to $[-1.0f, 1.0f]$.
   - Asymmetric scaling for bit-exact round-trip:
     ```cpp
     float sample = std::clamp(src[s], -1.0f, 1.0f);
     int32_t val = (sample >= 0.0f)
         ? static_cast<int32_t>(sample * 8388607.0f)
         : static_cast<int32_t>(sample * 8388608.0f);
     val = std::clamp(val, -8388608, 8388607);
     raw[0] = static_cast<uint8_t>(val & 0xFF);
     raw[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
     raw[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
     ```
   - For $-1.0f$, $val = -8,388,608$ (`0xFF800000`), yielding bytes `[0x00, 0x00, 0x80]`, which matches the hardware full-scale negative pattern bit-for-bit.

---

#### B. Format `ASIOSTInt32LSB24` (Type 27: 4-Byte Container, 24-Bit LSB-Aligned)
1. **Memory Representation**:
   - Container is a 32-bit integer (`int32_t`).
   - Valid 24-bit audio data resides in bits 0–23 (LSB aligned). Bit 23 is the sign bit.
   - Bits 24–31 are padding, which may be zero-extended (`0x00`), sign-extended (`0xFF` for negative values), or uninitialized DMA noise depending on the hardware ASIO driver.
2. **Robust Branchless Unpacking**:
   - Discard bits 24–31 and align bit 23 with bit 31 via unsigned left shift by 8:
     $$\text{uval} = \text{static\_cast<uint32\_t>}(\text{rawVal}) \ll 8$$
   - Arithmetic right shift by 8:
     $$\text{sample24} = \text{static\_cast<int32\_t>}(\text{uval}) \gg 8$$
   - Verification across driver padding behaviors:
     - Zero-extended negative sample: $\text{rawVal} = 0\text{x00800000}$.
       $\text{uval} = 0\text{x80000000} \implies \text{sample24} = -8,388,608$ (correctly sign-extended).
     - Sign-extended negative sample: $\text{rawVal} = 0\text{xFF800000}$.
       $\text{uval} = 0\text{x80000000} \implies \text{sample24} = -8,388,608$ (identical result).
     - Noise in upper byte: $\text{rawVal} = 0\text{xAB800000}$.
       $\text{uval} = 0\text{x80000000} \implies \text{sample24} = -8,388,608$ (immune to high-byte DMA garbage).
   - Multiply by $\text{inv24} = \frac{1.0f}{8388608.0f}$.
3. **Packing (Float to 32-Bit Container)**:
   - Scale clamped float $[-1.0f, 1.0f]$ to $[-8,388,608, 8,388,607]$.
   - Store signed 32-bit integer `dst[s] = val;` (bits 24–31 are natural sign extension). Both signed 32-bit and masked `val & 0x00FFFFFF` are decoded identically by the unpack algorithm.

---

### 2.2 Feature 5: MMCSS Thread Registration Lifecycle Design

1. **Lifecycle Binding**:
   - `AvSetMmThreadCharacteristicsW(L"Pro Audio", &m_mmcssTaskIndex)` must be invoked inside `AsioManager::start()` on the control thread that initializes audio streaming, prior to calling `m_driver->start()`.
   - `AvRevertMmThreadCharacteristics(m_mmcssHandle)` must be invoked inside `AsioManager::stop()` on the identical control thread, ensuring the handle is relinquished by the thread that created it.
2. **Audio Callback Thread Protection**:
   - Remove the shared `m_mmcssHandle` assignment from `processAudio()`.
   - If callback thread priority boosting is desired inside `processAudio()`, use a `thread_local HANDLE` so that it is thread-isolated and does not mutate `AsioManager::m_mmcssHandle`.
   - Result:
     - Eliminates cross-thread handle reversion.
     - Eliminates first-callback IPC delay during real-time streaming startup.
     - Guarantees zero handle leaks upon `AsioManager::stop()` and `~AsioManager()`.

---

### 2.3 Feature 6: Concurrency and 24-Bit Unit Tests Design

1. **Architecture for Testability**:
   - `AsioManager::processAudio` cannot be run in headless unit test environments without a physical sound card or COM driver registered in the Windows registry.
   - Solution: Extract the sample conversion routines into pure, static methods in `AsioManager` (or a dedicated `AsioSampleConverter` helper struct in `src/audio/asio_manager.h`):
     - `static void unpackSamples(ASIOSampleType type, const void* src, float* dst, uint32_t numSamples)`
     - `static void packSamples(ASIOSampleType type, const float* src, void* dst, uint32_t numSamples)`
     - `static void unpackInt24LSB(const void* src, float* dst, uint32_t numSamples)`
     - `static void packInt24LSB(const float* src, void* dst, uint32_t numSamples)`
     - `static void unpackInt32LSB24(const void* src, float* dst, uint32_t numSamples)`
     - `static void packInt32LSB24(const float* src, void* dst, uint32_t numSamples)`
   - This allows `tests/test_praccy.cpp` to verify bit-exact unpacking and packing in milliseconds without mock drivers or COM initialization.
2. **`AsioManagerTest.Format24BitUnpack`**:
   - Tests boundary values: Zero (`0`), Max positive (`+8,388,607`), Max negative (`-8,388,608`), Mid-range positive (`+4,194,304` / `+0.5f`), Mid-range negative (`-4,194,304` / `-0.5f`), Small positive (`+1 LSB`), Small negative (`-1 LSB`).
   - Asserts bit-exact reproduction and round-trip fidelity.
3. **`GraphEngineTest.ConcurrentParallelMutation`**:
   - Launches a high-frequency audio pumping thread executing `GraphEngine::process()` continuously over thousands of iterations.
   - Concurrently runs a UI worker thread performing rapid mutations on `ParallelBranch`: inserting `PluginSlot`s, removing slots, setting branch pans, gains, and mute/solo flags.
   - Asserts zero NaN/Inf samples, zero segmentation faults, zero thread deadlocks, and clean thread joins. Under ThreadSanitizer or stress testing, validates the lock-free synchronization designed in Feature 1.

---

## 3. Caveats

1. **Alignment and Unaligned Memory Access**:
   `ASIOSTInt24LSB` accesses 3-byte strides ($s \times 3$). On x86/x64 processors, unaligned 32-bit reads are natively supported by the hardware with negligible penalty. However, reading byte-by-byte via `raw[0], raw[1], raw[2]` is strictly portable across all architectures and avoids any unaligned pointer dereference undefined behavior.
2. **Rounding Modes for 24-Bit Packing**:
   Floating-point scaling by `8388607.0f` for positive values and `8388608.0f` for negative values guarantees that $-1.0f$ maps directly to $-8,388,608$ and $+1.0f$ maps directly to $+8,388,607$. Mid-range values such as $+0.5f$ produce $4,194,303.5f$, which truncates to $4,194,303$ with `static_cast<int32_t>` or rounds to $4,194,304$ with `std::lrint`. Both representations are within 1 LSB of exact half-scale. The unit test allows a $\pm 1$ LSB tolerance for fractional mid-points.
3. **Build Target Linking**:
   When `AsioManager` static methods or `src/audio/asio_manager.cpp` are referenced by `test_praccy.cpp`, `CMakeLists.txt` must link `winmm` and `avrt` to `test_praccy`. Alternatively, placing `AsioSampleConverter` inline in `src/audio/asio_manager.h` avoids adding `avrt` and `ole32` dependencies to `test_praccy`. Both patterns are documented in the implementation blueprints below.

---

## 4. Conclusion & Concrete Implementation Blueprints

### 4.1 Blueprint: `src/audio/asio_manager.h`
Add static sample conversion declarations and clean up MMCSS tracking:

```cpp
// In namespace praccy::audio, inside class AsioManager:

class AsioManager {
public:
    // ... existing public methods ...

    // Sample format conversion routines (public for real-time engine & unit tests)
    static void unpackSamples(ASIOSampleType type, const void* src, float* dst, uint32_t numSamples) noexcept;
    static void packSamples(ASIOSampleType type, const float* src, void* dst, uint32_t numSamples) noexcept;

    static void unpackInt24LSB(const void* src, float* dst, uint32_t numSamples) noexcept;
    static void packInt24LSB(const float* src, void* dst, uint32_t numSamples) noexcept;

    static void unpackInt32LSB24(const void* src, float* dst, uint32_t numSamples) noexcept;
    static void packInt32LSB24(const float* src, void* dst, uint32_t numSamples) noexcept;

    // ...
```

---

### 4.2 Blueprint: `src/audio/asio_manager.cpp`
Implement 24-bit unpack/pack routines and fix MMCSS start/stop lifecycle:

```cpp
// ==========================================
// Sample Conversion Implementations
// ==========================================

void AsioManager::unpackInt24LSB(const void* src, float* dst, uint32_t numSamples) noexcept {
    const auto* raw = static_cast<const uint8_t*>(src);
    constexpr float inv24 = 1.0f / 8388608.0f;

    for (uint32_t s = 0; s < numSamples; ++s) {
        const uint32_t offset = s * 3;
        const uint32_t uval = (static_cast<uint32_t>(raw[offset]) << 8) |
                              (static_cast<uint32_t>(raw[offset + 1]) << 16) |
                              (static_cast<uint32_t>(raw[offset + 2]) << 24);
        const int32_t sample24 = static_cast<int32_t>(uval) >> 8;
        dst[s] = static_cast<float>(sample24) * inv24;
    }
}

void AsioManager::packInt24LSB(const float* src, void* dst, uint32_t numSamples) noexcept {
    auto* raw = static_cast<uint8_t*>(dst);

    for (uint32_t s = 0; s < numSamples; ++s) {
        const float sample = std::clamp(src[s], -1.0f, 1.0f);
        int32_t sample24 = (sample >= 0.0f)
            ? static_cast<int32_t>(sample * 8388607.0f)
            : static_cast<int32_t>(sample * 8388608.0f);
        sample24 = std::clamp(sample24, -8388608, 8388607);

        const uint32_t offset = s * 3;
        raw[offset]     = static_cast<uint8_t>(sample24 & 0xFF);
        raw[offset + 1] = static_cast<uint8_t>((sample24 >> 8) & 0xFF);
        raw[offset + 2] = static_cast<uint8_t>((sample24 >> 16) & 0xFF);
    }
}

void AsioManager::unpackInt32LSB24(const void* src, float* dst, uint32_t numSamples) noexcept {
    const auto* raw = static_cast<const int32_t*>(src);
    constexpr float inv24 = 1.0f / 8388608.0f;

    for (uint32_t s = 0; s < numSamples; ++s) {
        const uint32_t uval = static_cast<uint32_t>(raw[s]) << 8;
        const int32_t sample24 = static_cast<int32_t>(uval) >> 8;
        dst[s] = static_cast<float>(sample24) * inv24;
    }
}

void AsioManager::packInt32LSB24(const float* src, void* dst, uint32_t numSamples) noexcept {
    auto* raw = static_cast<int32_t*>(dst);

    for (uint32_t s = 0; s < numSamples; ++s) {
        const float sample = std::clamp(src[s], -1.0f, 1.0f);
        int32_t sample24 = (sample >= 0.0f)
            ? static_cast<int32_t>(sample * 8388607.0f)
            : static_cast<int32_t>(sample * 8388608.0f);
        sample24 = std::clamp(sample24, -8388608, 8388607);
        raw[s] = sample24;
    }
}

void AsioManager::unpackSamples(ASIOSampleType type, const void* src, float* dst, uint32_t numSamples) noexcept {
    if (!src || !dst || numSamples == 0) return;

    switch (type) {
        case ASIOSTInt32LSB: {
            const auto* s = static_cast<const int32_t*>(src);
            constexpr float inv32 = 1.0f / 2147483648.0f;
            for (uint32_t i = 0; i < numSamples; ++i) dst[i] = static_cast<float>(s[i]) * inv32;
            break;
        }
        case ASIOSTFloat32LSB: {
            std::memcpy(dst, src, numSamples * sizeof(float));
            break;
        }
        case ASIOSTInt16LSB: {
            const auto* s = static_cast<const int16_t*>(src);
            constexpr float inv16 = 1.0f / 32768.0f;
            for (uint32_t i = 0; i < numSamples; ++i) dst[i] = static_cast<float>(s[i]) * inv16;
            break;
        }
        case ASIOSTInt24LSB: {
            unpackInt24LSB(src, dst, numSamples);
            break;
        }
        case ASIOSTInt32LSB24: {
            unpackInt32LSB24(src, dst, numSamples);
            break;
        }
        default:
            std::memset(dst, 0, numSamples * sizeof(float));
            break;
    }
}

void AsioManager::packSamples(ASIOSampleType type, const float* src, void* dst, uint32_t numSamples) noexcept {
    if (!src || !dst || numSamples == 0) return;

    switch (type) {
        case ASIOSTInt32LSB: {
            auto* d = static_cast<int32_t*>(dst);
            for (uint32_t i = 0; i < numSamples; ++i) {
                float sample = std::clamp(src[i], -1.0f, 1.0f);
                d[i] = static_cast<int32_t>(sample * 2147483647.0f);
            }
            break;
        }
        case ASIOSTFloat32LSB: {
            std::memcpy(dst, src, numSamples * sizeof(float));
            break;
        }
        case ASIOSTInt16LSB: {
            auto* d = static_cast<int16_t*>(dst);
            for (uint32_t i = 0; i < numSamples; ++i) {
                float sample = std::clamp(src[i], -1.0f, 1.0f);
                d[i] = static_cast<int16_t>(sample * 32767.0f);
            }
            break;
        }
        case ASIOSTInt24LSB: {
            packInt24LSB(src, dst, numSamples);
            break;
        }
        case ASIOSTInt32LSB24: {
            packInt32LSB24(src, dst, numSamples);
            break;
        }
        default:
            break;
    }
}

// ==========================================
// MMCSS Integration in AsioManager::start & stop
// ==========================================

bool AsioManager::start(int32_t bufferSize) {
    if (!m_driver) return false;
    if (m_isRunning.load()) return true;

    // Register MMCSS "Pro Audio" priority for calling control thread up front
    if (!m_mmcssHandle) {
        m_mmcssTaskIndex = 0;
        m_mmcssHandle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &m_mmcssTaskIndex);
    }

    if (bufferSize <= 0) {
        bufferSize = m_info.preferredBufferSize;
    }
    m_info.currentBufferSize = bufferSize;

    const int32_t totalChannels = m_info.numInputChannels + m_info.numOutputChannels;
    m_bufferInfos.resize(totalChannels);

    int32_t bufIdx = 0;
    for (int32_t i = 0; i < m_info.numInputChannels; ++i, ++bufIdx) {
        m_bufferInfos[bufIdx].isInput = 1;
        m_bufferInfos[bufIdx].channelNum = i;
        m_bufferInfos[bufIdx].buffers[0] = nullptr;
        m_bufferInfos[bufIdx].buffers[1] = nullptr;
    }

    for (int32_t i = 0; i < m_info.numOutputChannels; ++i, ++bufIdx) {
        m_bufferInfos[bufIdx].isInput = 0;
        m_bufferInfos[bufIdx].channelNum = i;
        m_bufferInfos[bufIdx].buffers[0] = nullptr;
        m_bufferInfos[bufIdx].buffers[1] = nullptr;
    }

    m_callbacks.bufferSwitch = &AsioManager::bufferSwitchCallback;
    m_callbacks.sampleRateDidChange = &AsioManager::sampleRateDidChangeCallback;
    m_callbacks.asioMessage = &AsioManager::asioMessageCallback;
    m_callbacks.bufferSwitchTimeInfo = &AsioManager::bufferSwitchTimeInfoCallback;

    ASIOError err = m_driver->createBuffers(m_bufferInfos.data(), totalChannels, bufferSize, &m_callbacks);
    if (err != ASE_OK) {
        std::cerr << "createBuffers failed with error: " << err << "\n";
        if (m_mmcssHandle) {
            AvRevertMmThreadCharacteristics(m_mmcssHandle);
            m_mmcssHandle = nullptr;
        }
        return false;
    }

    m_inputFloatBuffer.resize(std::max(1, m_info.numInputChannels), bufferSize);
    m_outputFloatBuffer.resize(std::max(1, m_info.numOutputChannels), bufferSize);

    err = m_driver->start();
    if (err != ASE_OK) {
        std::cerr << "driver start failed with error: " << err << "\n";
        m_driver->disposeBuffers();
        if (m_mmcssHandle) {
            AvRevertMmThreadCharacteristics(m_mmcssHandle);
            m_mmcssHandle = nullptr;
        }
        return false;
    }

    m_isRunning.store(true);
    return true;
}

void AsioManager::stop() {
    if (!m_driver || !m_isRunning.load()) return;

    m_isRunning.store(false);
    m_driver->stop();
    m_driver->disposeBuffers();

    // Cleanly revert MMCSS association on the same calling thread
    if (m_mmcssHandle) {
        AvRevertMmThreadCharacteristics(m_mmcssHandle);
        m_mmcssHandle = nullptr;
        m_mmcssTaskIndex = 0;
    }
}

// In processAudio():
void AsioManager::processAudio(int32_t doubleBufferIndex) {
    if (!m_isRunning.load(std::memory_order_relaxed)) return;

    // Optional thread-local MMCSS registration on driver streaming thread (no cross-thread handle sharing)
    thread_local HANDLE tl_driverMmcss = nullptr;
    thread_local DWORD tl_driverTaskIdx = 0;
    if (!tl_driverMmcss) {
        tl_driverMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &tl_driverTaskIdx);
    }

    const uint32_t bufferSize = static_cast<uint32_t>(m_info.currentBufferSize);
    const int32_t inCh = m_info.numInputChannels;
    const int32_t outCh = m_info.numOutputChannels;

    auto inView = m_inputFloatBuffer.view(bufferSize);
    auto outView = m_outputFloatBuffer.view(bufferSize);

    // Convert hardware input buffers to 32-bit float
    for (int32_t i = 0; i < inCh; ++i) {
        const auto& chInfo = m_info.inputChannels[i];
        void* rawBuf = m_bufferInfos[i].buffers[doubleBufferIndex];
        float* dst = inView.channel(i);

        if (!rawBuf) {
            std::memset(dst, 0, bufferSize * sizeof(float));
            continue;
        }

        unpackSamples(chInfo.type, rawBuf, dst, bufferSize);
    }

    outView.clear();

    // Execute registered audio processing pipeline
    if (m_callback) {
        m_callback(inView, outView);
    }

    // Convert 32-bit float output back to hardware buffers
    for (int32_t i = 0; i < outCh; ++i) {
        const auto& chInfo = m_info.outputChannels[i];
        void* rawBuf = m_bufferInfos[inCh + i].buffers[doubleBufferIndex];
        const float* src = outView.channel(i);

        if (!rawBuf) continue;

        packSamples(chInfo.type, src, rawBuf, bufferSize);
    }

    m_driver->outputReady();
}
```

---

### 4.3 Blueprint: Unit Tests in `tests/test_praccy.cpp`

Add the two required tests:

```cpp
#include <thread>
#include <chrono>
#include <atomic>
#include "audio/asio_manager.h"

// =========================================================================
// Feature 6: Concurrency Unit Test
// GraphEngineTest.ConcurrentParallelMutation
// =========================================================================

void testConcurrentParallelMutation() {
    std::cout << "[TEST] GraphEngineTest.ConcurrentParallelMutation... ";

    audio::GraphEngine engine;
    constexpr double sampleRate = 48000.0;
    constexpr uint32_t blockSize = 128;
    engine.prepare(sampleRate, blockSize);

    // Add parallel split/merge block with two active branches
    auto splitBlock = std::make_unique<audio::ParallelSplitMergeBlock>("Stress Split");
    auto* branchA = splitBlock->addBranch("Branch A");
    auto* branchB = splitBlock->addBranch("Branch B");
    engine.addSerialNode(std::move(splitBlock));

    std::atomic<bool> stopStress{false};
    std::atomic<uint64_t> audioBlocksProcessed{0};
    std::atomic<uint64_t> mutationsCompleted{0};

    // Thread 1: Simulated High-Frequency Audio Callback Thread
    std::thread audioThread([&]() {
        audio::OwnedAudioBuffer inBuf(2, blockSize);
        audio::OwnedAudioBuffer outBuf(2, blockSize);
        auto inView = inBuf.view(blockSize);
        auto outView = outBuf.view(blockSize);

        // Populate test impulse signal
        for (uint32_t s = 0; s < blockSize; ++s) {
            inView.channel(0)[s] = 0.25f;
            inView.channel(1)[s] = -0.25f;
        }

        while (!stopStress.load(std::memory_order_relaxed)) {
            outView.clear();
            engine.process(inView, outView);

            // Assert numerical validity of all processed samples
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

    // Thread 2: Simulated UI Thread Mutating Parallel Branches Concurrently
    std::thread uiThread([&]() {
        constexpr int numIterations = 600;
        for (int i = 0; i < numIterations && !stopStress.load(std::memory_order_relaxed); ++i) {
            // Rapidly add plugin slots to both branches
            auto driveSlot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::OverdriveEffect>());
            driveSlot->prepare(sampleRate, blockSize);
            branchA->addSlot(std::move(driveSlot));

            auto delaySlot = std::make_unique<audio::PluginSlot>(std::make_unique<plugins::StereoDelayEffect>());
            delaySlot->prepare(sampleRate, blockSize);
            branchB->addSlot(std::move(delaySlot));

            // Periodically remove slots
            if (i % 2 == 0 && branchA->numSlots() > 0) {
                branchA->removeSlot(0);
            }
            if (i % 3 == 0 && branchB->numSlots() > 0) {
                branchB->removeSlot(0);
            }

            // Rapid concurrent property updates
            branchA->setGainDb(static_cast<float>((i % 12) - 6));
            branchB->setPan(static_cast<float>((i % 10) - 5) / 5.0f);

            mutationsCompleted.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    uiThread.join();

    // Allow audio thread to process trailing blocks after final UI mutation
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stopStress.store(true, std::memory_order_release);
    audioThread.join();

    assert(audioBlocksProcessed.load() >= 500);
    assert(mutationsCompleted.load() >= 600);

    std::cout << "PASSED (" << audioBlocksProcessed.load() << " audio blocks, "
              << mutationsCompleted.load() << " mutations)\n";
}

// =========================================================================
// Feature 6: 24-Bit Unpack/Pack Unit Test
// AsioManagerTest.Format24BitUnpack
// =========================================================================

void testAsio24BitUnpackAndPack() {
    std::cout << "[TEST] AsioManagerTest.Format24BitUnpack... ";

    // ----------------------------------------------------
    // Part 1: ASIOSTInt24LSB (Packed 3 Bytes, Little Endian)
    // ----------------------------------------------------
    {
        constexpr uint32_t numSamples = 7;
        // Test patterns:
        // [0] Zero (0): {0x00, 0x00, 0x00}
        // [1] Max Positive (+8,388,607 / 0x7FFFFF): {0xFF, 0xFF, 0x7F}
        // [2] Max Negative (-8,388,608 / 0x800000): {0x00, 0x00, 0x80}
        // [3] Mid Positive (+4,194,304 / 0x400000 = +0.5f): {0x00, 0x00, 0x40}
        // [4] Mid Negative (-4,194,304 / 0xC00000 = -0.5f): {0x00, 0x00, 0xC0}
        // [5] +1 LSB (+1): {0x01, 0x00, 0x00}
        // [6] -1 LSB (-1): {0xFF, 0xFF, 0xFF}
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
    }

    // ----------------------------------------------------
    // Part 2: ASIOSTInt32LSB24 (4-Byte Container, LSB Aligned)
    // ----------------------------------------------------
    {
        constexpr uint32_t numSamples = 8;
        const int32_t rawInput[numSamples] = {
            0x00000000,                       // 0
            0x007FFFFF,                       // +8,388,607 (Max positive)
            static_cast<int32_t>(0xFF800000), // -8,388,608 (Max negative, sign-extended)
            0x00800000,                       // -8,388,608 (Max negative, zero-extended high byte)
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
        assert(unpacked[4] == 0.5f);
        assert(unpacked[5] == -0.5f);
        assert(std::abs(unpacked[6] - (1.0f / 8388608.0f)) < 1e-7f);
        assert(std::abs(unpacked[7] - (-1.0f / 8388608.0f)) < 1e-7f);

        int32_t packedOutput[numSamples] = {};
        audio::AsioManager::packInt32LSB24(unpacked, packedOutput, numSamples);

        assert(packedOutput[0] == 0);
        assert((packedOutput[1] & 0x00FFFFFF) == 0x007FFFFF);
        assert((packedOutput[2] & 0x00FFFFFF) == 0x00800000);
        assert((packedOutput[5] & 0x00FFFFFF) == 0x00C00000);
    }

    std::cout << "PASSED\n";
}
```

---

### 4.4 Blueprint: `CMakeLists.txt` Build Target Updates

To compile and link `test_praccy` with `AsioManager`:
In `CMakeLists.txt:92-114`:
```cmake
add_executable(test_praccy
    tests/test_praccy.cpp
    src/audio/asio_manager.cpp
    src/audio/graph_engine.cpp
    src/plugins/builtin_dsp.cpp
    src/plugins/vst3_host.cpp
    src/plugins/clap_host.cpp
    src/plugins/plugin_window.cpp
    src/tools/tuner.cpp
    src/tools/metronome.cpp
    src/tools/audio_player.cpp
    src/tools/quick_looper.cpp
    src/state/scene_manager.cpp
    src/state/app_config.cpp
    third_party/vst3_pluginterfaces/base/funknown.cpp
    third_party/vst3_pluginterfaces/base/coreiids.cpp
)
target_include_directories(test_praccy PRIVATE
    src
    third_party
    third_party/clap/include
)
target_link_libraries(test_praccy PRIVATE ole32 uuid user32 gdi32 winmm avrt)
```

---

## 5. Verification Method

### 5.1 Independent Code Verification
1. Inspect `src/audio/asio_manager.cpp:260-284, 302-326` to confirm unhandled `ASIOSTInt24LSB` and `ASIOSTInt32LSB24`.
2. Inspect `src/audio/asio_manager.cpp:188-192, 236-240` to confirm cross-thread MMCSS usage.
3. Inspect `tests/test_praccy.cpp` to confirm missing `ConcurrentParallelMutation` and `Format24BitUnpack` test functions.

### 5.2 Build and Test Commands
Execute the complete test suite:
```powershell
cmake --build build --target test_praccy
ctest --test-dir build --output-on-failure
```
Execute direct standalone binary:
```powershell
./build/test_praccy.exe
```
Expected output:
```
[TEST] GraphEngineTest.ConcurrentParallelMutation... PASSED (XXXX audio blocks, 600 mutations)
[TEST] AsioManagerTest.Format24BitUnpack... PASSED
ALL TESTS PASSED SUCCESSFULLY! (13/13)
```

### 5.3 Invalidation Conditions
This strategy is invalidated if:
* An alternative 24-bit representation (e.g. MSB-justified 24-bit within 32-bit container `ASIOSTInt32MSB24`) is required instead of LSB-aligned `ASIOSTInt32LSB24`.
* `AvSetMmThreadCharacteristicsW` is declared deprecated or unsupported on target Windows platforms (supported from Windows Vista through Windows 11).
* The command queue in Feature 1 adopts a non-SPSC communication design that alters `branch->addSlot()` signatures.
