#pragma once

#include <crd/core/build_config.hpp>
#include <crd/core/platform.hpp>

namespace crd
{
/// Callback fired inside report_assert_failure() before the platform error UI is shown.
/// Used by crd-log to emit a Critical record so failures reach the log file.
/// Must be re-entrant safe and must NOT throw. nullptr disables the callback (default).
using AssertHandler = void (*)(const char* expression, const char* file, int line, const char* message);

/// Platform-UI hook called at the tail of report_assert_failure().
/// Tests replace the default MessageBox path with a no-op so asserts run end-to-end without blocking.
/// Return 0 to ignore, 2 to break into the debugger (matches the Windows dialog contract).
using AssertPlatformHandler = int (*)(const char* formatted_message);

/// Install the assert-to-subsystem bridge callback (pass nullptr to clear).
void set_assert_handler(AssertHandler h) noexcept;
/// Return the currently installed bridge callback, or nullptr if none.
AssertHandler get_assert_handler() noexcept;
/// Override the final platform UI step (MessageBox on Windows). Pass nullptr to restore the default.
void set_assert_platform_handler(AssertPlatformHandler h) noexcept;
/// Return the currently installed platform UI hook, or nullptr if none.
AssertPlatformHandler get_assert_platform_handler() noexcept;
} // namespace crd

namespace crd::detail
{
int report_assert_failure(const char* expression, const char* file, int line, const char* message = nullptr);
}

#if CRD_COMPILER_MSVC
#define CRD_WHILE_FALSE __pragma(warning(push)) __pragma(warning(disable : 4127)) while (false) __pragma(warning(pop))
#else
#define CRD_WHILE_FALSE while (false)
#endif

#if CRD_ENABLE_ASSERTS
/// Evaluate `expr`; call report_assert_failure() and optionally break if it is false.
/// Active only when CRD_ENABLE_ASSERTS is set (Debug / RelWithDebInfo). No-op in Release.
#define CRD_ASSERT(expr)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (CRD_UNLIKELY(!(expr)))                                                                                     \
        {                                                                                                              \
            if (::crd::detail::report_assert_failure(#expr, __FILE__, __LINE__) == 2)                                  \
            {                                                                                                          \
                CRD_DEBUGBREAK();                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
    }                                                                                                                  \
    CRD_WHILE_FALSE

/// Like CRD_ASSERT but attaches a plain-text `msg` to the failure report.
#define CRD_ASSERT_MSG(expr, msg)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        if (CRD_UNLIKELY(!(expr)))                                                                                     \
        {                                                                                                              \
            if (::crd::detail::report_assert_failure(#expr, __FILE__, __LINE__, msg) == 2)                             \
            {                                                                                                          \
                CRD_DEBUGBREAK();                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
    }                                                                                                                  \
    CRD_WHILE_FALSE
#else
#define CRD_ASSERT(expr) ((void)0)
#define CRD_ASSERT_MSG(expr, msg) ((void)0)
#endif

#if CRD_ENABLE_ASSERTS
/// Evaluate `expr` in all build types. In assert-enabled builds behaves like CRD_ASSERT; in Release the side-effects of `expr` are still executed.
#define CRD_VERIFY(expr) CRD_ASSERT(expr)
#else
#define CRD_VERIFY(expr) ((void)(expr))
#endif

/// Unconditional failure. Always active regardless of CRD_ENABLE_ASSERTS. Reports `msg` and breaks into the debugger.
#define CRD_FATAL(msg)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        ::crd::detail::report_assert_failure("FATAL", __FILE__, __LINE__, msg);                                        \
        CRD_DEBUGBREAK();                                                                                              \
    }                                                                                                                  \
    CRD_WHILE_FALSE
