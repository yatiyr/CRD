// Counter-pool exhaustion positive control: acquiring more counters than max_counters must fail as a DISTINCT,
// always-on report -- not a silent nullptr the caller dereferences. run() acquires one counter per call, so
// with max_counters == 1 the second run() exhausts the pool. The exhaustion path bumps CounterPool's event
// tally and then fires CRD_FATAL (always active regardless of CRD_ENABLE_ASSERTS, unlike the old CRD_ASSERT_MSG
// which is a no-op in Release). This specimen installs an assert platform handler (like observer_swap_specimen)
// that, on the exhaustion message, reads the tally back through progress_snapshot() and exits 42 only if it was
// actually surfaced (60 if the message fired but the count did not surface, 97 for a different assert). If no
// fatal fires at all, control falls through to a clean exit 0 -- which the test rejects, so a broken guard can
// never pass.

#define CRD_DIAG_SPECIMEN_SANITIZER "none" // no sanitizer involved: this is a scheduler-liveness control

#include "specimen_common.hpp"

#include <crd/core/assert.hpp>
#include <crd/jobs/jobs.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
int counter_exhaustion_assert_handler(const char* formatted_message)
{
    if (formatted_message != nullptr && std::strstr(formatted_message, "CounterPool exhausted") != nullptr)
    {
        const crd::u32 ex = crd::jobs::progress_snapshot().exhaustions;
        std::printf("CRD_DIAG_EXHAUSTIONS=%u\n", ex);
        std::fflush(stdout);
        std::_Exit(ex >= 1U ? 42 : 60); // 42 = exhaustion counted+surfaced; 60 = message fired but count absent
    }
    std::_Exit(97); // a different assert fired first -- distinguishable from the expected one
}

void noop_job(void* /*data*/) noexcept {}
} // namespace

int main()
{
    crd_diag_harden();
    crd::set_assert_platform_handler(&counter_exhaustion_assert_handler);
    crd_diag_announce();

    crd::jobs::Config cfg;
    cfg.num_threads = 1U;  // run() acquires a counter synchronously; no worker needs to run the jobs
    cfg.max_counters = 1U; // exactly one counter -> the second run() exhausts the pool
    crd::jobs::init(cfg);

    crd::jobs::JobDecl j{};
    j.fn = &noop_job;

    (void)crd::jobs::run(j); // acquires the one counter (not waited, so it is not released)
    (void)crd::jobs::run(j); // the violation: no counter left -> CRD_FATAL, turned into _Exit(42) by the handler

    // Failure path only: the guard did not fire. Exit clean so the test (which demands 42) fails loudly.
    crd::jobs::shutdown();
    return 0;
}
