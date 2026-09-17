#pragma once

#include <crd/core/build_config.hpp>
#include <crd/core/platform.hpp>
#include <crd/core/types.hpp>

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

/// Declare that this process is headless/non-interactive (a server, a CI job, a spawned test child).
///
/// When set, the DEFAULT assert path (no platform handler installed) must never show a modal dialog and must never
/// silently ignore: it writes the failure evidence and terminates promptly via std::abort(). abort() raises SIGABRT,
/// which crd's crash handler captures on Linux (routing the failure to the emergency channel); on Windows it
/// terminates promptly with the evidence already emitted. A platform handler, if installed, still takes precedence
/// over this. Off by default (interactive: the Windows MessageBox / the debugger break is preserved). An installed
/// platform handler is unaffected. This is a declared mode, not auto-detected — headless contexts opt in (the test
/// harness's crd_diag_harden() sets it; a shipping headless app sets it at startup).
void set_assert_headless(bool headless) noexcept;
/// Return whether headless mode is set (default false).
bool get_assert_headless() noexcept;

/// Record (file, line) as an ignored assert site: a later report_assert_failure() at the same site returns 0
/// (ignore) without reaching the handler. This is the programmatic form of the interactive "Ignore" button. The
/// table is bounded to 256 sites as a FIFO ring — recording a 257th distinct site evicts the oldest (which will
/// then fire again), rather than silently refusing to record. `file` must have static lifetime (a __FILE__ literal;
/// compared by pointer). Returns true (the site is now recorded). Thread-safe. NOTE: a headless build never reaches
/// the ignore table (it terminates before the dialog); this is interactive/opt-in behaviour. See DIAG.5c(f).
bool ignore_assert_site(const char* file, int line) noexcept;

/// Number of times the ignore table was full (256 sites) and evicted its oldest entry. Non-zero means ignore churn
/// exceeded the table and evicted sites will fire again — the defined, bounded behaviour under many distinct
/// ignored sites (replacing the previous silent drop). See DIAG.5c(f).
u64 assert_ignore_eviction_count() noexcept;
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


/// "This branch should be unreachable" — debug-only assert.
///
/// Reports `msg` and breaks into the debugger when CRD_ENABLE_ASSERTS is set; no-op in Release.
/// Use at unreachable switch defaults / programming-error branches where the runtime path
/// either has a recovery (skip/continue/early-return) or genuinely cannot be reached.
///
/// Prefer over `CRD_ASSERT(false && "...")` — the latter trips MSVC C4127 (constant
/// conditional) under /WX since the `if (!false)` test inside CRD_ASSERT folds to a constant.
/// This macro has no constant test, so it is C4127-clean across MSVC / clang-cl / GCC.
#if CRD_ENABLE_ASSERTS
#define CRD_ASSERT_UNREACHABLE(msg)                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (::crd::detail::report_assert_failure("UNREACHABLE", __FILE__, __LINE__, msg) == 2)                         \
        {                                                                                                              \
            CRD_DEBUGBREAK();                                                                                          \
        }                                                                                                              \
    }                                                                                                                  \
    CRD_WHILE_FALSE
#else
#define CRD_ASSERT_UNREACHABLE(msg) ((void)0)
#endif
