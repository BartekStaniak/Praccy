#pragma once

#include <windows.h>
#include <cstdint>
#include <csetjmp>
#include <type_traits>
#include <atomic>
#include <mutex>
#include <string>
#include <malloc.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

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

// Leaf function: NO local variables with destructors, strictly complying with MSVC C2712
inline bool sehExecuteLeaf(void (*fn)(void*), void* arg, DWORD* outCode) {
    __try {
        fn(arg);
        return true;
    }
    __except (detail::filterSehException(GetExceptionCode(), outCode)) {
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
    return detail::sehExecuteLeaf(trampoline, const_cast<void*>(static_cast<const void*>(&f)), outExceptionCode);
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
