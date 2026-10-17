#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <csetjmp>
#include <type_traits>
#include <atomic>
#include <mutex>
#include <string>
#include <malloc.h>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>
#include <pmmintrin.h>
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
        case EXCEPTION_FLT_DENORMAL_OPERAND:    return "Floating Point Denormal Operand (0xC000008D)";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:      return "Floating Point Divide by Zero (0xC000008E)";
        case EXCEPTION_FLT_INEXACT_RESULT:      return "Floating Point Inexact Result (0xC000008F)";
        case EXCEPTION_FLT_INVALID_OPERATION:   return "Floating Point Invalid Operation (0xC0000090)";
        case EXCEPTION_FLT_OVERFLOW:            return "Floating Point Overflow (0xC0000091)";
        case EXCEPTION_FLT_STACK_CHECK:         return "Floating Point Stack Check (0xC0000092)";
        case EXCEPTION_FLT_UNDERFLOW:           return "Floating Point Underflow (0xC0000093)";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "Integer Divide by Zero (0xC0000094)";
        case EXCEPTION_INT_OVERFLOW:            return "Integer Overflow (0xC0000095)";
        case EXCEPTION_PRIV_INSTRUCTION:        return "Privileged Instruction (0xC0000096)";
        case EXCEPTION_IN_PAGE_ERROR:           return "In-Page Memory Error (0xC0000006)";
        case EXCEPTION_ILLEGAL_INSTRUCTION:     return "Illegal CPU Instruction (0xC000001D)";
        case EXCEPTION_STACK_OVERFLOW:          return "Stack Overflow (0xC00000FD)";
        default:                                return "Unknown Hardware Exception";
    }
}

// Zero-allocation fault telemetry formatting helper (F-08)
inline void formatFaultReason(char* buf, size_t bufSize, const char* prefix, uint32_t code) noexcept {
    if (!buf || bufSize == 0) return;
    const char* p = prefix ? prefix : "";
    const char* desc = getExceptionDescription(static_cast<DWORD>(code));
    if (std::strcmp(desc, "Unknown Hardware Exception") == 0) {
        std::snprintf(buf, bufSize, "%sUnknown Hardware Exception (0x%08lX)", p, static_cast<unsigned long>(code));
    } else {
        std::snprintf(buf, bufSize, "%s%s", p, desc);
    }
}

inline void formatFaultReason(char (&buf)[128], const char* prefix, uint32_t code) noexcept {
    formatFaultReason(buf, sizeof(buf), prefix, code);
}

template <size_t N>
inline void formatFaultReason(char (&buf)[N], const char* prefix, uint32_t code) noexcept {
    formatFaultReason(buf, N, prefix, code);
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

// Leaf function: NO local variables with destructors, strictly complying with MSVC C2712
inline bool sehTrampoline(void (*fnPtr)(void*) noexcept, void* fnCtx, DWORD* outExCode) noexcept {
    __try {
        fnPtr(fnCtx);
        return true;
    }
    __except (detail::filterSehException(GetExceptionCode(), outExCode)) {
        if (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW) {
            _resetstkoflw();
        }
        return false;
    }
}

inline bool sehExecuteLeaf(void (*fn)(void*) noexcept, void* arg, DWORD* outCode) noexcept {
    return sehTrampoline(fn, arg, outCode);
}

#else

struct CrashContext {
    jmp_buf jmpBuf;
    CrashContext* prev{nullptr};
    DWORD exceptionCode{0};
    void* faultAddress{nullptr};
    bool active{false};
};

static_assert(std::is_trivially_destructible_v<CrashContext>, "CrashContext must have trivial destructor for C-trampoline longjmp safety");

inline thread_local CrashContext* t_currentCrashContext = nullptr;
inline PVOID g_vehHandle = nullptr;
inline std::once_flag g_vehInitOnce;

inline LONG WINAPI vehExceptionHandler(PEXCEPTION_POINTERS ep) {
    if (!ep || !ep->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;

    // Explicitly pass non-hardware C++ exceptions through
    if (code == 0xE06D7363) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    CrashContext* ctx = t_currentCrashContext;
    if (!ctx || !ctx->active) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (isCrashException(code)) {
        ctx->exceptionCode = code;
        ctx->faultAddress = ep->ExceptionRecord->ExceptionAddress;
        // Do NOT call _resetstkoflw() inside vehExceptionHandler before longjmp!
        // Instead, invoke _resetstkoflw() immediately AFTER setjmp(ctx.jmpBuf) != 0
        // on the restored stack frame when ctx.exceptionCode == EXCEPTION_STACK_OVERFLOW.
        longjmp(ctx->jmpBuf, 1);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

inline void ensureVehRegistered() noexcept {
    std::call_once(g_vehInitOnce, []() {
        // Register with First=1 to intercept hardware crashes before external handlers
        g_vehHandle = AddVectoredExceptionHandler(1, vehExceptionHandler);
    });
}

// Pure C-compatible trampoline function containing zero C++ objects with non-trivial destructors (F-09)
inline bool sehTrampoline(void (*fnPtr)(void*) noexcept, void* fnCtx, DWORD* outExCode) noexcept {
    ensureVehRegistered();

    CrashContext ctx;
    ctx.prev = t_currentCrashContext;
    ctx.exceptionCode = 0;
    ctx.faultAddress = nullptr;
    ctx.active = true;
    t_currentCrashContext = &ctx;

    if (setjmp(ctx.jmpBuf) == 0) {
        fnPtr(fnCtx);
        ctx.active = false;
        t_currentCrashContext = ctx.prev;
        return true;
    } else {
        // Restored stack frame after hardware exception unwinding
        ctx.active = false;
        t_currentCrashContext = ctx.prev;
        if (ctx.exceptionCode == EXCEPTION_STACK_OVERFLOW) {
            _resetstkoflw();
        }
        if (outExCode) {
            *outExCode = ctx.exceptionCode;
        }
        return false;
    }
}

inline bool vehLeafInvoke(void (*fn)(void*) noexcept, void* ctxArg, DWORD* outExceptionCode) noexcept {
    return sehTrampoline(fn, ctxArg, outExceptionCode);
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
    auto trampoline = [](void* context) noexcept {
        auto* fn = static_cast<std::remove_reference_t<Func>*>(context);
        (*fn)();
    };
    return detail::sehTrampoline(trampoline, const_cast<void*>(static_cast<const void*>(&f)), outExceptionCode);
}

template <typename Func>
inline bool safeCallPluginAudio(Func&& f, DWORD* outExceptionCode = nullptr) {
    bool ok = safeCallPlugin(std::forward<Func>(f), outExceptionCode);
    if (!ok) {
        // Restore standard audio MXCSR control word after fault
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
        _mm_setcsr(0x1F80 | 0x8000 | 0x0040); // Standard masks + FTZ + DAZ
#endif
    }
    return ok;
}

template <typename Func>
inline bool safeCallPluginGui(Func&& f, DWORD* outExceptionCode = nullptr) {
    return safeCallPlugin(std::forward<Func>(f), outExceptionCode);
}

} // namespace praccy::plugins
