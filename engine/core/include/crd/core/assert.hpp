#pragma once

#include <crd/core/build_config.hpp>
#include <crd/core/platform.hpp>

namespace crd
{
// Optional callback fired from inside report_assert_failure() BEFORE the
// platform error UI is shown. Used by crd-log to emit a Critical record
// so the failure makes it into the log file. The handler must be
// re-entrant safe (a critical inside a critical is allowed but discouraged)
// and must NOT throw.
//
// Lifetime: handler is a bare function pointer; caller is responsible for
// ensuring the target function outlives any possible assert. Setting to
// nullptr disables the callback (default behaviour).
using AssertHandler = void (*)(const char* expression, const char* file, int line, const char* message);

// Optional platform-UI hook used at the tail of report_assert_failure().
// Tests can replace the default MessageBox path with a no-op handler so a
// real CRD_ASSERT(false) can run end-to-end without blocking the runner.
// Return value uses the same contract as the Windows dialog path:
//   0 = Ignore, 2 = Retry / break.
using AssertPlatformHandler = int (*)(const char* formatted_message);

// Install/remove the optional assert->subsystem bridge callback.
void set_assert_handler(AssertHandler h) noexcept;
// Return the currently installed bridge callback, or nullptr.
AssertHandler get_assert_handler() noexcept;
// Override the final platform UI step (MessageBox on Windows).
void set_assert_platform_handler(AssertPlatformHandler h) noexcept;
// Return the currently installed platform UI hook, or nullptr.
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
// No static locals — safe to use inside constexpr functions (C++20).
// Per-site ignore state is tracked inside report_assert_failure() itself.
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
#define CRD_VERIFY(expr) CRD_ASSERT(expr)
#else
#define CRD_VERIFY(expr) ((void)(expr))
#endif

// CRD_FATAL will always be active
#define CRD_FATAL(msg)                                                                                                 \
    do                                                                                                                 \
    {                                                                                                                  \
        ::crd::detail::report_assert_failure("FATAL", __FILE__, __LINE__, msg);                                        \
        CRD_DEBUGBREAK();                                                                                              \
    }                                                                                                                  \
    CRD_WHILE_FALSE
