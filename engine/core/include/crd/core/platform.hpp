#pragma once

// Compile-time platform, compiler, and architecture detection.
//
// This header is intentionally macro-heavy because the rest of the engine
// needs these values in preprocessor conditionals as well as normal code.
// The three string helpers at the bottom provide a runtime-readable view of
// the same information for logs and smoke tests.

/// 1 when the active compiler is MSVC; 0 otherwise.
/// @note clang-cl defines both _MSC_VER and __clang__, so clang-cl resolves as CLANG, not MSVC.
#if defined(_MSC_VER)
#define CRD_COMPILER_MSVC 1
#define CRD_COMPILER_GCC 0
#define CRD_COMPILER_CLANG 0
#elif defined(__clang__)
#define CRD_COMPILER_MSVC 0
#define CRD_COMPILER_GCC 0
/// 1 when the active compiler is Clang or clang-cl; 0 otherwise.
#define CRD_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define CRD_COMPILER_MSVC 0
/// 1 when the active compiler is GCC; 0 otherwise.
#define CRD_COMPILER_GCC 1
#define CRD_COMPILER_CLANG 0
#else
#error "Unsupported compiler!"
#endif

/// 1 when the target OS is Windows; 0 otherwise.
#if defined(_WIN32)
#define CRD_OS_WINDOWS 1
#define CRD_OS_LINUX 0
#define CRD_OS_MAC 0
#elif defined(__linux__)
#define CRD_OS_WINDOWS 0
/// 1 when the target OS is Linux; 0 otherwise.
#define CRD_OS_LINUX 1
#define CRD_OS_MAC 0
#elif defined(__APPLE__)
#define CRD_OS_WINDOWS 0
#define CRD_OS_LINUX 0
/// 1 when the target OS is macOS; 0 otherwise.
#define CRD_OS_MAC 1
#else
#error "Unsupported OS!"
#endif

/// 1 when the target architecture is x86-64; 0 otherwise.
#if defined(_M_X64) || defined(__x86_64__)
#define CRD_ARCH_X64 1
#define CRD_ARCH_ARM64 0
#elif defined(_M_ARM64) || defined(__aarch64__)
#define CRD_ARCH_X64 0
/// 1 when the target architecture is AArch64 (ARM64); 0 otherwise.
#define CRD_ARCH_ARM64 1
#else
#error "Unsupported architecture!"
#endif

/// Trigger an immediate debugger break (int3 on x64). In a non-debug session this raises SIGTRAP / STATUS_BREAKPOINT.
#if CRD_COMPILER_MSVC
#define CRD_DEBUGBREAK() __debugbreak()
#else
#define CRD_DEBUGBREAK() __builtin_trap()
#endif

/// Hint the compiler to inline the function at every call site, overriding its normal heuristic.
#if CRD_COMPILER_MSVC
#define CRD_FORCEINLINE __forceinline
/// Prevent the compiler from inlining the function, even under LTO.
#define CRD_NOINLINE __declspec(noinline)
#else
#define CRD_FORCEINLINE __attribute__((always_inline)) inline
/// Prevent the compiler from inlining the function, even under LTO.
#define CRD_NOINLINE __attribute__((noinline))
#endif

/// Hint that `x` is true in the common case; may improve branch-prediction layout on GCC/Clang.
#if CRD_COMPILER_MSVC
#define CRD_LIKELY(x) (x)
/// Hint that `x` is false in the common case; may improve branch-prediction layout on GCC/Clang.
#define CRD_UNLIKELY(x) (x)
#else
#define CRD_LIKELY(x) __builtin_expect(!!(x), 1)
/// Hint that `x` is false in the common case; may improve branch-prediction layout on GCC/Clang.
#define CRD_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

/// Marks a symbol for export from a shared library (DLL on Windows, default visibility on ELF).
#if CRD_OS_WINDOWS
#define CRD_API_EXPORT __declspec(dllexport)
/// Marks a symbol as imported from a shared library (DLL on Windows; no-op on ELF — default visibility handles it).
#define CRD_API_IMPORT __declspec(dllimport)
#else
#define CRD_API_EXPORT __attribute__((visibility("default")))
/// Marks a symbol as imported from a shared library (no-op on ELF targets).
#define CRD_API_IMPORT
#endif

namespace crd
{
/// Returns a stable null-terminated string describing the active target OS ("Windows", "Linux", or "macOS").
const char* platform_name();
/// Returns a stable null-terminated string describing the active compiler toolchain ("MSVC", "Clang", or "GCC").
const char* compiler_name();
/// Returns a stable null-terminated string describing the active CPU architecture ("x86-64" or "AArch64").
const char* arch_name();
} // namespace crd
