// Fiber-pool exhaustion positive control: dispatching more concurrent jobs than the fiber tier holds must fail
// as a DISTINCT, always-on report -- not a silent drop. Before this hardening, run_job_in_fiber received a
// nullptr from an exhausted FiberPool::acquire and returned early, DROPPING the popped job: its counter never
// decremented, so the work vanished and any watchdog would mislabel the stuck state ExecutorStarved. Now the
// exhaustion path bumps FiberPool's event tally and fires CRD_FATAL (always active, unlike the old no-op-in-
// Release CRD_ASSERT_MSG).
//
// With small_fiber_count == 1 and several small-stack jobs that spin forever (holding their fiber), the first
// dispatched job takes the one fiber and a second worker's dispatch finds the tier empty -> CRD_FATAL. The
// installed handler reads the tally through progress_snapshot() and exits 42 only if it surfaced (60 otherwise,
// 97 for a different assert). The fatal fires on a worker thread and _Exit terminates the process directly.

#define CRD_DIAG_SPECIMEN_SANITIZER "none" // no sanitizer involved: this is a scheduler-liveness control

#include "specimen_common.hpp"

#include <crd/core/assert.hpp>
#include <crd/jobs/jobs.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace
{
int fiber_exhaustion_assert_handler(const char* formatted_message)
{
    if (formatted_message != nullptr && std::strstr(formatted_message, "fiber pool exhausted") != nullptr)
    {
        const crd::u32 ex = crd::jobs::progress_snapshot().exhaustions;
        std::printf("CRD_DIAG_EXHAUSTIONS=%u\n", ex);
        std::fflush(stdout);
        std::_Exit(ex >= 1U ? 42 : 60); // 42 = exhaustion counted+surfaced; 60 = message fired but count absent
    }
    std::_Exit(97); // a different assert fired first -- distinguishable from the expected one
}

// Spins forever so the fiber this job holds is never released -- keeping the tier exhausted for the next
// dispatch. The process is terminated by the fatal handler before this ever needs to end.
void spin_forever(void* /*data*/) noexcept
{
    while (true)
        std::this_thread::yield();
}
} // namespace

int main()
{
    crd_diag_harden();
    crd::set_assert_platform_handler(&fiber_exhaustion_assert_handler);
    crd_diag_announce();

    crd::jobs::Config cfg;
    cfg.num_threads        = 3U; // main + two background workers, so two dispatches race for the one fiber
    cfg.small_fiber_count  = 1U; // one small fiber; the second small-stack dispatch exhausts the tier
    cfg.medium_fiber_count = 1U; // tiers must be > 0 (init asserts); only the small tier is exercised
    cfg.large_fiber_count  = 1U;
    crd::jobs::init(cfg);

    // Several small-stack spinners: the first to dispatch takes the one fiber and holds it; a concurrent
    // dispatch then finds the small tier empty -> CRD_FATAL -> _Exit(42) from that worker thread.
    for (int i = 0; i < 4; ++i)
    {
        crd::jobs::JobDecl j{};
        j.fn    = &spin_forever;
        j.stack = crd::jobs::StackSize::Small;
        (void)crd::jobs::run(j);
    }

    // Failure path only: the guard did not fire. Exit clean so the test (which demands 42) fails loudly rather
    // than the process hanging until the harness timeout.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::_Exit(0);
}
