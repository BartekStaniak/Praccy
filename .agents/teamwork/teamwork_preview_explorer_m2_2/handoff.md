# Implementation Blueprint: Win32 SEH/VEH Plugin Crash Isolation (Feature 9)

**Milestone**: Milestone 2 — SecOps, Crash Isolation & Hardening  
**Author**: Explorer Subagent (`teamwork_preview_explorer_m2_2`)  
**Target Feature**: Feature 9 (`Win32 SEH/VEH Plugin Crash Isolation`)  
**Project Root**: `f:/Projects/Praccy`  
**Date**: 2026-10-06T20:28:00Z  

---

## 1. Observation

### 1.1 Unprotected Plugin Invocation Boundaries
An audit of third-party plugin hosting code in `src/plugins/` revealed that all invocations across the VST3 and CLAP boundaries are executed directly without exception handling or hardware fault isolation:

1. **VST3 Audio Processing (`src/plugins/vst3_host.cpp:346-372`)**:
   ```cpp
   void Vst3PluginInstance::process(audio::AudioProcessContext& ctx) {
       ...
       m_processor->process(data);
   }
   ```
   `m_processor->process(data)` executes untrusted foreign machine code directly on the real-time ASIO audio callback thread. Any null pointer dereference, wild pointer write, or buffer overflow inside the plugin immediately triggers an unhandled Win32 hardware exception (`EXCEPTION_ACCESS_VIOLATION`, `0xC0000005`), terminating the Praccy host process instantly.

2. **VST3 GUI Invocations (`src/plugins/vst3_host.cpp:425-484`)**:
   - Line 441: `m_plugView->attached(reinterpret_cast<void*>(parentHwnd), Steinberg::kPlatformTypeHWND)`
   - Line 460: `view->getSize(&rect)`
   - Line 478: `m_plugView->removed()`
   - Line 479: `m_plugView->release()`
   These Win32 window creation and teardown calls run unshielded on the main UI thread.

3. **VST3 Control and State Invocations (`src/plugins/vst3_host.cpp:408-506`)**:
   - Line 410: `m_controller->setParamNormalized(paramId, std::clamp(value, 0.0f, 1.0f))`
   - Line 489: `m_component->getState(&stream)`
   - Line 498: `m_component->setState(&stream)`
   - Line 501: `m_controller->setComponentState(&stream)`
   All parameter changes and preset loading/saving calls are unshielded.

4. **CLAP Audio Processing (`src/plugins/clap_host.cpp:153-165`)**:
   ```cpp
   void ClapPluginInstance::process(audio::AudioProcessContext& ctx) {
       ...
       m_plugin->process(m_plugin, &processData);
   }
   ```
   Direct unshielded execution on the audio thread.

5. **CLAP GUI & State Invocations (`src/plugins/clap_host.cpp:217-260`)**:
   - Lines 225-240: `m_guiExt->create()`, `m_guiExt->set_parent()`, `m_guiExt->show()`
   - Lines 245-246: `m_guiExt->hide()`, `m_guiExt->destroy()`
   - Line 255: `m_guiExt->get_size()`
   - Lines 175, 184, 191, 206: `m_paramsExt->count()`, `get_info()`, `get_value()`

### 1.2 Absence of Faulted State Tracking and Pass-Through Handling
1. **`IPluginInstance` Interface (`src/plugins/plugin_base.h:19-46`)**:
   - `IPluginInstance` does not declare any mechanism to query or store whether a plugin has encountered a fatal crash (`isFaulted()`).
   - There is no atomic fault flag, no fault reason string, and no reset mechanism.
2. **`PluginSlot::process()` (`src/audio/graph_engine.cpp:44-105`)**:
   - Lines 71-78 create `AudioProcessContext innerCtx` and call `m_innerNode->process(innerCtx)`.
   - If a plugin crashes or fails, `PluginSlot` continues running, applying gains and crossfades to potentially corrupted or unwritten buffers.
3. **UI Representation (`src/ui/rack_view.cpp:1217-1510`)**:
   - `RackView::renderPluginSlot` checks `slot->isBypassed()`, but has no awareness of a crashed/faulted plugin.
   - If a plugin crashes, the host either terminates or, if partially bypassed, provides no visual indicator that the plugin has crashed and no way to reload or reset it.

### 1.3 Compiler Diagnostics and Platform Nuances
1. **MSVC Compiler Error C2712**:
   When using Microsoft's structured exception handling (`__try` / `__except`), MSVC enforces:
   ```
   error C2712: Cannot use __try in functions that require object unwinding
   ```
   This error occurs whenever a function containing `__try` instantiates any local C++ object with a non-trivial destructor, takes parameters with destructors, or contains C++ lambda captures. To compile cleanly under MSVC with `/W4 /WX`, the `__try` / `__except` construct must be strictly confined to a leaf function that contains zero objects requiring C++ unwinding.
2. **MinGW GCC Compiler Compatibility**:
   Compiling `__try` / `__except` syntax under MinGW-w64 GCC 16.2.0 yields:
   ```
   error: expected 'catch' before '__except'
   ```
   GCC's C++ frontend does not implement Microsoft `__try` / `__except` extensions.
3. **Win32 Vectored Exception Handling (VEH) Verification**:
   Empirical verification using `g++.exe -std=c++20` confirmed that registering a Win32 Vectored Exception Handler via `AddVectoredExceptionHandler(1, vehExceptionHandler)` combined with `setjmp` / `longjmp` and a thread-local context stack successfully catches Access Violations (`0xC0000005`), restores execution safely, allows consecutive fault handling without state corruption, supports nested protection scopes, and lets standard C++ exceptions propagate cleanly.

---

## 2. Logic Chain

### 2.1 Dual-Compiler Portability & C2712 Elimination
1. *Observation 1.3* shows that MSVC forbids `__try` in any function with C++ object unwinding (C2712), while MinGW GCC rejects `__try` / `__except` completely.
2. Therefore, `src/plugins/crash_isolation.h` must provide a portable abstraction with dual implementations selected at compile-time via `#if defined(_MSC_VER)`:
   - **For MSVC (`_MSC_VER`)**:
     - Isolate `__try` / `__except` into a non-template leaf function `sehExecuteLeaf(void (*fn)(void*), void* arg, DWORD* outCode)`.
     - The parameters are raw C function pointers and primitive pointers. It declares zero local variables with destructors.
     - Because it has no C++ objects, MSVC compiler error C2712 is mathematically impossible.
     - The exception filter expression `filterSehException` checks `GetExceptionCode()`:
       - If `code == 0xE06D7363` (MSVC C++ exception), it returns `EXCEPTION_CONTINUE_SEARCH` so standard C++ `try`/`catch` blocks retain normal behavior.
       - If `isCrashException(code)` is true (`0xC0000005`, `0x80000002`, `0xC000001D`, etc.), it saves the code and returns `EXCEPTION_EXECUTE_HANDLER`.
     - Outer C++ template wrappers (`safeCallPluginAudio`, `safeCallPluginGui`) pass a lightweight lambda trampoline to `sehExecuteLeaf`. The template wrapper itself contains NO `__try`, avoiding C2712 entirely.
   - **For MinGW GCC (`!defined(_MSC_VER)`)**:
     - Use Win32 Vectored Exception Handling (`AddVectoredExceptionHandler`).
     - A process-global VEH handler `vehExceptionHandler` intercepts hardware crash exceptions before standard OS unhandled exception dispatch.
     - A `thread_local` pointer `t_currentCrashContext` tracks an intrusive linked stack of `CrashContext` structures (`jmp_buf`, `prev` pointer, `exceptionCode`, `faultAddress`).
     - When `safeCallPluginAudio` is entered:
       1. `ctx.prev = t_currentCrashContext; t_currentCrashContext = &ctx;`
       2. `if (setjmp(ctx.jmpBuf) == 0)`: execute the guarded function.
       3. If a crash occurs: the VEH handler sees `t_currentCrashContext != nullptr`, records the exception code, and calls `longjmp(ctx->jmpBuf, 1)`.
       4. The `else` branch executes, pops the context (`t_currentCrashContext = ctx.prev;`), and returns `false`.
       5. If a normal C++ exception is thrown inside the guarded code, a C++ `try`/`catch(...)` pops `t_currentCrashContext` and re-throws, preserving C++ exception safety.

### 2.2 Real-Time Audio Engine Concurrency & Zero-Allocation Safety
1. In high-performance ASIO streaming, `process()` is invoked at high frequencies (e.g., 187.5 times per second at 256 samples / 48 kHz).
2. Calling `AddVectoredExceptionHandler` or `RemoveVectoredExceptionHandler` on every audio block would acquire internal process locks in `ntdll.dll`, causing priority inversion and audio buffer dropouts.
3. Therefore:
   - The VEH handler is registered **once** globally during initialization (`initCrashIsolation()`), using `std::call_once`.
   - When no exception occurs, the audio thread overhead consists solely of initializing a stack-allocated POD struct, updating an intrusive thread-local pointer, and executing `setjmp`.
   - On x86_64, `setjmp` takes ~15 CPU instructions (<10 nanoseconds), requires zero heap allocations, acquires zero mutexes, and is 100% real-time safe.
   - Because each thread maintains its own independent `t_currentCrashContext`, the ASIO audio callback thread and the Win32 GUI thread never contend or block each other.

### 2.3 Fault State Latching, Dry Pass-Through & Fail-Safe Audio
1. *Observation 1.1 & 1.2* show that when an access violation occurs, the plugin's internal state is corrupted. Invoking `process()` on that plugin in subsequent audio blocks will trigger repeated exceptions, burning CPU cycles and potentially corrupting the audio pipeline.
2. Therefore:
   - `IPluginInstance` maintains an atomic latch: `std::atomic<bool> m_faulted{false}`.
   - The first time an exception is trapped by `safeCallPluginAudio`:
     1. `m_faulted.store(true, std::memory_order_release);`
     2. `m_faultReason = "Hardware Access Violation (0xC0000005) in process()";`
     3. Immediate fallback: `ctx.output.copyFrom(ctx.input);`
   - On all subsequent blocks, `process()` checks `if (m_faulted.load(std::memory_order_relaxed))` at the very entry and instantly executes `ctx.output.copyFrom(ctx.input)`, bypassing the plugin with zero DSP overhead.
   - In `PluginSlot::process()`, checking `innerPlugin->isFaulted()` ensures that even input gain, wet/dry crossfades, and temporary scratch buffers are skipped, guaranteeing pure, clean pass-through audio.

### 2.4 User Interface Alerting & Recovery
1. When a plugin crashes, the host must notify the musician immediately without interrupting audio playback or terminating the app.
2. In `RackView::renderPluginSlot`:
   - Detect `isFaulted = pluginInst && pluginInst->isFaulted()`.
   - If faulted:
     - Render card with dark crimson background tint (`ImVec4(0.24f, 0.10f, 0.10f, 1.0f)`) and a red border glow (`IM_COL32(235, 60, 60, 255)`).
     - Display a prominent format badge: `[CRASH ISOLATED]`.
     - In the preview area, display: `"PLUGIN FAULTED - ACCESS VIOLATION ISOLATED"`, `"Dry audio pass-through active"`, and a `[RELOAD PLUGIN]` button.
     - If the plugin window was open, call `PluginWindowManager::instance().closePluginWindow(pluginInst)` to prevent invalid HWND message dispatch from destabilizing the host.
     - When the user clicks `[RELOAD PLUGIN]`, Praccy resets the fault state, re-instantiates or re-prepares the plugin, giving the user control.

### 2.5 Deterministic Unit Testing
1. *Observation 1.2* notes the lack of unit tests for crash isolation.
2. To verify the crash isolation engine:
   - Implement `CrashingMockPlugin` derived from `IPluginInstance` in `tests/test_praccy.cpp`.
   - The mock plugin provides a toggle `triggerCrash()`. When false, it doubles input signal (`output = input * 2.0f`). When true, it writes to invalid address `0xDEADBEEF`.
   - In `test_praccy.cpp`, `testPluginCrashIsolation()` executes 4 distinct assertions:
     1. **Baseline Audio Processing**: Normal DSP operates cleanly (`isFaulted() == false`, output is amplified).
     2. **Isolated Access Violation**: Access violation is trapped; test asserts `isFaulted() == true`, output buffer is identical to input buffer (dry pass-through), host process remains alive.
     3. **Continuous Dry Bypass**: Subsequent audio blocks pass dry signal cleanly with zero exceptions.
     4. **GraphEngine Full Pipeline Integration**: Pumping 100 blocks through `GraphEngine` with a crashing plugin proves the entire audio chain remains stable and glitch-free.

---

## 3. Caveats

1. **Process Heap Corruption vs Memory Isolation**:
   - Win32 SEH and VEH isolate hardware execution exceptions (null pointer dereference, misaligned memory access, invalid page writes).
   - If a misbehaving plugin scribbles random garbage onto the process heap *before* hitting an unmapped memory page and triggering an access violation, SEH/VEH cannot undo the memory write.
   - However, permanently latching `m_faulted = true` ensures that the corrupted plugin code is never executed again, minimizing cascading damage.
2. **Stack Overflow Handling (`EXCEPTION_STACK_OVERFLOW`, `0xC00000FD`)**:
   - When a plugin exhausts thread stack space (e.g., infinite recursion), Windows removes the stack guard page.
   - Catching `0xC00000FD` requires calling `_resetstkoflw()` to restore the guard page before any further deep function calls can be made on that thread.
3. **Floating-Point Status & SIMD Control Register (MXCSR)**:
   - A crashing DSP plugin may leave the SSE control register (`MXCSR`) in an abnormal state (e.g., with floating-point exception masks disabled or denormals-are-zero flags cleared).
   - Upon trapping an exception in `safeCallPluginAudio`, Praccy should restore standard audio MXCSR settings (`_mm_setcsr(_MM_MASK_MASK | _MM_FLUSHTOZERO_ON | _MM_DENORMALS_ZERO_ON)`).
4. **Third-Party Win32 Window Procedure Callbacks**:
   - When a plugin creates an embedded HWND, Windows dispatches window messages directly to the plugin's internal `WndProc` via `DispatchMessageW`.
   - While `openGui()` and `closeGui()` are protected by `safeCallPluginGui()`, if the plugin's own window procedure faults during a mouse move or paint event, Praccy's top-level message loop or window procedure must also handle or immediately close the plugin window via `PluginWindowManager`.

---

## 4. Conclusion

### 4.1 Specification: `src/plugins/crash_isolation.h`
Create `src/plugins/crash_isolation.h` with portable MSVC (`__try`/`__except` leaf function) and MinGW GCC (`AddVectoredExceptionHandler` + `setjmp`/`longjmp`) support:

```cpp
#pragma once

#include <windows.h>
#include <cstdint>
#include <csetjmp>
#include <type_traits>
#include <atomic>
#include <mutex>
#include <string>

#if defined(_MSC_VER)
#include <xmmintrin.h>
#include <pmmintrin.h>
#else
#include <x86intrin.h>
#endif

namespace praccy::plugins {

inline bool isCrashException(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_FLT_DENORMAL_OPERAND:
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INEXACT_RESULT:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_STACK_CHECK:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
    }
}

inline const char* getExceptionDescription(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:        return "Access Violation (0xC0000005)";
        case EXCEPTION_DATATYPE_MISALIGNMENT:   return "Datatype Misalignment (0x80000002)";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "Array Bounds Exceeded (0xC000008C)";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "Floating Point Divide by Zero (0xC000008E)";
        case EXCEPTION_FLT_OVERFLOW:            return "Floating Point Overflow (0xC0000091)";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "Integer Divide by Zero (0xC0000094)";
        case EXCEPTION_ILLEGAL_INSTRUCTION:     return "Illegal CPU Instruction (0xC000001D)";
        case EXCEPTION_IN_PAGE_ERROR:           return "In-Page Memory Error (0xC0000006)";
        case EXCEPTION_STACK_OVERFLOW:          return "Stack Overflow (0xC00000FD)";
        default:                                return "Unknown Hardware Exception";
    }
}

namespace detail {

#if defined(_MSC_VER)

inline int filterSehException(DWORD code, DWORD* outExceptionCode) noexcept {
    if (outExceptionCode) {
        *outExceptionCode = code;
    }
    // Do not intercept MSVC C++ exceptions (0xE06D7363)
    if (code == 0xE06D7363) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (isCrashException(code)) {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Leaf function: NO local variables with destructors, complying with MSVC C2712
inline bool sehLeafInvoke(void (*fn)(void*), void* ctx, DWORD* outExceptionCode) {
    __try {
        fn(ctx);
        return true;
    }
    __except (detail::filterSehException(GetExceptionCode(), outExceptionCode)) {
        if (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW) {
            _resetstkoflw();
        }
        return false;
    }
}

#else

struct CrashContext {
    jmp_buf jmpBuf;
    CrashContext* prev{nullptr};
    DWORD exceptionCode{0};
    void* faultAddress{nullptr};
};

inline thread_local CrashContext* t_currentCrashContext = nullptr;
inline PVOID g_vehHandle = nullptr;
inline std::once_flag g_vehInitOnce;

inline LONG WINAPI vehExceptionHandler(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CrashContext* ctx = t_currentCrashContext;
    if (!ctx) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (isCrashException(code)) {
        ctx->exceptionCode = code;
        ctx->faultAddress = ep->ExceptionRecord->ExceptionAddress;
        if (code == EXCEPTION_STACK_OVERFLOW) {
            _resetstkoflw();
        }
        longjmp(ctx->jmpBuf, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

inline void ensureVehRegistered() {
    std::call_once(g_vehInitOnce, []() {
        g_vehHandle = AddVectoredExceptionHandler(1, vehExceptionHandler);
    });
}

inline bool vehLeafInvoke(void (*fn)(void*), void* ctxArg, DWORD* outExceptionCode) {
    ensureVehRegistered();
    CrashContext ctx;
    ctx.prev = t_currentCrashContext;
    ctx.exceptionCode = 0;
    ctx.faultAddress = nullptr;
    t_currentCrashContext = &ctx;

    if (setjmp(ctx.jmpBuf) == 0) {
        try {
            fn(ctxArg);
            t_currentCrashContext = ctx.prev;
            return true;
        } catch (...) {
            t_currentCrashContext = ctx.prev;
            throw;
        }
    } else {
        t_currentCrashContext = ctx.prev;
        if (outExceptionCode) {
            *outExceptionCode = ctx.exceptionCode;
        }
        return false;
    }
}

#endif

} // namespace detail

inline void initCrashIsolation() {
#if !defined(_MSC_VER)
    detail::ensureVehRegistered();
#endif
}

template <typename Func>
inline bool safeCallPlugin(Func&& f, DWORD* outExceptionCode = nullptr) {
    auto trampoline = [](void* context) {
        auto* fn = static_cast<std::remove_reference_t<Func>*>(context);
        (*fn)();
    };
#if defined(_MSC_VER)
    return detail::sehLeafInvoke(trampoline, const_cast<void*>(static_cast<const void*>(&f)), outExceptionCode);
#else
    return detail::vehLeafInvoke(trampoline, const_cast<void*>(static_cast<const void*>(&f)), outExceptionCode);
#endif
}

template <typename Func>
inline bool safeCallPluginAudio(Func&& f, DWORD* outExceptionCode = nullptr) {
    bool ok = safeCallPlugin(std::forward<Func>(f), outExceptionCode);
    if (!ok) {
        // Restore standard audio MXCSR control word after fault
        _mm_setcsr(0x1F80 | 0x8000 | 0x0040); // Standard masks + FTZ + DAZ
    }
    return ok;
}

template <typename Func>
inline bool safeCallPluginGui(Func&& f, DWORD* outExceptionCode = nullptr) {
    return safeCallPlugin(std::forward<Func>(f), outExceptionCode);
}

} // namespace praccy::plugins
```

---

### 4.2 Integration: `src/plugins/plugin_base.h`
Add faulted state tracking to `IPluginInstance`:

```cpp
// In src/plugins/plugin_base.h:
class IPluginInstance : public audio::AudioNode {
public:
    virtual ~IPluginInstance() = default;

    // Fault state tracking
    [[nodiscard]] virtual bool isFaulted() const noexcept {
        return m_faulted.load(std::memory_order_acquire);
    }
    virtual void setFaulted(bool faulted) noexcept {
        m_faulted.store(faulted, std::memory_order_release);
    }
    [[nodiscard]] virtual const std::string& faultReason() const noexcept {
        return m_faultReason;
    }
    virtual void setFaultReason(std::string reason) {
        m_faultReason = std::move(reason);
    }
    virtual void resetFault() noexcept {
        m_faulted.store(false, std::memory_order_release);
        m_faultReason.clear();
    }
    ...
protected:
    std::atomic<bool> m_faulted{false};
    std::string m_faultReason;
};
```

---

### 4.3 Integration: `src/plugins/vst3_host.cpp` & `clap_host.cpp`

#### 1. In `src/plugins/vst3_host.cpp`:
Include `"crash_isolation.h"` and wrap all calls:

- **`process()`**:
  ```cpp
  void Vst3PluginInstance::process(audio::AudioProcessContext& ctx) {
      if (!m_processor || ctx.numSamples == 0 || m_faulted.load(std::memory_order_relaxed)) {
          ctx.output.copyFrom(ctx.input);
          return;
      }

      const uint32_t numSamples = ctx.numSamples;
      Steinberg::Vst::ProcessData data{};
      data.processMode = Steinberg::Vst::kRealtime;
      data.symbolicSampleSize = Steinberg::Vst::kSample32;
      data.numSamples = numSamples;

      Steinberg::Vst::AudioBusBuffers inBus{};
      inBus.numChannels = ctx.input.numChannels();
      inBus.channelBuffers32 = const_cast<float**>(ctx.input.rawChannels());

      Steinberg::Vst::AudioBusBuffers outBus{};
      outBus.numChannels = ctx.output.numChannels();
      outBus.channelBuffers32 = const_cast<float**>(ctx.output.rawChannels());

      data.numInputs = 1;
      data.inputs = &inBus;
      data.numOutputs = 1;
      data.outputs = &outBus;

      DWORD exCode = 0;
      bool ok = safeCallPluginAudio([&]() {
          m_processor->process(data);
      }, &exCode);

      if (!ok) {
          m_faulted.store(true, std::memory_order_release);
          m_faultReason = std::string("VST3 crash: ") + getExceptionDescription(exCode);
          ctx.output.copyFrom(ctx.input);
      }
  }
  ```

- **`openGui()`**:
  ```cpp
  bool Vst3PluginInstance::openGui(HWND parentHwnd) {
      if (m_faulted.load(std::memory_order_acquire)) return false;
      if (!m_controller || !parentHwnd) return false;

      DWORD exCode = 0;
      bool ok = safeCallPluginGui([&]() {
          if (!m_plugView) {
              m_plugView = m_controller->createView(Steinberg::Vst::ViewType::kEditor);
          }
          if (m_plugView && m_plugView->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) == Steinberg::kResultOk) {
              m_impl->plugFrame.parentHwnd = parentHwnd;
              m_plugView->setFrame(&m_impl->plugFrame);
              if (m_plugView->attached(reinterpret_cast<void*>(parentHwnd), Steinberg::kPlatformTypeHWND) == Steinberg::kResultOk) {
                  m_guiParentHwnd = parentHwnd;
              } else {
                  m_plugView->setFrame(nullptr);
              }
          }
      }, &exCode);

      if (!ok) {
          m_faulted.store(true, std::memory_order_release);
          m_faultReason = std::string("VST3 GUI crash: ") + getExceptionDescription(exCode);
          return false;
      }
      return m_guiParentHwnd != nullptr;
  }
  ```

- **`closeGui()`**:
  ```cpp
  void Vst3PluginInstance::closeGui() {
      if (m_plugView) {
          safeCallPluginGui([&]() {
              m_plugView->setFrame(nullptr);
              m_plugView->removed();
              m_plugView->release();
          });
          m_plugView = nullptr;
          m_guiParentHwnd = nullptr;
          m_impl->plugFrame.parentHwnd = nullptr;
      }
  }
  ```

- **`setParameterValue()` & `getParameterValue()`**:
  ```cpp
  void Vst3PluginInstance::setParameterValue(uint32_t paramId, float value) {
      if (m_controller && !m_faulted.load(std::memory_order_relaxed)) {
          safeCallPlugin([&]() {
              m_controller->setParamNormalized(paramId, std::clamp(value, 0.0f, 1.0f));
          });
      }
  }

  float Vst3PluginInstance::getParameterValue(uint32_t paramId) const {
      if (!m_controller || m_faulted.load(std::memory_order_relaxed)) return 0.0f;
      float val = 0.0f;
      safeCallPlugin([&]() {
          val = static_cast<float>(m_controller->getParamNormalized(paramId));
      });
      return val;
  }
  ```

- **`saveState()` & `loadState()`**:
  Wrap `m_component->getState()` and `m_component->setState()` inside `safeCallPlugin`.

#### 2. In `src/plugins/clap_host.cpp`:
Include `"crash_isolation.h"` and wrap all calls:

- **`process()`**:
  ```cpp
  void ClapPluginInstance::process(audio::AudioProcessContext& ctx) {
      if (!m_plugin || ctx.numSamples == 0 || m_faulted.load(std::memory_order_relaxed)) {
          ctx.output.copyFrom(ctx.input);
          return;
      }

      ...
      DWORD exCode = 0;
      bool ok = safeCallPluginAudio([&]() {
          m_plugin->process(m_plugin, &processData);
      }, &exCode);

      if (!ok) {
          m_faulted.store(true, std::memory_order_release);
          m_faultReason = std::string("CLAP crash: ") + getExceptionDescription(exCode);
          ctx.output.copyFrom(ctx.input);
      }
  }
  ```

- **`openGui()` & `closeGui()`**:
  Wrap `m_guiExt->create()`, `set_parent()`, `show()`, `hide()`, `destroy()` in `safeCallPluginGui`.

---

### 4.4 Audio Pass-Through in `PluginSlot` (`src/audio/graph_engine.cpp`)
At the start of `PluginSlot::process(AudioProcessContext& ctx)`:
```cpp
void PluginSlot::process(AudioProcessContext& ctx) {
    const uint32_t numSamples = ctx.numSamples;
    if (!m_innerNode || numSamples == 0) {
        ctx.output.copyFrom(ctx.input);
        return;
    }

    // Fast fail-safe bypass if inner plugin has faulted
    auto* plugInst = dynamic_cast<plugins::IPluginInstance*>(m_innerNode.get());
    if (plugInst && plugInst->isFaulted()) {
        ctx.output.copyFrom(ctx.input);
        m_meter.process(ctx.output.channel(0), ctx.output.numChannels() > 1 ? ctx.output.channel(1) : nullptr, numSamples);
        return;
    }
    ...
```

---

### 4.5 User Interface Alerting (`src/ui/rack_view.cpp`)
In `RackView::renderPluginSlot`:
```cpp
    auto* pluginInst = dynamic_cast<plugins::IPluginInstance*>(slot->innerNode());
    const bool isFaulted = pluginInst && pluginInst->isFaulted();

    // Visual card styling for faulted plugin
    ImVec4 cardBg = isFaulted ? ImVec4(0.22f, 0.10f, 0.10f, 1.0f) :
                    (bypassed ? ImVec4(0.12f, 0.13f, 0.16f, 0.95f) : ImVec4(0.16f, 0.18f, 0.23f, 1.0f));

    // Badge indicator on card header
    if (isFaulted) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[CRASH ISOLATED]");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Fault reason: %s\nDry audio pass-through is active.", pluginInst->faultReason().c_str());
        }
    }

    // Faceplate preview area alert
    if (isFaulted) {
        dl->AddRectFilled(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH),
                          IM_COL32(50, 15, 15, 230), 5.0f);
        dl->AddRect(previewPos, ImVec2(previewPos.x + previewW, previewPos.y + previewH),
                    IM_COL32(230, 60, 60, 255), 5.0f, 0, 1.5f);

        const char* crashTitle = "PLUGIN CRASH ISOLATED";
        ImVec2 tSz = ImGui::CalcTextSize(crashTitle);
        dl->AddText(ImVec2(previewPos.x + (previewW - tSz.x) * 0.5f, previewPos.y + 25.0f),
                    IM_COL32(255, 80, 80, 255), crashTitle);

        const char* crashSub = "Dry signal passed through cleanly";
        ImVec2 sSz = ImGui::CalcTextSize(crashSub);
        dl->AddText(ImVec2(previewPos.x + (previewW - sSz.x) * 0.5f, previewPos.y + 48.0f),
                    IM_COL32(200, 200, 210, 200), crashSub);

        // Interactive Reload button
        ImGui::SetCursorScreenPos(ImVec2(previewPos.x + (previewW - 100.0f) * 0.5f, previewPos.y + 70.0f));
        char reloadBtnId[32];
        std::snprintf(reloadBtnId, sizeof(reloadBtnId), "Reload##%d", slotIndex);
        if (ImGui::Button(reloadBtnId, ImVec2(100.0f, 22.0f))) {
            pluginInst->resetFault();
        }
    }
```

---

### 4.6 Unit Test Specification (`tests/test_praccy.cpp`)
Add `testPluginCrashIsolation()` to `tests/test_praccy.cpp`:

```cpp
#include "plugins/crash_isolation.h"
#include "plugins/plugin_base.h"

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
                // Deliberately dereference null pointer to simulate access violation
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
            m_faultReason = plugins::getExceptionDescription(exCode);
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
            m_faultReason = plugins::getExceptionDescription(exCode);
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
    assert(mockPlugin->faultReason().find("Access Violation") != std::string::npos);
    // Output must equal input (0.5f), not 1.0f and not garbage
    assert(std::abs(outView.channel(0)[0] - 0.5f) < 1e-5f);
    assert(std::abs(outView.channel(1)[64] - 0.5f) < 1e-5f);

    // Subsequent process calls: must remain safely bypassed
    outView.clear();
    mockPlugin->process(ctx);
    assert(std::abs(outView.channel(0)[0] - 0.5f) < 1e-5f);

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

    // Process blocks before crash
    for (int i = 0; i < 5; ++i) {
        engine.process(inView, outView);
        assert(std::abs(outView.channel(0)[0] - 1.0f) < 1e-4f);
    }

    // Trigger crash in plugin slot inside GraphEngine
    rawPluginPtr->armCrash();
    engine.process(inView, outView);
    assert(rawPluginPtr->isFaulted());
    // Audio bypassed cleanly to dry signal
    assert(std::abs(outView.channel(0)[0] - 0.5f) < 1e-4f);

    // Pump 20 more blocks through GraphEngine with faulted slot
    for (int i = 0; i < 20; ++i) {
        engine.process(inView, outView);
        assert(std::abs(outView.channel(0)[0] - 0.5f) < 1e-4f);
    }

    std::cout << "PASSED\n";
}
```

---

## 5. Verification Method

### 5.1 Verification Commands
1. **Compilation of Test Suite**:
   ```powershell
   & "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build test_praccy
   ```
   *Expected Outcome*: Successful build with exit code 0, zero errors.
2. **Execution of Automated Regression Suite**:
   ```powershell
   f:\Projects\Praccy\build\test_praccy.exe
   ```
   *Expected Outcome*: All 15 tests pass with exit code 0, explicitly displaying:
   `[TEST] Win32 SEH/VEH Plugin Crash Isolation & Dry Bypass... PASSED`
3. **Application Build**:
   ```powershell
   & "C:\Users\Bartek\w64devkit\bin\make.exe" -C f:\Projects\Praccy\build Praccy
   ```
   *Expected Outcome*: Successful build with exit code 0.

### 5.2 Verification Checklist & Invalidation Conditions
The blueprint is invalidated if any of the following occur:
1. `test_praccy.exe` terminates with exit code `0xC0000005` when a plugin triggers an access violation.
2. MSVC compiler emits error `C2712` when compiling `crash_isolation.h`.
3. MinGW GCC emits compilation errors on `safeCallPluginAudio`.
4. Trapping an access violation outputs silence or corrupted memory instead of bit-exact dry audio (`ctx.output.copyFrom(ctx.input)`).
5. Calling `safeCallPluginAudio` introduces dynamic memory allocations or NTDLL lock contention during normal audio processing.
