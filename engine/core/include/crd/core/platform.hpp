#pragma once

// Compile-time platform, compiler, and architecture detection.
//
// This header is intentionally macro-heavy because the rest of the engine
// needs these values in preprocessor conditionals as well as normal code.
// The three string helpers at the bottom provide a runtime-readable view of
// the same information for logs and smoke tests.

// ------ Define compiler macros ----
#if defined(_MSC_VER)
#define CRD_COMPILER_MSVC 1
#define CRD_COMPILER_GCC 0
#define CRD_COMPILER_CLANG 0
#elif defined(__clang__)
#define CRD_COMPILER_MSVC 0
#define CRD_COMPILER_GCC 0
#define CRD_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define CRD_COMPILER_MSVC 0
#define CRD_COMPILER_GCC 1
#define CRD_COMPILER_CLANG 0
#else
#error "Unsupported compiler!"
#endif

// --- Define OS macros --------------
#if defined(_WIN32)
#define CRD_OS_WINDOWS 1
#define CRD_OS_LINUX 0
#define CRD_OS_MAC 0
#elif defined(__linux__)
#define CRD_OS_WINDOWS 0
#define CRD_OS_LINUX 1
#define CRD_OS_MAC 0
#elif defined(__APPLE__)
#define CRD_OS_WINDOWS 0
#define CRD_OS_LINUX 0
#define CRD_OS_MAC 1
#else
#error "Unsupported OS!"
#endif

// --- Define Arch macros --------------
#if defined(_M_X64) || defined(__x86_64__)
#define CRD_ARCH_X64 1
#define CRD_ARCH_ARM64 0
#elif defined(_M_ARM64) || defined(__aarch64__)
#define CRD_ARCH_X64 0
#define CRD_ARCH_ARM64 1
#else
#error "Unsupported architecture!"
#endif

// --- Define debugbreak macros --------
#if CRD_COMPILER_MSVC
#define CRD_DEBUGBREAK() __debugbreak()
#else
#define CRD_DEBUGBREAK() __builtin_trap()
#endif

// --- Define forceinline and noinline macros -------
#if CRD_COMPILER_MSVC
#define CRD_FORCEINLINE __forceinline
#define CRD_NOINLINE __declspec(noinline)
#else
#define CRD_FORCEINLINE __attribute__((always_inline)) inline
#define CRD_NOINLINE __attribute__((noinline))
#endif

// --- Define branch prediction hints ---------------
#if CRD_COMPILER_MSVC
#define CRD_LIKELY(x) (x)
#define CRD_UNLIKELY(x) (x)
#else
#define CRD_LIKELY(x) __builtin_expect(!!(x), 1)
#define CRD_UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

// --- Define api export macros ---------------------
#if CRD_OS_WINDOWS
#define CRD_API_EXPORT __declspec(dllexport)
#define CRD_API_IMPORT __declspec(dllimport)
#else
#define CRD_API_EXPORT __attribute__((visibility("default")))
#define CRD_API_IMPORT
#endif

namespace crd
{
// Returns a stable string describing the active target OS.
const char* platform_name();
// Returns a stable string describing the active compiler toolchain.
const char* compiler_name();
// Returns a stable string describing the active CPU architecture.
const char* arch_name();
} // namespace crd
