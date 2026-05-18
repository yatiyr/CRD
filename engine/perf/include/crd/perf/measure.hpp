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
// type — it's a hard contract, not a soft hint.
// ---------------------------------------------------------------------------

#include <crd/core/assert.hpp>
#include <crd/core/types.hpp>
#include <crd/time/clocks.hpp>

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

} // namespace crd::perf

// CRD_PERF_BUDGET_LE: run `lambda`, assert its wall-clock duration is
// <= max_ms. `name_literal` appears in the assert message for triage.
//
// Fires CRD_ASSERT_MSG, which is active in assert-enabled builds
// (Debug, RelWithDebInfo, ASan, Shipping with asserts on). In pure
// Release (asserts off via NDEBUG) the macro compiles to a lambda call
// + a void-cast so the lambda's side effects still run and there is no
// unused-variable warning. For Release-mode budget enforcement, wrap
// `measure_ms(...)` in a Catch2 `CHECK` instead.
//
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage): macro is the right tool
// here because it captures the `name_literal` as compile-time text for
// the assert message AND the `lambda` for direct evaluation; a template
// function couldn't fold the name in cleanly.
#define CRD_PERF_BUDGET_LE(name_literal, max_ms, lambda)                                                \
    do                                                                                                   \
    {                                                                                                    \
        const double _crd_dur_ms = ::crd::perf::measure_ms(lambda);                                     \
        (void)_crd_dur_ms;                                                                               \
        CRD_ASSERT_MSG(_crd_dur_ms <= (max_ms),                                                          \
                       "perf budget exceeded: " name_literal);                                           \
    } while (false)
