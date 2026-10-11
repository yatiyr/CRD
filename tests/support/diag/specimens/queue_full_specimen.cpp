// Queue-full positive control (DIAG.10c): a job pushed into a full injection queue, or a pinned job pushed into a
// full pinned lane, must fail as a distinct, always-on, counted report, never a silent drop. Before DIAG.10c both
// were guarded by CRD_ASSERT_MSG alone: with asserts compiled out the job was dropped (the pinned slot overwritten),
// its counter never reached zero and its waiter hung.
//
// Mode `injection`: one thread (no worker drains anything), an injection capacity of 2 and three run() calls; the
// third push finds the Normal queue full. Mode `pinned`: one thread, a pinned lane of 2 and three jobs pinned to
// thread 0; the third finds the lane full. The installed assert platform handler, on the expected message, reads
// crd::jobs::pressure_snapshot(), judges the window since init with the crd-core pressure probe, prints the report
// line as CRD_DIAG_PRESSURE=... and exits 42 only if the window is Exhausted with exactly one refusal and the
// resource held at its bound (60 otherwise, 97 for a different assert or mode). If no fatal fires, control falls
// through to a clean exit 0, which the test rejects.

#define CRD_DIAG_SPECIMEN_ROUTE_ABSENT // no sanitizer involved: this is a scheduler-budget control

#include "specimen_common.hpp"

#include <crd/core/assert.hpp>
#include <crd/core/budget_pressure.hpp>
#include <crd/jobs/jobs.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
// Which budget the mode exhausts, and the reading taken right after init().
bool               g_pinned_mode = false;
crd::BudgetReading g_before{};

const crd::BudgetReading& pick(const crd::jobs::JobsPressure& p) noexcept
{
    return g_pinned_mode ? p.pinned : p.injection[1]; // the Normal lane
}

int queue_full_assert_handler(const char* formatted_message)
{
    const char* const expected = g_pinned_mode ? "pinned lane full" : "injection queue full";
    if (formatted_message != nullptr && std::strstr(formatted_message, expected) != nullptr)
    {
        const crd::jobs::JobsPressure p     = crd::jobs::pressure_snapshot();
        const crd::BudgetReading&     after = pick(p);
        const crd::PressureReport     rep   = crd::pressure_report(g_before, after);
        char                          line[160] = {};
        (void)crd::format_pressure_report(g_pinned_mode ? "pinned" : "injection.normal", rep, after, line,
                                          sizeof(line));
        std::printf("CRD_DIAG_PRESSURE=%s\n", line);
        std::fflush(stdout);
        const bool counted = rep.verdict == crd::PressureVerdict::Exhausted && rep.refused == 1U
                             && after.in_use == after.capacity;
        std::_Exit(counted ? 42 : 60);
    }
    std::_Exit(97);
}

void noop_job(void* /*data*/) noexcept {}
} // namespace

int main(int argc, char** argv)
{
    crd_diag_harden();
    if (argc < 2 || (std::strcmp(argv[1], "injection") != 0 && std::strcmp(argv[1], "pinned") != 0))
    {
        std::printf("usage: crd-diag-queue-full-specimen injection|pinned\n");
        return 97;
    }
    g_pinned_mode = std::strcmp(argv[1], "pinned") == 0;
    crd::set_assert_platform_handler(&queue_full_assert_handler);
    crd_diag_announce();

    crd::jobs::Config cfg;
    cfg.num_threads              = 1U; // thread 0 only, which never pumps: nothing drains a queue
    cfg.injection_queue_capacity = 2U;
    cfg.pinned_queue_capacity    = 2U;
    crd::jobs::init(cfg);
    g_before = pick(crd::jobs::pressure_snapshot());

    crd::jobs::JobDecl j{};
    j.fn         = &noop_job;
    j.pin_thread = g_pinned_mode ? 0 : -1;
    for (int i = 0; i < 3; ++i) // the third push finds the queue full
    {
        (void)crd::jobs::run(j);
    }

    // Failure path only: the guard did not fire (the job was dropped or overwritten). Exit clean so the test, which
    // demands 42, fails; the counters stay held, so no shutdown is attempted.
    return 0;
}
