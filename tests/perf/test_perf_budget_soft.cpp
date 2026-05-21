// crd-perf -- CRD_PERF_BUDGET_LE soft-mode escape hatch (2026-05-21).
//
// On a heterogeneous CI runner matrix an absolute-millisecond budget calibrated
// on dev hardware is not a reliable correctness gate (lower clocks, contended
// memory bandwidth, first-touch page faults inside the timed region). Soft mode
// (CRD_PERF_BUDGET_SOFT or the standard CI env var) downgrades the hard
// CRD_ASSERT to a stderr warning so CI hardware variance never SIGILLs the
// suite, while the measured lambda + any inner REQUIRE still run.

#include <catch2/catch_test_macros.hpp>

#include <crd/core/platform.hpp>
#include <crd/perf/measure.hpp>

#include <cstdlib>

namespace
{
void set_env(const char* name, const char* value)
{
#if CRD_OS_WINDOWS
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void clear_env(const char* name)
{
#if CRD_OS_WINDOWS
    _putenv_s(name, "");  // empty value removes the variable on Windows
#else
    unsetenv(name);
#endif
}
} // namespace

TEST_CASE("perf budget: CRD_PERF_BUDGET_SOFT marks budgets soft", "[perf][budget]")
{
    set_env("CRD_PERF_BUDGET_SOFT", "1");
    CHECK(crd::perf::perf_budgets_are_soft());
    clear_env("CRD_PERF_BUDGET_SOFT");
}

TEST_CASE("perf budget: soft mode downgrades an impossible budget to a warning, never aborts",
          "[perf][budget]")
{
    set_env("CRD_PERF_BUDGET_SOFT", "1");
    REQUIRE(crd::perf::perf_budgets_are_soft());

    // A 0 ms budget is unsatisfiable -> CRD_ASSERT (abort / SIGILL) in HARD
    // mode. In SOFT mode the macro must only warn and fall through. Reaching
    // the assignment after the macro is the proof it did not abort the process.
    bool reached_after = false;
    CRD_PERF_BUDGET_LE("soft_mode_unit_test", 0.0, [&] {
        volatile double acc = 0.0;
        for (int i = 0; i < 100000; ++i)
        {
            acc += static_cast<double>(i);
        }
        (void)acc;
    });
    reached_after = true;
    CHECK(reached_after);

    clear_env("CRD_PERF_BUDGET_SOFT");
}

TEST_CASE("perf budget: soft mode still RUNS the measured lambda (correctness preserved)",
          "[perf][budget]")
{
    set_env("CRD_PERF_BUDGET_SOFT", "1");

    // Inner REQUIRE/side-effects must execute in soft mode -- only the timing
    // gate softens, never the work being measured.
    int side_effect = 0;
    CRD_PERF_BUDGET_LE("soft_mode_runs_lambda", 0.0, [&] {
        side_effect = 42;
        REQUIRE(side_effect == 42);
    });
    CHECK(side_effect == 42);

    clear_env("CRD_PERF_BUDGET_SOFT");
}
