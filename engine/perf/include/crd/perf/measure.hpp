#pragma once

// ---------------------------------------------------------------------------
// crd::perf::measure_ms + CRD_PERF_BUDGET_LE — wall-clock measurement
// helpers for test/DoD perf-budget assertions.
//
// Phase 3.1.7 v9-prereq-test-harness (2026-05-18). Pairs with crd-perf
// region instrumentation — `measure_ms(fn)` is a lambda-form wall-clock
// that doesn't require crd-perf to be enabled (CRD_PERF_ENABLED=0 still
// returns valid timings). For per-slice DoD perf assertions, use
// `CRD_PERF_BUDGET_LE(name, max_ms, lambda)` which fails the test if
// the lambda's wall-clock duration exceeds max_ms.
//
// Pattern:
//
//   const double dur_ms = crd::perf::measure_ms([&]{
//       // ... GPU dispatch + fence wait ...
//   });
//   CHECK(dur_ms <= 8.0);
//
// Or with the macro (logs the name on failure):
//
//   CRD_PERF_BUDGET_LE("lbvh_1m_prims", 8.0, [&]{
//       // ... 1M-primitive LBVH build ...
//   });
//
// The macro asserts via CRD_ASSERT_MSG so it fires regardless of build
// type — it's a hard contract on local dev hardware.
//
// SOFT MODE (CI): an absolute-millisecond budget calibrated on dev hardware is
// not a reliable correctness gate on a heterogeneous, shared CI runner matrix
// (lower clocks, contended memory bandwidth, first-touch page faults inside the
// timed region). When `CRD_PERF_BUDGET_SOFT` or the standard `CI` env var is
// set, an over-budget result logs a warning to stderr instead of asserting, so
// CI hardware variance never SIGILLs the suite. The lambda — including any
// Catch2 REQUIRE/CHECK inside it — still runs in both modes, so correctness is
// always enforced; only the timing gate softens. Local dev (neither var set)
// keeps the hard assert. → docs/lessons/03-measuring-performance-correctly.md.
// ---------------------------------------------------------------------------

#include <crd/core/assert.hpp>
#include <crd/core/platform.hpp>  // CRD_OS_WINDOWS for the env-read split
#include <crd/core/types.hpp>
#include <crd/time/clocks.hpp>

#include <cstddef>  // std::size_t for _dupenv_s
#include <cstdio>   // std::fprintf for the soft-mode warning
#include <cstdlib>  // std::getenv / _dupenv_s + std::free for the soft-mode env check
#include <utility>

namespace crd::perf
{

// Returns wall-clock duration of `fn()` in milliseconds (f64).
// Uses MonotonicClock — immune to wall-clock adjustments.
template <typename F>
[[nodiscard]] inline double measure_ms(F&& fn)
{
    const auto t0 = crd::time::MonotonicClock::now();
    std::forward<F>(fn)();
    const auto t1 = crd::time::MonotonicClock::now();
    const auto dur = t1 - t0;
    // Duration is crd::time::Duration = Quantity<dim::Time, f64> in seconds.
    return dur.value * 1000.0;
}

namespace detail
{
// Presence check for an env var. MSVC deprecates std::getenv under /WX (and
// clang-cl under -Werror=deprecated-declarations) → use _dupenv_s, same pattern
// as engine/platform/src/context.cpp.
[[nodiscard]] inline bool env_present(const char* name) noexcept
{
#if CRD_OS_WINDOWS
    char*         value   = nullptr;
    std::size_t   len     = 0;
    const errno_t rc      = _dupenv_s(&value, &len, name);
    const bool    present = (rc == 0) && (value != nullptr);
    std::free(value);
    return present;
#else
    return std::getenv(name) != nullptr;
#endif
}
} // namespace detail

// True when perf budgets should be SOFT — log a warning instead of hard-
// asserting. Enabled by `CRD_PERF_BUDGET_SOFT` (explicit Cerid control, set by
// the CI workflow) or the standard `CI` env var (so it works out-of-the-box on
// any CI provider). Read live (not cached) so a test can toggle it via setenv;
// the env-read cost is irrelevant for the handful of perf-budget call sites.
[[nodiscard]] inline bool perf_budgets_are_soft() noexcept
{
    return detail::env_present("CRD_PERF_BUDGET_SOFT") || detail::env_present("CI");
}

// Emit the soft-mode over-budget warning to stderr (visible under ctest
// --output-on-failure). Dependency-free on purpose — measure.hpp stays a
// lightweight header with no crd-log edge.
inline void report_budget_exceeded_soft(const char* name, double dur_ms, double max_ms) noexcept
{
    std::fprintf(stderr,
                 "[perf-budget][SOFT] %s took %.3f ms > %.3f ms budget — NOT failing "
                 "(CRD_PERF_BUDGET_SOFT / CI set; hardware-variance tolerated)\n",
                 name, dur_ms, max_ms);
}

} // namespace crd::perf

// CRD_PERF_BUDGET_LE: run `lambda`, then enforce its wall-clock duration is
// <= max_ms. `name_literal` appears in the message for triage.
//
// HARD on local dev: fires CRD_ASSERT_MSG (active in Debug / RelWithDebInfo /
// ASan / Shipping-with-asserts). SOFT in CI: when `perf_budgets_are_soft()`
// (CRD_PERF_BUDGET_SOFT or CI env set), an over-budget result logs a stderr
// warning instead of asserting — dev-box-calibrated ms budgets are not a
// correctness gate on heterogeneous CI hardware. The `lambda` (and any Catch2
// REQUIRE/CHECK inside it) ALWAYS runs in both modes, so correctness is always
// enforced; only the timing gate softens. In pure Release (asserts off via
// NDEBUG, hard path) the assert compiles out but the lambda still runs.
//
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): macro is the right tool
// here because it captures the `name_literal` as compile-time text for
// the message AND the `lambda` for direct evaluation; a template
// function couldn't fold the name in cleanly.
#define CRD_PERF_BUDGET_LE(name_literal, max_ms, lambda)                                                \
    do                                                                                                   \
    {                                                                                                    \
        const double _crd_dur_ms = ::crd::perf::measure_ms(lambda);                                     \
        if (::crd::perf::perf_budgets_are_soft())                                                        \
        {                                                                                                \
            if (_crd_dur_ms > (max_ms))                                                                  \
            {                                                                                            \
                ::crd::perf::report_budget_exceeded_soft(name_literal, _crd_dur_ms, (max_ms));           \
            }                                                                                            \
        }                                                                                                \
        else                                                                                             \
        {                                                                                                \
            CRD_ASSERT_MSG(_crd_dur_ms <= (max_ms), "perf budget exceeded: " name_literal);              \
        }                                                                                                \
    } while (false)
